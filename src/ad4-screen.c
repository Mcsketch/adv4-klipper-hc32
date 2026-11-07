/*
 * ad4-screen - the installer's on-panel UI for the Adventurer 4.
 *
 * The SoC drives an 800x480 16 bpp framebuffer (/dev/fb0) and a TSC2007
 * touchscreen (/dev/input/event0). auto_run.sh runs flashforge_init.sh
 * BEFORE it launches the vendor Qt GUI (ffstartup-arm), so at install time
 * the panel is free and we own it.
 *
 * The controller scans landscape but the LCD is mounted portrait, so all
 * drawing happens in a 480x800 logical canvas that px() rotates into the
 * framebuffer (and ts_read() rotates back). AD4_ROT=0 turns that off.
 *
 * Subcommands (each paints and exits, so the shell script drives it):
 *   menu [--auto TOKEN SECS]   interactive chooser; prints the chosen token
 *   splash                     branded splash, no touch
 *   boot                       boot animation (~4 s), ends on a title frame
 *   confirm TITLE BODY [GO] [SECS]  one GO / CANCEL question; exit 0 = go,
 *                              2 = cancel or timeout (default 120 s)
 *   status TITLE [SUB] [PCT]   progress screen
 *   done   TITLE [SUB]         success screen
 *   fail   TITLE [SUB]         error screen
 *   touch  [SECS]              diagnostic: live crosshair + tap-accuracy targets
 *   calib  [SECS]              four-mark touch calibration, writes AD4_CAL/CAL_PATH
 *   klipper [HOST[:PORT]]      runs forever: Moonraker status, menus, touch controls
 *
 * menu exit codes: 0 = a choice was made, 1 = no touchscreen (caller falls
 * back to the flag files), 2 = the user chose skip/abort.
 *
 * Accepted touches beep on the piezo (ad4_buzzer.h). The completion riff is
 * NOT played here: flashforge_init.sh runs `ad4-buzz ok` for that, so it
 * sounds even on a printer with no panel. `boot` is the exception: it plays
 * the riff itself, in step with the animation.
 *
 * The pixel layout is read from FBIOGET_VSCREENINFO rather than assumed, so
 * the same binary works on RGB565/BGR565/RGB888. The touch axis range comes
 * from EVIOCGABS, likewise not guessed.
 *
 * Build for the SoC:  make -C src   (arm-linux-gnueabi-gcc, static)
 * Host preview (writes a PPM instead of touching hardware):
 *   cc -DAD4_DUMP ad4-screen.c -o /tmp/ad4screen && /tmp/ad4screen menu --dump /tmp/m.ppm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#ifdef __linux__
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <poll.h>
#include <time.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <dirent.h>
#include <strings.h>
#endif

#include "ad4_font8x8.h"
#include "ad4_buzzer.h"

/* ---------------------------------------------------------------- surface */
typedef struct {
    int w, h, stride, bpp;        /* w/h are LOGICAL: portrait 480x800 when rot */
    int fw, fh;                   /* framebuffer geometry: 800x480 on the AD4 */
    int rot;                      /* 1 = panel is portrait, pre-rotate 90 deg CCW */
    int ro, rl, go, gl, bo, bl;   /* red/green/blue bit offset and width */
    unsigned char *mem;
    int owned;                    /* mem was malloc'd (host dump mode) */
} surf;

static unsigned packc(const surf *s, unsigned rgb) {
    unsigned r = (rgb >> 16) & 0xff, g = (rgb >> 8) & 0xff, b = rgb & 0xff;
    r >>= (8 - s->rl); g >>= (8 - s->gl); b >>= (8 - s->bl);
    return (r << s->ro) | (g << s->go) | (b << s->bo);
}

/* The AD4's LCD is mounted portrait while the controller scans 800x480
 * landscape, so everything drawn here shows up turned 90 deg. Verified on the
 * panel: the display shows rotateCCW(fb), so we pre-rotate the other way and
 * it lands upright: fb(x=fw-1-y, y=x). */
static void px(const surf *s, int x, int y, unsigned rgb) {
    if (x < 0 || y < 0 || x >= s->w || y >= s->h) return;
    int fx = x, fy = y;
    if (s->rot) { fx = s->fw - 1 - y; fy = x; }
    unsigned char *p = s->mem + (size_t)fy * s->stride + (size_t)fx * (s->bpp / 8);
    unsigned v = packc(s, rgb);
    if (s->bpp == 16) { unsigned short t = (unsigned short)v; memcpy(p, &t, 2); }
    else { unsigned t = v; memcpy(p, &t, 4); }
}

static void fill(const surf *s, int x, int y, int w, int h, unsigned rgb) {
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++) px(s, i, j, rgb);
}

static void blend_rect(const surf *s, int x, int y, int w, int h,
                       unsigned a, unsigned b) {   /* vertical gradient */
    for (int j = 0; j < h; j++) {
        int t = h > 1 ? j * 256 / (h - 1) : 0;
        unsigned ar = (a >> 16) & 0xff, ag = (a >> 8) & 0xff, ab = a & 0xff;
        unsigned br = (b >> 16) & 0xff, bg = (b >> 8) & 0xff, bb = b & 0xff;
        unsigned c = (((ar + (br - ar) * t / 256) << 16) |
                      ((ag + (bg - ag) * t / 256) << 8) |
                       (ab + (bb - ab) * t / 256));
        fill(s, x, y + j, w, 1, c);
    }
}

static int in_rrect(int x, int y, int w, int h, int r) {
    if (x < 0 || y < 0 || x >= w || y >= h) return 0;
    if (r <= 0) return 1;
    int cx = x, cy = y;
    if (x < r) cx = r; else if (x >= w - r) cx = w - 1 - r;
    if (y < r) cy = r; else if (y >= h - r) cy = h - 1 - r;
    int dx = x - cx, dy = y - cy;
    return dx * dx + dy * dy <= r * r;
}

static void rrect(const surf *s, int x, int y, int w, int h, int r,
                  unsigned top, unsigned bot) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            if (in_rrect(i, j, w, h, r)) {
                int t = h > 1 ? j * 256 / (h - 1) : 0;
                unsigned tr = (top >> 16) & 0xff, tg = (top >> 8) & 0xff, tb = top & 0xff;
                unsigned cr = (bot >> 16) & 0xff, cg = (bot >> 8) & 0xff, cb = bot & 0xff;
                px(s, x + i, y + j,
                   (((tr + (cr - tr) * t / 256) << 16) |
                    ((tg + (cg - tg) * t / 256) << 8) |
                     (tb + (cb - tb) * t / 256)));
            }
}

/* ------------------------------------------------------------------ text */
static int tw(const char *t, int sc) { return (int)strlen(t) * 8 * sc; }

static void text(const surf *s, int x, int y, const char *t, int sc, unsigned fg) {
    for (; *t; t++, x += 8 * sc) {
        int c = (unsigned char)*t;
        if (c < 32 || c > 126) c = '?';
        const unsigned char *g = font8x8[c - 32];
        for (int r = 0; r < 8; r++)
            for (int b = 0; b < 8; b++)          /* bit 0 = leftmost pixel */
                if (g[r] & (1u << b))
                    fill(s, x + b * sc, y + r * sc, sc, sc, fg);
    }
}

static void textc(const surf *s, int cx, int y, const char *t, int sc, unsigned fg) {
    text(s, cx - tw(t, sc) / 2, y, t, sc, fg);
}

/* Left-aligned word wrap at a pixel width; returns the y past the last line.
 * Portrait is only 480 px wide, so the long values (stick inventory) need it. */
static int text_wrap(const surf *s, int x, int y, const char *t, int sc,
                     unsigned fg, int maxw) {
    int maxc = maxw / (8 * sc);
    if (maxc < 1) maxc = 1;
    while (*t) {
        const char *nl = strchr(t, '\n');
        int n = nl ? (int)(nl - t) : (int)strlen(t);
        int take = n < maxc ? n : maxc;
        if (take < n && t[take] != ' ' && t[take] != '-') {
            int k = take;
            while (k > 0 && t[k - 1] != ' ' && t[k - 1] != '-') k--;
            if (k > 0) take = k;
        }
        char buf[256];
        if (take > 255) take = 255;
        memcpy(buf, t, take); buf[take] = 0;
        text(s, x, y, buf, sc, fg);
        y += 12 * sc;
        t += take;
        while (*t == ' ' || *t == '\n') t++;
    }
    return y;
}

/* Centred word wrap (button labels use '\n'); returns the y past the last line. */
static int textc_wrap(const surf *s, int cx, int y, const char *t, int sc,
                      unsigned fg, int lh) {
    const int maxc = (s->w - 48) / (8 * sc);
    while (*t) {
        const char *nl = strchr(t, '\n');
        int n = nl ? (int)(nl - t) : (int)strlen(t);
        int take = (maxc > 0 && n > maxc) ? maxc : n;
        if (take < n && t[take] != ' ' && t[take] != '-') {
            int k = take;
            while (k > 0 && t[k - 1] != ' ' && t[k - 1] != '-') k--;
            if (k > 0) take = k;
        }
        char buf[128];
        if (take > 127) take = 127;
        memcpy(buf, t, take); buf[take] = 0;
        text(s, cx - take * 8 * sc / 2, y, buf, sc, fg);
        y += lh;
        t += take;
        while (*t == ' ' || *t == '\n') t++;
    }
    return y;
}

/* ---------------------------------------------------------------- palette */
#define C_BG0    0x0a1120u
#define C_BG1    0x17253fu
#define C_CARD   0x1b2b45u
#define C_CARD2  0x24374fu
#define C_ACC    0x4d9fffu
#define C_ACCD   0x1d5fb0u
#define C_TXT    0xdce7f7u
#define C_DIM    0x8fa6c4u
#define C_OK     0x35d69au
#define C_WARN   0xf0a848u
#define C_ERR    0xff6b78u
#define C_ERRD   0xb03a44u

static void background(const surf *s) {
    blend_rect(s, 0, 0, s->w, s->h, C_BG0, C_BG1);
    fill(s, 0, 0, s->w, 4, C_ACC);
}

static void header(const surf *s, const char *title, const char *chip,
                   unsigned chipcol) {
    text(s, 24, 24, title, 2, C_TXT);
    int y = 52;
    if (chip && *chip) {
        int w = tw(chip, 2) + 28;
        if (w > s->w - 48) w = s->w - 48;
        rrect(s, 24, y, w, 36, 12, chipcol, chipcol);
        text(s, 38, y + 10, chip, 2, 0x06101fu);
        y += 46;
    }
    fill(s, 24, y, s->w - 48, 1, 0x2e4468u);
}

static void progress(const surf *s, int y, int pct, unsigned col) {
    int x = 24, w = s->w - 48, h = 22;
    rrect(s, x, y, w, h, 11, C_CARD, C_CARD2);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    if (pct > 0) {
        int iw = w * pct / 100;
        if (iw < h) iw = h;
        rrect(s, x, y, iw, h, 11, col, col);
    }
    char p[8];
    snprintf(p, sizeof p, "%d%%", pct);
    textc(s, s->w / 2, y + 3, p, 2, pct > 8 ? 0x06101fu : C_DIM);
}

/* ------------------------------------------------------------- host dump */
#ifdef AD4_DUMP
static void dump_ppm(const surf *s, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    fprintf(f, "P6\n%d %d\n255\n", s->w, s->h);
    for (int y = 0; y < s->h; y++)
        for (int x = 0; x < s->w; x++) {
            const unsigned char *p = s->mem + (size_t)y * s->stride + (size_t)x * (s->bpp / 8);
            unsigned v;
            if (s->bpp == 16) { unsigned short t; memcpy(&t, p, 2); v = t; }
            else { unsigned t; memcpy(&t, p, 4); v = t; }
            unsigned r = ((v >> s->ro) & ((1u << s->rl) - 1));
            unsigned g = ((v >> s->go) & ((1u << s->gl) - 1));
            unsigned b = ((v >> s->bo) & ((1u << s->bl) - 1));
            unsigned char o[3] = {
                (unsigned char)(r << (8 - s->rl)),
                (unsigned char)(g << (8 - s->gl)),
                (unsigned char)(b << (8 - s->bl))
            };
            fwrite(o, 1, 3, f);
        }
    fclose(f);
}
#endif

/* ------------------------------------------------------------- hardware */
#if defined(__linux__) && !defined(AD4_DUMP)
static int fb_open(surf *s) {
    int fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) { perror("/dev/fb0"); return -1; }
    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo f;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &v) < 0) { perror("VSCREENINFO"); close(fd); return -1; }
    if (ioctl(fd, FBIOGET_FSCREENINFO, &f) < 0) { perror("FSCREENINFO"); close(fd); return -1; }
    if (v.bits_per_pixel != 16 && v.bits_per_pixel != 32) {
        fprintf(stderr, "unsupported bpp %d\n", v.bits_per_pixel);
        close(fd); return -1;
    }
    s->fw = v.xres; s->fh = v.yres; s->bpp = v.bits_per_pixel;
    /* AD4_ROT=0 draws straight into the framebuffer, for when the panel is
     * re-cabled or the controller starts reporting portrait itself. */
    s->rot = v.xres > v.yres;
    if (getenv("AD4_ROT") && *getenv("AD4_ROT") == '0') s->rot = 0;
    s->w = s->rot ? s->fh : s->fw;
    s->h = s->rot ? s->fw : s->fh;
    s->stride = f.line_length;
    s->ro = v.red.offset;   s->rl = v.red.length;
    s->go = v.green.offset; s->gl = v.green.length;
    s->bo = v.blue.offset;  s->bl = v.blue.length;
    /* always the HARDWARE extent: stride * fh, never the logical height */
    size_t len = (size_t)s->stride * s->fh;
    void *m = mmap(0, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) { perror("mmap /dev/fb0"); close(fd); return -1; }
    s->mem = m; s->owned = 0;
    return fd;
}
static void fb_close(surf *s, int fd) {
    munmap(s->mem, (size_t)s->stride * s->h);
    close(fd);
}
#else
/* Host build: no hardware, so assume the panel we are targeting. */
static int fb_open(surf *s) {
    s->fw = 800; s->fh = 480; s->rot = 0;   /* host preview: logical portrait as-is */
    s->w = 480; s->h = 800; s->bpp = 16; s->stride = 960;
    s->ro = 11; s->rl = 5; s->go = 5; s->gl = 6; s->bo = 0; s->bl = 5;
    s->mem = calloc(1, (size_t)s->stride * s->h);
    s->owned = 1;
    return s->mem ? 0 : -1;
}
static void fb_close(surf *s, int fd) { (void)fd; if (s->owned) free(s->mem); }
#endif

/* ---------------------------------------------------------------- touch */
/* down outlives a single report: the driver only re-sends BTN_TOUCH on press
 * and release, so mid-drag samples carry axes alone. lx/ly carry the last raw
 * sample so a caller can read the sensor without the mapping applied. */
typedef struct { int minx, maxx, miny, maxy, down, lx, ly; } tscal;

/* The driver advertises the ADC's full 0..4095, but the panel only sweeps part
 * of it: normalising against the advertised range squeezes the touchable area
 * into the middle of the screen and kills the edges. These endpoints were
 * measured at the panel borders; `calib` re-measures them per panel. */
#define CAL_PATH "/tmp/ad4-touch.cal"
static const int ts_defx0 = 327, ts_defx1 = 3784, ts_defy0 = 640, ts_defy1 = 3451;

#ifdef __linux__
static int ts_open(tscal *c) {
    for (int i = 0; i < 32; i++) {
        char p[48];
        snprintf(p, sizeof p, "/dev/input/event%d", i);
        int fd = open(p, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        unsigned char bits[64];
        memset(bits, 0, sizeof bits);
        if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof bits), bits) < 0) { close(fd); continue; }
        if (!(bits[0] & 0x03)) { close(fd); continue; }   /* needs ABS_X and ABS_Y */
        struct input_absinfo ax = {0}, ay = {0};
        ioctl(fd, EVIOCGABS(ABS_X), &ax);
        ioctl(fd, EVIOCGABS(ABS_Y), &ay);
        c->minx = ts_defx0; c->maxx = ts_defx1;
        c->miny = ts_defy0; c->maxy = ts_defy1;
        c->down = 0; c->lx = 0; c->ly = 0;
        const char *calp = getenv("AD4_CAL");
        if (!calp || !*calp) calp = CAL_PATH;
        FILE *f = fopen(calp, "r");
        if (f) {
            int a, b, d, e;
            if (fscanf(f, "%d %d %d %d", &a, &b, &d, &e) == 4 && b > a && e > d) {
                c->minx = a; c->maxx = b; c->miny = d; c->maxy = e;
            } else {
                fprintf(stderr, "touch: %s unreadable, keeping built-in cal\n", calp);
            }
            fclose(f);
        }
        char name[64] = "";
        ioctl(fd, EVIOCGNAME(sizeof name), name);
        fprintf(stderr, "touch: %s name=%s driver X %d..%d Y %d..%d | cal X %d..%d Y %d..%d\n",
                p, name, ax.minimum, ax.maximum, ay.minimum, ay.maximum,
                c->minx, c->maxx, c->miny, c->maxy);
        return fd;
    }
    return -1;
}

/* Kernel 3.4 on 32-bit ARM: struct input_event is 16 bytes. Parse it by
 * fixed offsets instead of trusting a struct layout across toolchains. */
#define IE_SIZE 16
/* 0 = a position in *x and *y, 1 = the pen lifted, -1 = nothing left to read. */
static int ts_read(int fd, tscal *c, surf *s, int *x, int *y) {
    unsigned char buf[IE_SIZE];
    int have = 0, rx = 0, ry = 0, pr = -1;
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n != IE_SIZE) return -1;
        unsigned short type, code;
        int value;
        memcpy(&type, buf + 8, 2);
        memcpy(&code, buf + 10, 2);
        memcpy(&value, buf + 12, 4);
        if (type == EV_ABS && code == ABS_X) { rx = value; have |= 1; }
        else if (type == EV_ABS && code == ABS_Y) { ry = value; have |= 2; }
        else if (type == EV_ABS && code == ABS_PRESSURE) { pr = value; have |= 4; }
        else if (type == EV_KEY && code == BTN_TOUCH) { c->down = value; have |= 8; }
        else if (type == EV_SYN && code == SYN_REPORT) {
            /* Pen-down is the last BTN_TOUCH seen, or pressure if this report
             * carried it: the driver may omit the key mid-drag, or report a
             * pressure of 0 while touching. */
            if (c->down || pr > 0) {
                if ((have & 3) == 3) break;
                continue;
            }
            /* Pen up. The release report carries the key but no axes, so it is
             * the stroke boundary rather than a position. */
            if (have & 8) return 1;
            continue;
        }
    }
    c->lx = rx; c->ly = ry;
    /* The sensor reports in framebuffer space (its axes follow the controller
     * scan, not the physical panel), so normalise against fw/fh first and only
     * then undo the drawing rotation. */
    int fx = c->maxx == c->minx ? 0 : (rx - c->minx) * (s->fw - 1) / (c->maxx - c->minx);
    int fy = c->maxy == c->miny ? 0 : (ry - c->miny) * (s->fh - 1) / (c->maxy - c->miny);
    if (fx < 0) fx = 0;
    if (fx >= s->fw) fx = s->fw - 1;
    if (fy < 0) fy = 0;
    if (fy >= s->fh) fy = s->fh - 1;
    /* Panel-verified: the sensor's Y axis runs opposite to the controller's
     * scan, so undoing the drawing rotation also needs a horizontal flip. */
    int sx = fx, sy = fy;
    if (s->rot) { sx = s->w - 1 - fy; sy = s->fw - 1 - fx; }
    *x = sx; *y = sy;
    fprintf(stderr, "tap raw(%d,%d) -> fb(%d,%d) -> px(%d,%d)\n", rx, ry, fx, fy, sx, sy);
    return 0;
}

static long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
#endif /* __linux__ */

/* ------------------------------------------------------------ screens */
typedef struct { int x, y, w, h; const char *label; const char *token;
                 unsigned top, bot; int sc; int danger; int arg; } btn;

static void draw_btn(const surf *s, const btn *b) {
    rrect(s, b->x, b->y, b->w, b->h, 18, b->top, b->bot);
    textc_wrap(s, b->x + b->w / 2, b->y + b->h / 2 - (strchr(b->label, '\n') ? 12 * b->sc : 4 * b->sc),
               b->label, b->sc, 0xffffffu, 24 * b->sc);
}

static void info_lines(const surf *s, int y, const char *const *kv, int n) {
    const int lx = 24, maxw = s->w - 48;
    for (int i = 0; i < n; i++) {
        const char *eq = strchr(kv[i], '=');
        if (!eq) { y = text_wrap(s, lx, y, kv[i], 2, C_DIM, maxw) + 8; continue; }
        char k[40];
        int nk = (int)(eq - kv[i]);
        while (nk > 0 && kv[i][nk - 1] == ' ') nk--;   /* the keys are padded */
        if (nk > 39) nk = 39;
        memcpy(k, kv[i], nk); k[nk] = 0;
        const char *vp = eq + 1;
        while (*vp == ' ') vp++;
        text(s, lx, y, k, 2, C_DIM);
        y += 24;
        y = text_wrap(s, lx + 16, y, vp, 1, C_TXT, maxw - 16) + 10;
    }
}

static const char *g_env(const char *k, const char *d) {
    const char *v = getenv(k);
    return (v && *v) ? v : d;
}

/* --------------------------------------------------------------- menu */
/* Stacked, not side by side: portrait gives 432 px of width and a tap target
 * the height of a finger. The two-line buttons are the jobs that end with a
 * flash; DUMP only reads, and FULL FACTORY is the one that can lose work, so
 * it is red and needs a second tap. */
static const btn MENU[] = {
    { 24, 336, 432, 112, "KLIPPERIZE\ninstall klipper",
      "klipper", C_ACC, C_ACCD, 2, 0 },
    { 24, 456, 432,  80, "RESTORE MCU\nstock firmware only",
      "restore", 0x5a6a80u, 0x39465au, 2, 0 },
    { 24, 544, 432,  80, "FULL FACTORY\nsoc + mcu, wipes klipper",
      "factory", C_ERRD, 0x6d2530u, 2, 1 },
    { 24, 632, 432,  56, "DUMP FLASH",
      "dump", C_CARD2, C_CARD, 2, 0 },
    { 24, 696, 432,  44, "SKIP",
      "skip", C_CARD2, C_CARD, 2, 0 },
};
#define MENU_N ((int)(sizeof MENU / sizeof MENU[0]))

/* One paint path for the first frame, the countdown and the confirm, so the
 * three can never drift apart. */
static void paint_menu(surf *s, const char *chip, const char *const *lines,
                       int ask, const char *hint, int hintcol) {
    background(s);
    header(s, "ADVENTURER 4  KLIPPER", chip, C_ACC);
    info_lines(s, 118, lines, 4);
    if (ask >= 0)
        textc(s, s->w / 2, 300, "tap it again to confirm", 2, C_ERR);
    for (int i = 0; i < MENU_N; i++) {
        const btn *b = &MENU[i];
        if (i == ask)
            rrect(s, b->x, b->y, b->w, b->h, 18, C_ERR, C_ERRD);
        draw_btn(s, b);
    }
    textc(s, s->w / 2, 754, hint, 2, hintcol);
}

static int cmd_menu(const char *dump, const char *autotok, int autosec) {
    surf s;
    if (fb_open(&s) < 0) return 1;

    char chip[48];
    snprintf(chip, sizeof chip, "MCU %s", g_env("AD4_MCU", "?"));

    char l0[160], l1[160], l2[160], l3[160];
    snprintf(l0, sizeof l0, "MCU      = %s", g_env("AD4_MCU", "unknown"));
    snprintf(l1, sizeof l1, "Installed= %s", g_env("AD4_INSTALLED", "unknown"));
    snprintf(l2, sizeof l2, "Stick    = %s", g_env("AD4_STICK", "klipper.hex"));
    snprintf(l3, sizeof l3, "Factory  = %s", g_env("AD4_FACTORY", "not checked"));
    const char *lines[] = { l0, l1, l2, l3 };

    paint_menu(&s, chip, lines, -1, "nothing runs until you tap", C_DIM);

#ifdef AD4_DUMP
    (void)autotok; (void)autosec;
    if (dump) { dump_ppm(&s, dump); fb_close(&s, 0); return 0; }
    fb_close(&s, 0);
    fprintf(stderr, "host build: no touchscreen, use --dump\n");
    return 1;
#else
    tscal c;
    int fd = ts_open(&c);
    if (fd < 0) {
        textc(&s, s.w / 2, 754, "NO TOUCHSCREEN - using stick flags", 2, C_WARN);
        fb_close(&s, 0);
        return 1;
    }
    long t0 = now_ms();
    long deadline = autotok ? t0 + (long)autosec * 1000 : 0;
    /* The countdown is the one path with no second tap in front of it, so a
     * destructive token can never be the auto default - a FACTORY flag file
     * on an unattended stick must not wipe /opt/PROGRAM by itself. */
    for (int i = 0; i < MENU_N; i++) {
        if (autotok && !strcmp(MENU[i].token, autotok) && MENU[i].danger) {
            fprintf(stderr, "menu: '%s' is destructive, waiting for a tap\n", autotok);
            autotok = 0;
            deadline = 0;
        }
    }
    int ask = -1;
    for (;;) {
        struct pollfd p = { .fd = fd, .events = POLLIN };
        long left = deadline ? deadline - now_ms() : 1000;
        if (deadline && left <= 0) {
            printf("%s\n", autotok);
            fflush(stdout);
            close(fd); fb_close(&s, 0);
            return 0;
        }
        if (poll(&p, 1, (int)left) > 0) {
            int x, y;
            if (ts_read(fd, &c, &s, &x, &y) == 0) {
                for (int i = 0; i < MENU_N; i++) {
                    const btn *b = &MENU[i];
                    if (!(x >= b->x && x < b->x + b->w &&
                          y >= b->y && y < b->y + b->h))
                        continue;
                    if (ask >= 0 && i != ask) {
                        /* While a destructive action is armed, every other tap
                         * only cancels it: a slip from one button to the next
                         * must not trade a mis-tap for a wiped /opt/PROGRAM. */
                        ask = -1;
                        deadline = 0;
                        buzz_cue(AD4_MARK);
                        paint_menu(&s, chip, lines, ask,
                                   "cancelled", C_DIM);
                        break;
                    }
                    if (b->danger && ask != i) {
                        ask = i;
                        deadline = 0;      /* never auto-fire a confirmed wipe */
                        buzz_cue(AD4_MARK);
                        paint_menu(&s, chip, lines, ask,
                                   "any other tap cancels", C_WARN);
                        break;
                    }
                    rrect(&s, b->x, b->y, b->w, b->h, 18, C_OK, C_ACCD);
                    draw_btn(&s, b);
                    buzz_cue(AD4_TAP);
                    printf("%s\n", b->token);
                    fflush(stdout);
                    close(fd); fb_close(&s, 0);
                    return strcmp(b->token, "skip") == 0 ? 2 : 0;
                }
            }
        }
        if (deadline) {   /* repaint the countdown */
            int left_s = (int)((deadline - now_ms() + 999) / 1000);
            char cd[64];
            snprintf(cd, sizeof cd, "auto %s in %ds - tap SKIP to stop", autotok, left_s);
            paint_menu(&s, chip, lines, ask, cd, C_WARN);
        }
    }
#endif
}

/* ------------------------------------------------------------ touch test */
/* Diagnostic mode: a live crosshair tracks the finger, and five fixed targets
 * turn each stroke into a pixel error, so the mapping and its accuracy are
 * both read off the panel instead of inferred from a log. */
#ifndef AD4_DUMP
static void crosshair(const surf *s, int x, int y, unsigned col) {
    fill(s, x - 44, y - 1, 89, 3, col);
    fill(s, x - 1, y - 44, 3, 89, col);
    fill(s, x - 5, y - 5, 11, 11, col);
}
#endif

/* Hollow cross: the gap in the middle is where a centred tap should land, so
 * an offset reads directly against the stroke marker. */
static void draw_target(const surf *s, int x, int y, unsigned col) {
    fill(s, x - 26, y - 1, 18, 3, col);
    fill(s, x + 8, y - 1, 18, 3, col);
    fill(s, x - 1, y - 26, 3, 18, col);
    fill(s, x - 1, y + 8, 3, 18, col);
}

/* One stroke scored: its mean position against the nearest target. */
#ifndef AD4_DUMP
static void score_stroke(const surf *s, const int *tx, const int *ty, int nt,
                         int ax, int ay, int an, int *taps, int *worst,
                         char *note, size_t notesz) {
    int mx = ax / an, my = ay / an, best = 0, bd = 1 << 30;
    for (int i = 0; i < nt; i++) {
        int d = (mx - tx[i]) * (mx - tx[i]) + (my - ty[i]) * (my - ty[i]);
        if (d < bd) { bd = d; best = i; }
    }
    int dx = mx - tx[best], dy = my - ty[best];
    if (-dx > *worst) *worst = -dx;
    if (dx > *worst) *worst = dx;
    if (-dy > *worst) *worst = -dy;
    if (dy > *worst) *worst = dy;
    (*taps)++;
    snprintf(note, notesz, "tap %d dx=%d dy=%d", *taps, dx, dy);
    fprintf(stderr, "tap %d: mean(%d,%d) target %d at (%d,%d) dx=%d dy=%d\n",
            *taps, mx, my, best + 1, tx[best], ty[best], dx, dy);
    fill(s, mx - 3, my - 3, 7, 7, C_ACCD);
    buzz_cue(AD4_MARK);
}
#endif

static int cmd_touch(const char *dump, int secs) {
    surf s;
    if (fb_open(&s) < 0) return 1;
    static const int tx[] = { 40, 440, 240, 40, 440 };
    static const int ty[] = { 200, 200, 420, 660, 660 };
    const int nt = (int)(sizeof tx / sizeof tx[0]);
    /* Flat, not background(): the live crosshair is retired by repainting a
     * square over it, which only matches a gradient if the field is flat. */
    fill(&s, 0, 0, s.w, s.h, C_BG0);
    textc_wrap(&s, s.w / 2, 112, "tap each target, then drag to trace", 2, C_DIM, 22);
    for (int i = 0; i < nt; i++) draw_target(&s, tx[i], ty[i], C_CARD2);
#ifdef AD4_DUMP
    (void)secs;
    header(&s, "TOUCH TEST", "host preview", C_ACC);
    if (dump) dump_ppm(&s, dump);
    fb_close(&s, 0);
    return 1;
#else
    tscal c;
    int fd = ts_open(&c);
    if (fd < 0) {
        header(&s, "TOUCH TEST", "", C_ACC);
        textc(&s, s.w / 2, 400, "no touchscreen found", 2, C_ERR);
        fb_close(&s, 0);
        return 1;
    }
    /* The raw range goes in the header chip: the crosshair repaints a square
     * around itself and must not eat body text. */
    char rng[64];
    snprintf(rng, sizeof rng, "raw X %d..%d Y %d..%d", c.minx, c.maxx, c.miny, c.maxy);
    header(&s, "TOUCH TEST", rng, C_ACC);

    long stop = now_ms() + (long)(secs > 0 ? secs : 45) * 1000;
    int n = 0, taps = 0, worst = 0, cx = -1, cy = -1, ax = 0, ay = 0, an = 0;
    char note[48] = "waiting for a touch";
    long last = now_ms();
    while (now_ms() < stop) {
        struct pollfd p = { .fd = fd, .events = POLLIN };
        if (poll(&p, 1, 200) > 0) {
            for (;;) {   /* the fd is nonblocking, so drain the whole queue */
                int x, y;
                int r = ts_read(fd, &c, &s, &x, &y);
                if (r < 0) break;
                if (r == 0) {
                    n++;
                    if (cx >= 0) {   /* retire the previous live cross */
                        fill(&s, cx - 46, cy - 46, 93, 93, C_BG0);
                        for (int i = 0; i < nt; i++)
                            draw_target(&s, tx[i], ty[i], C_CARD2);
                    }
                    crosshair(&s, x, y, C_ACC);
                    cx = x; cy = y;
                    ax += x; ay += y; an++;
                    last = now_ms();
                } else if (an) {     /* pen lifted: score the stroke */
                    score_stroke(&s, tx, ty, nt, ax, ay, an,
                                 &taps, &worst, note, sizeof note);
                    ax = ay = an = 0;
                }
            }
        }
        /* The release report is not guaranteed to arrive, so a quiet stretch
         * closes the stroke too. */
        if (an && now_ms() - last > 400) {
            score_stroke(&s, tx, ty, nt, ax, ay, an, &taps, &worst, note, sizeof note);
            ax = ay = an = 0;
        }
        /* Redrawn every pass: the countdown proves the test is alive when the
         * driver goes quiet mid-drag. */
        char pos[96];
        snprintf(pos, sizeof pos, "%s  |  %d reports  %lus left", note, n,
                 (unsigned long)((stop - now_ms()) / 1000));
        fill(&s, 24, 740, s.w - 48, 28, C_CARD);
        text(&s, 28, 748, pos, 1, n ? C_TXT : C_DIM);
    }
    fill(&s, 0, 0, s.w, s.h, C_BG0);
    header(&s, "TOUCH TEST", "window closed", C_ACCD);
    char sum[96];
    snprintf(sum, sizeof sum, "%d reports, %d taps, worst %d px off target", n, taps, worst);
    textc_wrap(&s, s.w / 2, 200, sum, 2, n ? C_OK : C_ERR, s.w - 48);
    textc(&s, s.w / 2, 300, "the panel is idle now, not hung", 2, C_DIM);
    fprintf(stderr, "touch test: %d reports, %d taps, worst %d px off target\n",
            n, taps, worst);
    fb_close(&s, 0);
    return n > 0 ? 0 : 1;
#endif
}

/* ------------------------------------------------------------- calibration */
/* The advertised axis range is wider than the panel, so the real one can only
 * come from touching the panel. Four marks inset from the bezel, then
 * extrapolate out to the screen edges: the corners themselves are dead band. */
static int cmd_calib(int secs) {
    surf s;
    if (fb_open(&s) < 0) return 1;
#ifndef __linux__
    header(&s, "CALIBRATION", "", C_ACC);
    textc(&s, s.w / 2, 400, "no touchscreen in this build", 2, C_ERR);
    fb_close(&s, 0);
    return 1;
#else
    tscal c;
    int fd = ts_open(&c);
    if (fd < 0) {
        header(&s, "CALIBRATION", "", C_ACC);
        textc(&s, s.w / 2, 400, "no touchscreen found", 2, C_ERR);
        fb_close(&s, 0);
        return 1;
    }
    static const int gx[4] = { 40, 440, 40, 440 };
    static const int gy[4] = { 140, 140, 660, 660 };
    long rx[4] = { 0 }, ry[4] = { 0 };
    int i = 0;
    long stop = now_ms() + (long)(secs > 0 ? secs : 120) * 1000;
    while (i < 4 && now_ms() < stop) {
        fill(&s, 0, 0, s.w, s.h, C_BG0);
        char chip[32];
        snprintf(chip, sizeof chip, "mark %d of 4", i + 1);
        header(&s, "CALIBRATION", chip, C_ACC);
        textc_wrap(&s, s.w / 2, 130, "press on the mark, hold a moment, then lift", 2, C_DIM, 26);
        draw_target(&s, gx[i], gy[i], C_ACC);
        long sx = 0, sy = 0, last = now_ms();
        int n = 0, lifted = 0;
        while (!lifted && now_ms() < stop) {
            struct pollfd p = { .fd = fd, .events = POLLIN };
            if (poll(&p, 1, 200) > 0) {
                for (;;) {
                    int x, y;
                    int r = ts_read(fd, &c, &s, &x, &y);
                    if (r < 0) break;
                    if (r == 0) { sx += c.lx; sy += c.ly; n++; last = now_ms(); }
                    else if (n) { lifted = 1; break; }
                }
            }
            char st[64];
            snprintf(st, sizeof st, "%d samples  %lus left", n,
                     (unsigned long)((stop - now_ms()) / 1000));
            fill(&s, 24, 740, s.w - 48, 28, C_CARD);
            text(&s, 28, 748, st, 1, n ? C_TXT : C_DIM);
            if (n && now_ms() - last > 500) break;
        }
        if (!n) break;
        rx[i] = sx / n; ry[i] = sy / n;
        fprintf(stderr, "calib mark %d at (%d,%d): raw X %ld Y %ld from %d samples\n",
                i + 1, gx[i], gy[i], rx[i], ry[i], n);
        buzz_cue(AD4_MARK);
        i++;
    }
    fill(&s, 0, 0, s.w, s.h, C_BG0);
    if (i < 4) {
        header(&s, "CALIBRATION", "incomplete", C_ERR);
        char m[80];
        snprintf(m, sizeof m, "%d of 4 marks captured, nothing saved", i);
        textc(&s, s.w / 2, 240, m, 2, C_ERR);
        fprintf(stderr, "calib: only %d of 4 marks, nothing saved\n", i);
        fb_close(&s, 0);
        return 1;
    }
    /* The rotation swaps the axes: sensor Y carries panel X, sensor X carries
     * panel Y, so each pair of marks calibrates the other axis. */
    double ryL = (ry[0] + ry[2]) / 2.0, ryR = (ry[1] + ry[3]) / 2.0;
    double ky = (ryR - ryL) / (gx[1] - gx[0]);
    double fy0 = ryL + ky * (0 - gx[0]);            /* raw at panel x = 0 */
    double fy1 = ryL + ky * ((s.w - 1) - gx[0]);    /* raw at panel x = w-1 */
    double rxT = (rx[0] + rx[1]) / 2.0, rxB = (rx[2] + rx[3]) / 2.0;
    double kx = (rxB - rxT) / (gy[2] - gy[0]);
    double fx0 = rxT + kx * (0 - gy[0]);            /* raw at panel y = 0 */
    double fx1 = rxT + kx * ((s.h - 1) - gy[0]);    /* raw at panel y = h-1 */
    int cal[4] = { (int)(fx0 < fx1 ? fx0 : fx1), (int)(fx0 < fx1 ? fx1 : fx0),
                   (int)(fy0 < fy1 ? fy0 : fy1), (int)(fy0 < fy1 ? fy1 : fy0) };
    char res[96];
    snprintf(res, sizeof res, "cal X %d..%d  Y %d..%d", cal[0], cal[1], cal[2], cal[3]);
    if (cal[1] - cal[0] < 512 || cal[3] - cal[2] < 512) {
        header(&s, "CALIBRATION", "rejected", C_ERR);
        textc(&s, s.w / 2, 240, res, 2, C_ERR);
        textc(&s, s.w / 2, 300, "span too small, nothing saved", 2, C_DIM);
        fprintf(stderr, "calib: rejected %s\n", res);
        fb_close(&s, 0);
        return 1;
    }
    const char *calp = getenv("AD4_CAL");
    if (!calp || !*calp) calp = CAL_PATH;
    FILE *f = fopen(calp, "w");
    if (f) { fprintf(f, "%d %d %d %d\n", cal[0], cal[1], cal[2], cal[3]); fclose(f); }
    header(&s, "CALIBRATION", f ? "saved" : "write failed", f ? C_OK : C_ERR);
    textc(&s, s.w / 2, 240, res, 2, C_TXT);
    textc_wrap(&s, s.w / 2, 320, f ? calp : "could not write the cal file", 2, C_DIM, 26);
    textc(&s, s.w / 2, 420, "run: ad4-screen touch 60 to verify", 2, C_ACC);
    fprintf(stderr, "calib: %s -> %s\n", res, f ? calp : "NOT SAVED");
    fb_close(&s, 0);
    return f ? 0 : 1;
#endif
}

static int cmd_simple(const char *kind, const char *dump, const char *title,
                      const char *sub, int pct) {
    surf s;
    if (fb_open(&s) < 0) return 1;
    background(&s);
    unsigned acc = C_ACC;
    if (!strcmp(kind, "done")) acc = C_OK;
    if (!strcmp(kind, "fail")) acc = C_ERR;
    char chip[48];
    snprintf(chip, sizeof chip, "MCU %s", g_env("AD4_MCU", "?"));
    header(&s, "ADVENTURER 4  KLIPPER", chip, acc);

    int tsc = tw(title ? title : "", 3) <= s.w - 48 ? 3 : 2;
    textc(&s, s.w / 2, 170, title ? title : "", tsc, C_TXT);
    int y = 240;
    if (sub && *sub) y = textc_wrap(&s, s.w / 2, 240, sub, 2, C_DIM, 26);
    /* the mark goes below the message, however many lines it wrapped to */
    int my = y + 60 > 360 ? y + 60 : 360;

    if (!strcmp(kind, "status")) {
        progress(&s, my, pct < 0 ? 0 : pct, acc);
        /* only when the caller had nothing else to say */
        if (!sub || !*sub) textc(&s, s.w / 2, my + 50, "do not power off the printer", 2, C_WARN);
    } else if (!strcmp(kind, "done")) {
        /* two strokes meeting at a vertex, otherwise it reads as a "V" */
        int cx = s.w / 2 - 10, cy = my, t = 11;
        for (int i = 0; i < 20; i++) fill(&s, cx - 40 + i, cy + i, t, t, C_OK);
        for (int i = 0; i < 46; i++) fill(&s, cx - 21 + i, cy + 20 - i, t, t, C_OK);
    } else if (!strcmp(kind, "fail")) {
        int cx = s.w / 2, cy = my + 20, t = 11;
        for (int i = 0; i <= 40; i++) {
            fill(&s, cx - 26 + i, cy - 26 + i, t, t, C_ERR);
            fill(&s, cx + 26 - i, cy - 26 + i, t, t, C_ERR);
        }
    }
#ifdef AD4_DUMP
    if (dump) dump_ppm(&s, dump);
#endif
    fb_close(&s, 0);
    return 0;
}

static int cmd_splash(const char *dump) {
    surf s;
    if (fb_open(&s) < 0) return 1;
    background(&s);
    textc(&s, s.w / 2, 320, "ADVENTURER 4", 4, C_TXT);
    textc(&s, s.w / 2, 392, "klipper installer", 3, C_ACC);
    textc(&s, s.w / 2, 560, g_env("AD4_IP", ""), 2, C_DIM);
#ifdef AD4_DUMP
    if (dump) dump_ppm(&s, dump);
#endif
    fb_close(&s, 0);
    return 0;
}

/* ------------------------------------------------------------ boot */
/* Boot animation: a pixel-art superhero is printed on the bed row by row,
 * the nozzle snaking across each layer and every new pixel glowing hot before
 * it cools to its colour; then the nozzle parks, the figure hops, and the
 * title comes up, to the "Stage clear" riff. About 4 s; it exits on its
 * last frame, which stays up.
 *
 * Frames are composed off-screen and copied to the framebuffer whole, so
 * nothing tears. With --dump, a path containing '%' gets every frame
 * (printf'd with the frame number, 30 fps of virtual time); otherwise only
 * the last frame is written. */
static const char *const HERO[] = {
    ".........KKKK.........",
    ".......KKDDDDKK.......",
    "......KDDDDDDDDK......",
    "......KDSSSSSSDK......",
    "......KNNNNNNNNK......",
    "......KNWWNNWWNK......",
    "......KSSSSSSSSK......",
    "......KSSSssSSSK......",
    ".......KsSSSSsK.......",
    "........KKssKK........",
    "......KKBBBBBBKK......",
    "....KKBBBBBBBBBBKK....",
    "...KRKBBYYYYYYBBKRK...",
    "..KRKBBKBYYYYBKBBKRK..",
    ".KRKBBKBBBYYBBBKBBKRK.",
    ".KRKBKRKBBBBBBKRKBKRK.",
    ".KRKWWKKBBBBBBKKWWKRK.",
    "..KRKKWWKYYYYKWWKKRK..",
    "..KRRRKKBBBBBBKKRRRK..",
    "..KRRRKBBBBBBBBKRRRK..",
    ".KRRRrKbBBKKBBbKrRRRK.",
    ".KRRRrKbBBKKBBbKrRRRK.",
    "KRRRRrKbBBKKBBbKrRRRRK",
    "KRRRRrKbBBKKBBbKrRRRRK",
    "KrRrRrKbBBKKBBbKrRrRrK",
    ".KKKKKKRRRKKRRRKKKKKK.",
    "......KRRRKKRRRK......",
    ".....KRRRrKKrRRRK.....",
    ".....KKKKKKKKKKKK.....",
};
#define HERO_W 22
#define HERO_H ((int)(sizeof HERO / sizeof HERO[0]))

static const char *const NOZZLE[] = {
    ".KKKKKKKKK.",
    "KMMMMMMMMMK",
    "KMmmmmmmmMK",
    "KRRRRRRRRRK",
    "KRrrrrrrrRK",
    ".KKKKKKKKK.",
    "...KCCCK...",
    "....KCK....",
    ".....K.....",
};
#define NOZ_W 11
#define NOZ_H ((int)(sizeof NOZZLE / sizeof NOZZLE[0]))

static unsigned pal(char c) {
    switch (c) {
    case 'K': return 0x141022u;   /* outline */
    case 'D': return 0x2b2140u;   /* hair */
    case 'N': return 0x16204au;   /* mask */
    case 'W': return 0xfff4d6u;   /* eyes, gloves */
    case 'S': return 0xf2c39bu;   /* skin */
    case 's': return 0xd39a73u;
    case 'R': return 0xe0484fu;   /* cape, boots; heater block on the nozzle */
    case 'r': return 0xa8323au;
    case 'B': return 0x4d9fffu;   /* suit */
    case 'b': return 0x2f6fc4u;
    case 'Y': return 0xffd23cu;   /* emblem (a nozzle), belt */
    case 'y': return 0xc99a1eu;
    case 'M': return 0xb4c0d2u;   /* nozzle metal */
    case 'm': return 0x7d8aa0u;
    case 'C': return 0xd08a3cu;   /* brass tip */
    }
    return 0;
}

static unsigned mix(unsigned a, unsigned b, int t) {   /* t 0..256: a -> b */
    int ar = (a >> 16) & 0xff, ag = (a >> 8) & 0xff, ab = a & 0xff;
    int br = (b >> 16) & 0xff, bg = (b >> 8) & 0xff, bb = b & 0xff;
    return ((unsigned)(ar + (br - ar) * t / 256) << 16) |
           ((unsigned)(ag + (bg - ag) * t / 256) << 8) |
            (unsigned)(ab + (bb - ab) * t / 256);
}

#define BP      10                       /* screen px per hero pixel */
#define BED_Y   548
#define CELL_MS 5                        /* nozzle time per pixel */
#define HOP_MS  20                       /* travel between layers */
#define COOL_MS 260                      /* hot glow fades over this */
#define HOT     0xffe08au
#define HOT2    0xff7a1au

typedef struct { int r, lo, hi, dir, t0; } blayer;

static int cmd_boot(const char *dump) {
    surf s;
    int fd = fb_open(&s);
    if (fd < 0) return 1;
    size_t len = (size_t)s.stride * (s.rot ? s.fh : s.h);
    surf bk = s;
    unsigned char *bg = malloc(len);
    bk.mem = malloc(len);
    if (!bg || !bk.mem) { fb_close(&s, fd); return 1; }
    bk.owned = 0;
    { surf t = s; t.mem = bg; background(&t); }

    int x0 = (s.w - HERO_W * BP) / 2, y0 = BED_Y - HERO_H * BP;
    /* layers bottom-up; each prints only its filled span, alternating sides */
    blayer L[HERO_H];
    int nl = 0, t = 500;                 /* the nozzle drops in first */
    for (int r = HERO_H - 1; r >= 0; r--) {
        int lo = -1, hi = -1;
        for (int c = 0; c < HERO_W; c++)
            if (HERO[r][c] != '.') { if (lo < 0) lo = c; hi = c; }
        if (lo < 0) continue;
        L[nl] = (blayer){ r, lo, hi, nl & 1, t };
        t += (hi - lo + 1) * CELL_MS + HOP_MS;
        nl++;
    }
    const int t_built = t, t_park = t + 350, t_hop = t + 450, t_title = t + 750,
              t_end = t + 1500;
    const char *title = "ADVENTURER 4", *sub = "KLIPPER";

#ifndef AD4_DUMP
    long start = now_ms();
#endif
    int riff = 0;
    for (int frame = 0;; frame++) {
#ifdef AD4_DUMP
        int now = frame * 33;
#else
        int now = (int)(now_ms() - start);
#endif
        if (now > t_end) now = t_end;
        if (!riff && now >= t_built) {   /* print done: the riff, in a child so frames keep coming */
            riff = 1;
#ifndef AD4_DUMP
            if (fork() == 0) _exit(buzz_cue(AD4_RIFF) ? 1 : 0);
#endif
        }
        memcpy(bk.mem, bg, len);

        /* bed */
        rrect(&bk, x0 - 40, BED_Y, HERO_W * BP + 80, 18, 4, 0x34425cu, 0x1c2638u);
        fill(&bk, x0 - 36, BED_Y, HERO_W * BP + 72, 2, now < t_built ? 0xff9f5au : C_ACC);

        /* hop: a small parabola once the nozzle is gone */
        int lift = 0;
        if (now >= t_hop && now < t_hop + 300) {
            int u = now - t_hop;                 /* 0..300 */
            lift = 4 * 34 * u * (300 - u) / (300 * 300);
        }

        /* printed pixels */
        int nx = -1, ny = -1;                    /* nozzle tip, screen px */
        for (int i = 0; i < nl; i++) {
            const blayer *l = &L[i];
            int r = l->r;
            if (now < l->t0) break;
            int span = l->hi - l->lo + 1;
            for (int j = 0; j < span; j++) {
                int c = l->dir ? l->hi - j : l->lo + j;
                int laid = l->t0 + j * CELL_MS;
                if (now < laid || HERO[r][c] == '.') continue;
                unsigned col = pal(HERO[r][c]);
                int age = now - laid;
                if (age < COOL_MS) {
                    unsigned hot = mix(HOT, HOT2, age * 256 / COOL_MS);
                    col = mix(hot, col, age * 256 / COOL_MS);
                }
                fill(&bk, x0 + c * BP, y0 + r * BP - lift, BP, BP, col);
            }
            if (now < l->t0 + span * CELL_MS) {  /* nozzle is on this layer */
                int j = (now - l->t0) / CELL_MS;
                int frac = (now - l->t0) % CELL_MS * BP / CELL_MS;
                int c = l->dir ? l->hi - j : l->lo + j;
                nx = x0 + c * BP + BP / 2 + (l->dir ? -frac : frac);
                ny = y0 + r * BP;
            } else if (i + 1 < nl && now < L[i + 1].t0) {   /* hopping up */
                nx = x0 + (l->dir ? l->lo : l->hi) * BP + BP / 2;
                ny = y0 + r * BP;
            }
        }
        if (now < L[0].t0) {                     /* drop in from the top */
            nx = x0 + L[0].lo * BP + BP / 2;
            ny = 120 + (y0 + (HERO_H - 1) * BP - 120) * now / L[0].t0;
        } else if (now >= t_built && now < t_park) {   /* park: up and away */
            nx = x0 + (L[nl - 1].dir ? L[nl - 1].lo : L[nl - 1].hi) * BP + BP / 2;
            ny = y0 - (y0 + 60) * (now - t_built) / (t_park - t_built);
        }

        /* gantry rail and nozzle */
        if (ny > -60 && nx >= 0) {
            int ty = ny - NOZ_H * 6;
            fill(&bk, 0, ty + 8, s.w, 6, 0x2e4468u);
            fill(&bk, 0, ty + 8, s.w, 2, 0x45618fu);
            for (int j = 0; j < NOZ_H; j++)
                for (int i = 0; i < NOZ_W; i++)
                    if (NOZZLE[j][i] != '.')
                        fill(&bk, nx - NOZ_W * 3 + i * 6, ty + j * 6, 6, 6, pal(NOZZLE[j][i]));
            if (now >= L[0].t0 && now < t_built)
                fill(&bk, nx - 3, ny - 2, 6, 4, HOT);
        }

        /* title, letter by letter */
        if (now >= t_title) {
            int n = (now - t_title) / 45, len1 = (int)strlen(title);
            char buf[32];
            snprintf(buf, sizeof buf, "%.*s", n < len1 ? n : len1, title);
            text(&bk, (s.w - tw(title, 4)) / 2, 606, buf, 4, C_TXT);
            if (n > len1 + 2) textc(&bk, s.w / 2, 668, sub, 3, C_ACC);
            if (n > len1 + 6) textc(&bk, s.w / 2, 740, g_env("AD4_IP", ""), 2, C_DIM);
        }

        memcpy(s.mem, bk.mem, len);
#ifdef AD4_DUMP
        if (dump && strchr(dump, '%')) {
            char p[256];
            snprintf(p, sizeof p, dump, frame);
            dump_ppm(&s, p);
        }
#endif
        if (now >= t_end) break;
#ifndef AD4_DUMP
        usleep(15000);
#endif
    }
#ifdef AD4_DUMP
    if (dump && !strchr(dump, '%')) dump_ppm(&s, dump);
#endif
    free(bk.mem);
    free(bg);
    fb_close(&s, fd);
    return 0;
}

/* ------------------------------------------------------------ confirm */
/* The install and rollback sticks ask once before they flash. A timeout or
 * CANCEL is a no: a stick left in by mistake must never write the MCU. */
static const btn CONFIRM[] = {
    { 24, 520, 432, 112, "", "go", C_OK, 0x1f8a5fu, 3, 0 },
    { 24, 656, 432,  72, "CANCEL", "cancel", C_CARD2, C_CARD, 2, 0 },
};

static void paint_confirm(surf *s, const char *title, const char *body,
                          const char *go, int left) {
    background(s);
    header(s, "ADVENTURER 4  KLIPPER", g_env("AD4_MCU", ""), C_ACC);
    int tsc = tw(title, 3) <= s->w - 48 ? 3 : 2;
    textc(s, s->w / 2, 130, title, tsc, C_TXT);
    text_wrap(s, 24, 190, body, 2, C_DIM, s->w - 48);
    btn g = CONFIRM[0];
    g.label = go;
    draw_btn(s, &g);
    draw_btn(s, &CONFIRM[1]);
    char cd[64];
    snprintf(cd, sizeof cd, "cancels by itself in %ds", left);
    textc(s, s->w / 2, 754, cd, 2, C_WARN);
}

/* exit 0 = go, 2 = cancel or timeout, 1 = no panel or no touchscreen */
static int cmd_confirm(const char *dump, const char *title, const char *body,
                       const char *go, int secs) {
    surf s;
    if (secs <= 0) secs = 120;
    if (fb_open(&s) < 0) return 1;
    paint_confirm(&s, title, body, go, secs);
#ifdef AD4_DUMP
    if (dump) dump_ppm(&s, dump);
    fb_close(&s, 0);
    return 2;
#else
    (void)dump;
    tscal c;
    int fd = ts_open(&c);
    if (fd < 0) { fb_close(&s, 0); return 1; }
    long deadline = now_ms() + (long)secs * 1000;
    /* a finger already on the glass when the screen appears is not an answer */
    int pressed = 1;
    long armed = now_ms() + 700;
    for (;;) {
        long left = deadline - now_ms();
        if (left <= 0) break;
        struct pollfd p = { .fd = fd, .events = POLLIN };
        if (poll(&p, 1, left < 1000 ? (int)left : 1000) > 0) {
            int x, y, r;
            while ((r = ts_read(fd, &c, &s, &x, &y)) >= 0) {
                if (r == 1) { pressed = 0; continue; }
                if (pressed || now_ms() < armed) { pressed = 1; continue; }
                pressed = 1;
                for (int i = 0; i < 2; i++) {
                    const btn *b = &CONFIRM[i];
                    if (x < b->x || x >= b->x + b->w || y < b->y || y >= b->y + b->h)
                        continue;
                    buzz_cue(AD4_TAP);
                    printf("%s\n", b->token);
                    fflush(stdout);
                    close(fd); fb_close(&s, 0);
                    return i == 0 ? 0 : 2;
                }
            }
        }
        paint_confirm(&s, title, body, go, (int)((deadline - now_ms() + 999) / 1000));
    }
    printf("timeout\n");
    close(fd); fb_close(&s, 0);
    return 2;
#endif
}

/* ------------------------------------------------------------ klipper */
/* The panel's day job once the MCU runs Klipper. The vendor Qt GUI speaks
 * Flashforge's MCU protocol and can never come back, so this polls Moonraker
 * (through Caddy on the Klipper host, wired link by default) once a second and
 * paints state, temperatures and job progress, with a few touch actions. The
 * SoC has no python and busybox wget cannot POST, so HTTP is done by hand. */
#ifdef __linux__
/* Slicer thumbnails are PNG; stb_image (v2.30, nothings/stb 013ac3b) decodes
 * them, built for PNG from memory only. */
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STB_IMAGE_IMPLEMENTATION
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "stb_image.h"
#pragma GCC diagnostic pop

/* The SoC has no RTC and no NTP, so its clock reads 1970. Moonraker's Date
 * header gives the real time: g_wall is UTC minus time(NULL). */
static long g_wall;
static int g_blen;                    /* body length of the last http_req */

static void http_date(const char *hdr) {
    static const char mon[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char *d = strstr(hdr, "\r\nDate: ");
    struct tm tm;
    char m[4];
    memset(&tm, 0, sizeof tm);
    if (!d || sscanf(d + 8, "%*3s, %d %3s %d %d:%d:%d", &tm.tm_mday, m, &tm.tm_year,
                     &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6)
        return;
    const char *f = strstr(mon, m);
    if (!f) return;
    tm.tm_mon = (int)(f - mon) / 3;
    tm.tm_year -= 1900;
    g_wall = (long)(timegm(&tm) - time(NULL));
}

/* Returns the HTTP status (body left in out, g_blen bytes, NUL-terminated),
 * or -1 when the host is down. */
static int http_req(const char *host, int port, const char *method,
                    const char *path, char *out, int outn) {
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    out[0] = 0;
    if (inet_pton(AF_INET, host, &a.sin_addr) != 1) return -1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    fcntl(fd, F_SETFL, O_NONBLOCK);
    connect(fd, (struct sockaddr *)&a, sizeof a);
    struct pollfd p = { .fd = fd, .events = POLLOUT };
    int err = 0;
    socklen_t el = sizeof err;
    if (poll(&p, 1, 1500) != 1 ||
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) < 0 || err) {
        close(fd);
        return -1;
    }
    char req[1024];
    int n = snprintf(req, sizeof req,
                     "%s %s HTTP/1.0\r\nHost: %s:%d\r\nContent-Length: 0\r\n\r\n",
                     method, path, host, port);
    if (write(fd, req, n) != n) { close(fd); return -1; }
    int got = 0;
    long end = now_ms() + 3000;
    p.events = POLLIN;
    while (got < outn - 1) {
        long left = end - now_ms();
        if (left <= 0 || poll(&p, 1, (int)left) != 1) break;
        ssize_t r = read(fd, out + got, outn - 1 - got);
        if (r <= 0) break;
        got += (int)r;
    }
    close(fd);
    out[got] = 0;
    if (!got) return -2;              /* connected, no answer yet: still running */
    int code = -1;
    sscanf(out, "HTTP/%*s %d", &code);
    char *body = strstr(out, "\r\n\r\n");
    g_blen = 0;
    if (body) {
        *body = 0;
        http_date(out);
        g_blen = got - (int)(body + 4 - out);
        memmove(out, body + 4, g_blen);
    }
    out[g_blen] = 0;
    return code;
}

/* Moonraker answers compact JSON, so two lookups do: jobj() bounds the
 * "name":{...} span, jval() finds "key": inside a span. */
static const char *jobj(const char *js, const char *name, const char **end) {
    char pat[48];
    snprintf(pat, sizeof pat, "\"%s\":{", name);
    const char *p = strstr(js, pat);
    if (!p) return NULL;
    p += strlen(pat) - 1;
    int depth = 0, str = 0;
    for (const char *q = p; *q; q++) {
        if (str) {
            if (*q == '\\' && q[1]) q++;
            else if (*q == '"') str = 0;
            continue;
        }
        if (*q == '"') str = 1;
        else if (*q == '{') depth++;
        else if (*q == '}' && --depth == 0) { *end = q; return p; }
    }
    return NULL;
}

static const char *jval(const char *b, const char *e, const char *key) {
    char pat[48];
    int n = snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = b ? strstr(b, pat) : NULL;
    return (p && p < e) ? p + n : NULL;
}

static double jnum(const char *b, const char *e, const char *key) {
    const char *v = jval(b, e, key);
    return v ? strtod(v, NULL) : 0;
}

static void jstr(const char *b, const char *e, const char *key, char *out, int n) {
    const char *v = jval(b, e, key);
    int j = 0;
    if (v && *v == '"')
        for (v++; *v && *v != '"' && j < n - 1; v++) {
            if (*v == '\\' && v[1]) {
                v++;
                if (*v == 'n') { out[j++] = ' '; continue; }
                if (*v == 'u') { out[j++] = '?'; v += 4; continue; }
            }
            out[j++] = *v;
        }
    out[j] = 0;
}

typedef struct {
    int online;                       /* Moonraker answered at all */
    char kstate[16], kmsg[160];       /* klippy: ready/startup/shutdown/error */
    char pstate[16], file[96];        /* print_stats */
    char pmsg[160];                   /* print_stats message: why a job failed */
    char dmsg[96];                    /* display_status message: why a job stopped (B2) */
    char ver[48];                     /* Klipper software_version */
    char homed[8];                    /* toolhead homed_axes */
    double et, ett, bt, btt, prog, dur, tot, fused, pfan, cfan, light, pos[3];
    double speed, flow, zoff;         /* gcode_move: M220, M221, SET_GCODE_OFFSET */
    int layer, nlayer;                /* SET_PRINT_STATS_INFO, 0 = not sent */
    int fdir;                         /* _FIL_STATE: 1 load, -1 unload, 0 none */
    int swap, fed;                    /* PAUSE: filament change 1, ran out 2; new filament loaded */
    int cstep, cpass, cpasses;        /* CLEAR_CLOG: step, 0 = not running */
    double fe0, flen, fe;             /* its start and length, live extruder position */
    char smode[12];                   /* SPEED_MODE's saved choice, "" = never set */
    int mode[5][4];                   /* its modes: mm/s, accel, nozzle C, flow % */
} kstat;

/* printer.cfg's SPEED_MODE names; the last, BRACE, is locked */
static const char *const k_mname[5] = { "CRUSH", "WOODY", "BUZZ", "MCQUEEN", "BRACE" };

static void k_poll(const char *host, int port, kstat *k) {
    static char buf[8192];
    const char *b, *e;
    memset(k, 0, sizeof *k);
    int code = http_req(host, port, "GET", "/printer/info", buf, sizeof buf);
    k->online = code > 0;
    if (code != 200 || !(b = jobj(buf, "result", &e))) {
        snprintf(k->kstate, sizeof k->kstate, "offline");
        if ((b = jobj(buf, "error", &e))) jstr(b, e, "message", k->kmsg, sizeof k->kmsg);
        return;
    }
    jstr(b, e, "state", k->kstate, sizeof k->kstate);
    jstr(b, e, "state_message", k->kmsg, sizeof k->kmsg);
    jstr(b, e, "software_version", k->ver, sizeof k->ver);
    if (http_req(host, port, "GET",
                 "/printer/objects/query?print_stats=state,filename,print_duration,total_duration,filament_used,info,message"
                 "&extruder=temperature,target&heater_bed=temperature,target"
                 "&display_status=progress,message&fan=speed&fan_generic%20chamber_fan=speed&output_pin%20light=value"
                 "&toolhead=position,homed_axes&gcode_move=speed_factor,extrude_factor,homing_origin"
                 "&motion_report=live_position&gcode_macro%20_FIL_STATE=dir,e0,len&gcode_macro%20PAUSE=swap,fed&gcode_macro%20CLEAR_CLOG=step,pass,passes"
                 "&save_variables=variables&gcode_macro%20SPEED_MODE=modes",
                 buf, sizeof buf) != 200)
        return;
    if ((b = jobj(buf, "print_stats", &e))) {
        jstr(b, e, "state", k->pstate, sizeof k->pstate);
        jstr(b, e, "filename", k->file, sizeof k->file);
        jstr(b, e, "message", k->pmsg, sizeof k->pmsg);
        k->dur = jnum(b, e, "print_duration");
        k->tot = jnum(b, e, "total_duration");
        k->fused = jnum(b, e, "filament_used");
        k->layer = (int)jnum(b, e, "current_layer");
        k->nlayer = (int)jnum(b, e, "total_layer");
    }
    if ((b = jobj(buf, "extruder", &e))) {
        k->et = jnum(b, e, "temperature"); k->ett = jnum(b, e, "target");
    }
    if ((b = jobj(buf, "heater_bed", &e))) {
        k->bt = jnum(b, e, "temperature"); k->btt = jnum(b, e, "target");
    }
    if ((b = jobj(buf, "display_status", &e))) {
        k->prog = jnum(b, e, "progress");
        jstr(b, e, "message", k->dmsg, sizeof k->dmsg);
    }
    if ((b = jobj(buf, "fan", &e))) k->pfan = jnum(b, e, "speed");
    if ((b = jobj(buf, "fan_generic chamber_fan", &e))) k->cfan = jnum(b, e, "speed");
    /* [output_pin light] (PA4); -1 hides the button when printer.cfg has none */
    k->light = (b = jobj(buf, "output_pin light", &e)) ? jnum(b, e, "value") : -1;
    if ((b = jobj(buf, "toolhead", &e))) {
        const char *v = jval(b, e, "position");
        if (v) sscanf(v, "[%lf ,%lf ,%lf", &k->pos[0], &k->pos[1], &k->pos[2]);
        jstr(b, e, "homed_axes", k->homed, sizeof k->homed);
    }
    if ((b = jobj(buf, "gcode_move", &e))) {
        k->speed = jnum(b, e, "speed_factor");
        k->flow = jnum(b, e, "extrude_factor");
        const char *v = jval(b, e, "homing_origin");
        double o[3] = { 0 };
        if (v) sscanf(v, "[%lf ,%lf ,%lf", &o[0], &o[1], &o[2]);
        k->zoff = o[2];
    }
    if ((b = jobj(buf, "save_variables", &e))) jstr(b, e, "speed_mode", k->smode, sizeof k->smode);
    if ((b = jobj(buf, "gcode_macro SPEED_MODE", &e)))
        for (int i = 0; i < 5; i++) {
            const char *v = jval(b, e, k_mname[i]);
            if (v) sscanf(v, "[%d ,%d ,%d ,%d", &k->mode[i][0], &k->mode[i][1],
                          &k->mode[i][2], &k->mode[i][3]);
        }
    if ((b = jobj(buf, "gcode_macro _FIL_STATE", &e))) {
        k->fdir = (int)jnum(b, e, "dir");
        k->fe0 = jnum(b, e, "e0");
        k->flen = jnum(b, e, "len");
    }
    if ((b = jobj(buf, "gcode_macro PAUSE", &e))) {
        k->swap = (int)jnum(b, e, "swap");
        k->fed = (int)jnum(b, e, "fed");
    }
    if ((b = jobj(buf, "gcode_macro CLEAR_CLOG", &e))) {
        k->cstep = (int)jnum(b, e, "step");
        k->cpass = (int)jnum(b, e, "pass");
        k->cpasses = (int)jnum(b, e, "passes");
    }
    if ((b = jobj(buf, "motion_report", &e))) {
        const char *v = jval(b, e, "live_position");
        double q[4] = { 0 };
        if (v) sscanf(v, "[%lf ,%lf ,%lf ,%lf", &q[0], &q[1], &q[2], &q[3]);
        k->fe = q[3];
    }
}

/* A power cycle or SoC reboot drops the TCP link and Klipper parks in
 * shutdown until someone sends FIRMWARE_RESTART. Only link loss is cleared
 * automatically; a heater or MCU fault stays on screen for a human. */
static int k_linkloss(const kstat *k) {
    if (strcmp(k->kstate, "shutdown") && strcmp(k->kstate, "error")) return 0;
    return strstr(k->kmsg, "Lost communication") || strstr(k->kmsg, "Unable to open") ||
           strstr(k->kmsg, "Unable to connect");
}

static int k_busy(const kstat *k) {
    return !strcmp(k->pstate, "printing") || !strcmp(k->pstate, "paused");
}

/* the menus exist only while Klipper is ready and no job is running */
static int k_idle(const kstat *k) {
    return k->online && !strcmp(k->kstate, "ready") && !k_busy(k);
}

/* J8: a finished, cancelled or failed job stays on the home screen until
 * OK or the next job */
static int k_ended(const kstat *k) {
    return k_idle(k) && k->file[0] && (!strcmp(k->pstate, "complete") ||
           !strcmp(k->pstate, "cancelled") || !strcmp(k->pstate, "error"));
}

/* ---- local files: Moonraker's gcodes root, newest first */
#define K_MAXF 64
#define K_ROWS 5
typedef struct { char name[128]; double mod, size; } kfile;
static kfile kf[K_MAXF];
static int nkf;

static int kf_cmp(const void *a, const void *b) {
    double d = ((const kfile *)b)->mod - ((const kfile *)a)->mod;
    return (d > 0) - (d < 0);
}

/* Returns the number of files, or -1 when Moonraker did not answer. */
static int k_files(const char *host, int port) {
    static char buf[65536];
    nkf = 0;
    if (http_req(host, port, "GET", "/server/files/list?root=gcodes", buf, sizeof buf) != 200)
        return -1;
    for (const char *p = buf; nkf < K_MAXF && (p = strstr(p, "{\"path\":")); ) {
        const char *e = strchr(p, '}');
        if (!e) break;                 /* cut off at the buffer's end */
        kfile *f = &kf[nkf];
        jstr(p, e, "path", f->name, sizeof f->name);
        f->mod = jnum(p, e, "modified");
        f->size = jnum(p, e, "size");
        if (f->name[0]) nkf++;
        p = e;
    }
    qsort(kf, nkf, sizeof *kf, kf_cmp);
    return nkf;
}

static void urlenc(const char *s, char *o, int n) {
    int j = 0;
    for (; *s && j < n - 4; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || strchr("-_./~", c))
            o[j++] = (char)c;
        else
            j += sprintf(o + j, "%%%02X", c);
    }
    o[j] = 0;
}

/* ---- a USB stick in the printer (P6): vfat only (the SoC's kernel has
 * nothing else), mounted read-only while USB is open; G-code at the top
 * level. A print copies the file to Moonraker's gcodes/usb/, then starts it.
 * The stick must never hold Adventurer4*.tgz or flashforge_init.sh: the
 * SoC's boot script installs or runs those from any stick present. */
#define K_USBDIR "/tmp/usb"
typedef struct { char name[256]; double mod, size; } kufile;
static kufile ku[K_MAXF];
static int nku, g_usbmnt;

/* the stick's block device, or NULL; AD4_USB_DIR fakes one for --dump */
static const char *k_usbdev(void) {
    static const char *const dev[] = { "/dev/sda1", "/dev/sda" };
    if (getenv("AD4_USB_DIR")) return "dump";
    for (int i = 0; i < 2; i++)
        if (access(dev[i], F_OK) == 0) return dev[i];
    return NULL;
}

static const char *k_usbdir(void) { return g_env("AD4_USB_DIR", K_USBDIR); }

static void k_usbumount(void) {
    if (g_usbmnt) umount2(K_USBDIR, MNT_DETACH);
    g_usbmnt = 0;
}

static int ku_cmp(const void *a, const void *b) {
    double d = ((const kufile *)b)->mod - ((const kufile *)a)->mod;
    return (d > 0) - (d < 0);
}

/* Returns the number of G-code files, -1 with no stick, -2 when it would
 * not mount (not FAT32, or no partition). */
static int k_usb(void) {
    const char *dev = k_usbdev();
    nku = 0;
    if (!dev) return -1;
    if (strcmp(dev, "dump") && !g_usbmnt) {
        mkdir(K_USBDIR, 0755);
        if (mount(dev, K_USBDIR, "vfat", MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOEXEC, "") != 0)
            return -2;
        g_usbmnt = 1;
    }
    DIR *d = opendir(k_usbdir());
    if (!d) return -2;
    struct dirent *e;
    while (nku < K_MAXF && (e = readdir(d))) {
        int l = (int)strlen(e->d_name);
        if (e->d_name[0] == '.' || l < 3 || l >= (int)sizeof ku[0].name) continue;
        const char *x = strrchr(e->d_name, '.');
        if (!x || (strcasecmp(x, ".gcode") && strcasecmp(x, ".gco") && strcasecmp(x, ".g"))) continue;
        char p[320];
        struct stat st;
        snprintf(p, sizeof p, "%s/%s", k_usbdir(), e->d_name);
        if (stat(p, &st) || !S_ISREG(st.st_mode)) continue;
        snprintf(ku[nku].name, sizeof ku[nku].name, "%s", e->d_name);
        ku[nku].mod = (double)st.st_mtime;
        ku[nku].size = (double)st.st_size;
        nku++;
    }
    closedir(d);
    qsort(ku, nku, sizeof *ku, ku_cmp);
    return nku;
}

static int k_wall(int fd, const char *b, long n) {
    while (n > 0) {
        ssize_t w = write(fd, b, n);
        if (w <= 0) return -1;
        b += w; n -= w;
    }
    return 0;
}

/* Streams the stick's file to Moonraker as gcodes/usb/<name> (one multipart
 * POST, 16 KB at a time; prog() repaints between chunks). Returns the HTTP
 * status (201 on success) with Moonraker's path for it in out. */
static int k_upload(const char *host, int port, const char *name, char *out, int outn,
                    void (*prog)(void *, double, double), void *ctx) {
    static const char bd[] = "ad4usbad4usbad4usb";
    char src[320], fn[256], head[1024], tail[64];
    static char buf[16384];
    out[0] = 0;
    snprintf(src, sizeof src, "%s/%s", k_usbdir(), name);
    snprintf(fn, sizeof fn, "%s", name);
    for (char *q = fn; *q; q++) if (*q == '"' || *q == '\\' || *q < ' ') *q = '_';
    int in = open(src, O_RDONLY);
    if (in < 0) return -1;
    struct stat st;
    if (fstat(in, &st)) { close(in); return -1; }
    int hb = snprintf(buf, sizeof buf,
        "--%s\r\nContent-Disposition: form-data; name=\"root\"\r\n\r\ngcodes\r\n"
        "--%s\r\nContent-Disposition: form-data; name=\"path\"\r\n\r\nusb\r\n"
        "--%s\r\nContent-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\n"
        "Content-Type: application/octet-stream\r\n\r\n", bd, bd, bd, fn);
    int tb = snprintf(tail, sizeof tail, "\r\n--%s--\r\n", bd);
    int h = snprintf(head, sizeof head,
        "POST /server/files/upload HTTP/1.0\r\nHost: %s:%d\r\n"
        "Content-Type: multipart/form-data; boundary=%s\r\nContent-Length: %lld\r\n\r\n",
        host, port, bd, (long long)st.st_size + hb + tb);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    int fd = inet_pton(AF_INET, host, &a.sin_addr) == 1 ? socket(AF_INET, SOCK_STREAM, 0) : -1;
    struct timeval tv = { 15, 0 };
    if (fd >= 0) {
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    }
    if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof a) ||
        k_wall(fd, head, h) || k_wall(fd, buf, hb)) {
        if (fd >= 0) close(fd);
        close(in);
        return -1;
    }
    double done = 0;
    long t = 0;
    ssize_t r;
    while ((r = read(in, buf, sizeof buf)) > 0) {
        if (k_wall(fd, buf, r)) break;
        done += r;
        if (prog && now_ms() - t > 250) { prog(ctx, done, (double)st.st_size); t = now_ms(); }
    }
    close(in);
    if (done != (double)st.st_size || k_wall(fd, tail, tb)) { close(fd); return -1; }
    if (prog) prog(ctx, done, done);
    /* Moonraker answers once the file is written and its metadata read */
    tv.tv_sec = 60;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    int got = 0;
    while (got < (int)sizeof buf - 1 && (r = read(fd, buf + got, sizeof buf - 1 - got)) > 0)
        got += (int)r;
    close(fd);
    buf[got] = 0;
    int code = -1;
    const char *b, *e;
    sscanf(buf, "HTTP/%*s %d", &code);
    if ((b = jobj(buf, "item", &e))) jstr(b, e, "path", out, outn);
    return code;
}

typedef struct {
    int ok;
    double est, fil, grams, lh, ext, bed, layers, height;
    char type[24], slicer[32];
    char thumb[160];                  /* relative to the file's folder */
} kmeta;

static void k_meta_at(const char *b, const char *e, kmeta *m) {
    m->ok = 1;
    m->est = jnum(b, e, "estimated_time");
    m->fil = jnum(b, e, "filament_total");
    m->grams = jnum(b, e, "filament_weight_total");
    m->lh = jnum(b, e, "layer_height");
    m->ext = jnum(b, e, "first_layer_extr_temp");
    m->bed = jnum(b, e, "first_layer_bed_temp");
    m->layers = jnum(b, e, "layer_count");
    m->height = jnum(b, e, "object_height");
    jstr(b, e, "filament_type", m->type, sizeof m->type);
    jstr(b, e, "slicer", m->slicer, sizeof m->slicer);
    /* the thumbnail nearest 160 px wide (the job screen draws it at 160) */
    int bw = 0;
    for (const char *p = b; (p = strstr(p, "\"relative_path\":")) && p < e; p++) {
        const char *o = p, *c = strchr(p, '}');
        while (o > b && *o != '{') o--;
        if (!c || c > e) break;
        int w = (int)jnum(o, c, "width");
        if (!bw || abs(w - 160) < abs(bw - 160)) {
            bw = w;
            jstr(o, c, "relative_path", m->thumb, sizeof m->thumb);
        }
    }
}

static void k_meta(const char *host, int port, const char *name, kmeta *m) {
    static char buf[16384];
    char path[640], en[400];
    const char *b, *e;
    memset(m, 0, sizeof *m);
    urlenc(name, en, sizeof en);
    snprintf(path, sizeof path, "/server/files/metadata?filename=%s", en);
    if (http_req(host, port, "GET", path, buf, sizeof buf) == 200 &&
        (b = jobj(buf, "result", &e)))
        k_meta_at(b, e, m);
}

/* ---- P3: the last jobs from Moonraker's history, newest first */
typedef struct { char name[128], status[20]; double end, dur, fused; int exists; } kjob;
static kjob kh[K_ROWS];

/* Returns the number of jobs, or -1 when Moonraker did not answer. Each
 * job's metadata goes to meta[i] (for the thumbnail and grams). */
static int k_hist(const char *host, int port, kmeta *meta) {
    static char buf[32768];
    int n = 0;
    if (http_req(host, port, "GET", "/server/history/list?limit=5&order=desc", buf, sizeof buf) != 200)
        return -1;
    for (const char *p = strstr(buf, "{\"job_id\":"); p && n < K_ROWS; n++) {
        const char *q = strstr(p + 1, "{\"job_id\":"), *e = q ? q : buf + strlen(buf), *mb, *me;
        kjob *j = &kh[n];
        jstr(p, e, "filename", j->name, sizeof j->name);
        jstr(p, e, "status", j->status, sizeof j->status);
        j->end = jnum(p, e, "end_time");
        j->dur = jnum(p, e, "print_duration");
        j->fused = jnum(p, e, "filament_used");
        const char *v = jval(p, e, "exists");
        j->exists = v && *v == 't';
        memset(&meta[n], 0, sizeof meta[n]);
        if ((mb = jobj(p, "metadata", &me)) && me < e) k_meta_at(mb, me, &meta[n]);
        p = q;
    }
    return n;
}

typedef struct { unsigned char *px; int w, h; } kimg;

static void k_thumb(const char *host, int port, const char *name, const kmeta *m, kimg *im) {
    static char buf[131072];
    char rel[400], path[1300], en[1200];
    const char *sl = strrchr(name, '/');
    stbi_image_free(im->px);
    memset(im, 0, sizeof *im);
    if (!m->thumb[0]) return;
    snprintf(rel, sizeof rel, "%.*s%s", sl ? (int)(sl - name + 1) : 0, name, m->thumb);
    urlenc(rel, en, sizeof en);
    snprintf(path, sizeof path, "/server/files/gcodes/%s", en);
    if (http_req(host, port, "GET", path, buf, sizeof buf) != 200) return;
    int n;
    im->px = stbi_load_from_memory((unsigned char *)buf, g_blen, &im->w, &im->h, &n, 4);
}

/* sz x sz, alpha over bg: nearest-neighbour going up, box average going
 * down (the Print list draws a 160 px thumbnail at 56) */
static void k_img(const surf *s, int x, int y, int sz, const kimg *im, unsigned bg) {
    for (int j = 0; j < sz; j++)
        for (int i = 0; i < sz; i++) {
            int x0 = i * im->w / sz, x1 = (i + 1) * im->w / sz;
            int y0 = j * im->h / sz, y1 = (j + 1) * im->h / sz;
            if (x1 <= x0) x1 = x0 + 1;
            if (y1 <= y0) y1 = y0 + 1;
            unsigned r = 0, g = 0, bl = 0, a = 0, n = 0;
            for (int v = y0; v < y1; v++)
                for (int u = x0; u < x1; u++, n++) {
                    const unsigned char *q = im->px + 4 * ((size_t)v * im->w + u);
                    r += q[0] * q[3]; g += q[1] * q[3]; bl += q[2] * q[3]; a += q[3];
                }
            if (!a) { px(s, x + i, y + j, bg); continue; }
            unsigned al = a / n;
            px(s, x + i, y + j, mix(bg, (r / a) << 16 | (g / a) << 8 | bl / a, al + (al >> 7)));
        }
}

/* Seconds left, -1 if unknown. With a slicer estimate: what the file still
 * has to print, scaled by how far the job has run behind or ahead of plan.
 * Without one, extrapolate elapsed time once there is enough of it. */
static double k_left(const kstat *k, const kmeta *m) {
    double p = k->prog;
    if (m->est > 0) {
        if (p < 0.01 || k->dur < 60) return m->est * (1 - p);
        double d = k->dur / (m->est * p);
        d = d < 0.5 ? 0.5 : d > 3 ? 3 : d;
        return m->est * (1 - p) * ((1 - p) + p * d);
    }
    return p > 0.05 ? k->dur * (1 - p) / p : -1;
}

static void k_hm(double sec, char *o, int n) {
    int mn = (int)(sec / 60 + 0.5);
    if (mn >= 60) snprintf(o, n, "%d h %02d m", mn / 60, mn % 60);
    else if (mn > 0) snprintf(o, n, "%d min", mn);
    else snprintf(o, n, "< 1 min");
}

/* ---- screens. Buttons whose token starts with '/' POST that path to
 * Moonraker; the rest move between screens. */
/* ---- J7: the job's objects from exclude_object (needs "label objects" in the slicer) */
#define K_MAXO 6
#define K_MAXP 24
typedef struct {
    char name[64];
    double cx, cy;
    int np, excl, cur;
    double p[K_MAXP][2];
} kobj;
static kobj ko[K_MAXO];

/* Returns the number of objects, or -1 when Moonraker did not answer. */
static int k_objs(const char *host, int port) {
    static char buf[32768];
    const char *b, *e;
    int n = 0;
    if (http_req(host, port, "GET", "/printer/objects/query?exclude_object", buf, sizeof buf) != 200 ||
        !(b = jobj(buf, "exclude_object", &e)))
        return -1;
    char cur[64] = "";
    jstr(b, e, "current_object", cur, sizeof cur);
    const char *xl = jval(b, e, "excluded_objects"), *xe = xl ? strchr(xl, ']') : NULL;
    const char *p = jval(b, e, "objects");
    if (!p || *p != '[') return 0;
    for (p++; n < K_MAXO; ) {
        while (*p == ' ' || *p == ',') p++;
        if (*p != '{') break;
        const char *q = strchr(p, '}');
        if (!q) break;
        kobj *o = &ko[n];
        memset(o, 0, sizeof *o);
        jstr(p, q, "name", o->name, sizeof o->name);
        const char *v = jval(p, q, "center");
        if (v) sscanf(v, "[%lf ,%lf", &o->cx, &o->cy);
        if ((v = jval(p, q, "polygon")))
            for (const char *r = v + 1; o->np < K_MAXP && (r = strchr(r, '[')) && r < q; r++)
                if (sscanf(r, "[%lf ,%lf", &o->p[o->np][0], &o->p[o->np][1]) == 2) o->np++;
        char pat[72];
        snprintf(pat, sizeof pat, "\"%s\"", o->name);
        const char *x = xl ? strstr(xl, pat) : NULL;
        o->excl = x && x < xe;
        o->cur = !strcmp(o->name, cur);
        if (o->name[0]) n++;
        p = q + 1;
    }
    return n;
}

/* "block.stl_id_0_copy_0" -> "block" */
static void k_oname(const char *n, char *o, int sz) {
    int l = (int)strlen(n);
    const char *c = strstr(n, ".stl");
    if (!c) c = strstr(n, "_id_");
    if (c) l = (int)(c - n);
    snprintf(o, sz, "%.*s", l, n);
}

enum { K_HOME, K_FILES, K_FILE, K_HEAT, K_TOOLS, K_ABOUT, K_MOVE, K_TUNE, K_OBJS, K_HIST, K_USB,
       K_MODE, K_PAD, K_BRACE, K_FANS, K_HJOB };
typedef struct {
    int scr, page, sel, nfiles, step, nobj, nhist, nusb;
    kmeta meta; kimg img;             /* the file picked in PRINT */
    char jfile[96]; kmeta jmeta; kimg jimg; long jt;   /* the running job */
    kmeta rmeta[K_ROWS]; kimg rimg[K_ROWS];            /* the Print list's or History's rows */
    int ack;                          /* the finished job's card was dismissed */
    double rate[3];                   /* $/kg, g/cm3, $/printer hour; 0 = not set */
    int mback;                        /* SPEED MODE's BACK: HOME, TOOLS or TUNE */
    char pad[12]; int padbad;         /* the gamepad's presses so far, a wrong one */
    int hjob, hsrc, hback;            /* PAST JOB: kh[] row or the last job, and its BACK */
    kjob last; kmeta lmeta; kimg limg; int nlast; long lt;   /* home's last job */
} kmenu;

/* MOVE's step, mm */
static const double k_step[] = { 0.1, 1, 10, 50 };
#define K_GS "/printer/gcode/script?script="

/* ------------------------------------------------------------ look (P7) */
/* Flat and dark: Barlow (SIL OFL 1.1, fonts/OFL.txt) rasterised by
 * stb_truetype v1.26, anti-aliased shapes, stroked line icons, one accent
 * and colour only for state. The SoC has no FPU, so glyphs, icons and
 * rounded corners are rendered once and cached. */
#define STB_TRUETYPE_IMPLEMENTATION
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "stb_truetype.h"
#pragma GCC diagnostic pop
#include <math.h>

__asm__(".pushsection .rodata\n"
        ".balign 4\n.global ad4_ttf_med\nad4_ttf_med:\n.incbin \"fonts/Barlow-Medium.ttf\"\n"
        ".balign 4\n.global ad4_ttf_semi\nad4_ttf_semi:\n.incbin \"fonts/Barlow-SemiBold.ttf\"\n"
        ".balign 4\n.global ad4_ttf_cond\nad4_ttf_cond:\n.incbin \"fonts/BarlowCondensed-SemiBold.ttf\"\n"
        ".popsection\n");
extern const unsigned char ad4_ttf_med[], ad4_ttf_semi[], ad4_ttf_cond[];

#define U_SCR      0x0e1318u
#define U_SURF     0x151c23u
#define U_SURF2    0x1d262fu
#define U_LINE     0x27323cu
#define U_FG       0xe8edf1u
#define U_MUT      0x8d9ba7u
#define U_DIM      0x55626du
#define U_ACC      0x4ea4ffu
#define U_ACC_INK  0x04213du
#define U_GO       0x2fbf71u
#define U_GO_INK   0x03210fu
#define U_STOP     0xe8504cu
#define U_STOP_INK 0xffffffu
#define U_WARN     0xf3a52bu
#define U_WARN_INK 0x2b1900u
#define U_HEAT     0xff7b3au

/* ---- pixels with coverage */
static unsigned pget(const surf *s, int x, int y) {
    int fx = x, fy = y;
    if (s->rot) { fx = s->fw - 1 - y; fy = x; }
    const unsigned char *p = s->mem + (size_t)fy * s->stride + (size_t)fx * (s->bpp / 8);
    unsigned v;
    if (s->bpp == 16) { unsigned short t; memcpy(&t, p, 2); v = t; }
    else memcpy(&v, p, 4);
    unsigned r = v >> s->ro & ((1u << s->rl) - 1), g = v >> s->go & ((1u << s->gl) - 1),
             b = v >> s->bo & ((1u << s->bl) - 1);
    r = r << (8 - s->rl) | r >> (2 * s->rl - 8);
    g = g << (8 - s->gl) | g >> (2 * s->gl - 8);
    b = b << (8 - s->bl) | b >> (2 * s->bl - 8);
    return r << 16 | g << 8 | b;
}

static int g_op = 256;                /* opacity of everything drawn: 82 = disabled */

static void pxa(const surf *s, int x, int y, unsigned c, int a) {   /* a 0..256 */
    if (x < 0 || y < 0 || x >= s->w || y >= s->h || a <= 0) return;
    a = a * g_op >> 8;
    if (a >= 256) px(s, x, y, c);
    else if (a > 0) px(s, x, y, mix(pget(s, x, y), c, a));
}

/* a quarter circle's coverage, r x r, (0,0) the outer corner; t2 = ring
 * thickness x 2, 0 = filled */
static const unsigned char *k_corner(int r, int t2) {
    static unsigned char *tab[65][8];
    if (r > 64) r = 64;
    if (t2 > 7) t2 = 7;
    if (!tab[r][t2]) {
        unsigned char *p = malloc(r * r + 1);
        if (!p) return NULL;
        for (int j = 0; j < r; j++)
            for (int i = 0; i < r; i++) {
                float dx = r - (i + .5f), dy = r - (j + .5f), d = sqrtf(dx * dx + dy * dy);
                float o = r + .5f - d;
                o = o < 0 ? 0 : o > 1 ? 1 : o;
                if (t2) {
                    float n = r - t2 / 2.0f + .5f - d;
                    o -= n < 0 ? 0 : n > 1 ? 1 : n;
                    if (o < 0) o = 0;
                }
                p[j * r + i] = (unsigned char)(o * 255 + .5f);
            }
        tab[r][t2] = p;
    }
    return tab[r][t2];
}

static void rr(const surf *s, int x, int y, int w, int h, int r, unsigned c) {
    if (w <= 0 || h <= 0) return;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r > 64) r = 64;
    const unsigned char *t = k_corner(r, 0);
    for (int j = 0; j < h; j++) {
        int cj = j < r ? j : j >= h - r ? h - 1 - j : -1;
        for (int i = 0; i < w; i++) {
            int ci = i < r ? i : i >= w - r ? w - 1 - i : -1, a = 256;
            if (ci >= 0 && cj >= 0 && t) { a = t[cj * r + ci]; a += a >> 7; }
            pxa(s, x + i, y + j, c, a);
        }
    }
}

static void rring(const surf *s, int x, int y, int w, int h, int r, int t2, unsigned c) {
    if (w <= 0 || h <= 0) return;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r > 64) r = 64;
    const unsigned char *t = k_corner(r, t2);
    int tb = (t2 + 1) / 2;
    for (int j = 0; j < h; j++) {
        int cj = j < r ? j : j >= h - r ? h - 1 - j : -1;
        for (int i = 0; i < w; i++) {
            if (cj < 0 && i >= tb && i < w - tb) { i = w - tb - 1; continue; }
            int ci = i < r ? i : i >= w - r ? w - 1 - i : -1, a;
            if (ci >= 0 && cj >= 0) { a = t ? t[cj * r + ci] : 0; a += a >> 7; }
            else {
                int o = i, q = w - 1 - i;
                if (q < o) o = q;
                if (j < o) o = j;
                if (h - 1 - j < o) o = h - 1 - j;
                a = t2 * 128 - o * 256;
                a = a > 256 ? 256 : a;
            }
            pxa(s, x + i, y + j, c, a);
        }
    }
}

/* ---- text */
typedef struct { int f, px, ls; } ufont;      /* 0 Medium, 1 SemiBold, 2 Condensed; ls in 1/10 px */
#define UF(f, p, l) ((ufont){ f, p, l })
typedef struct { int f, px, cp, adv, x0, y0, w, h; unsigned char *bm; } uglyph;
#define U_NG 4096
static uglyph g_gl[U_NG];
static stbtt_fontinfo g_ft[3];
static int g_ftok;

static uglyph *u_glyph(int f, int sz, int cp) {
    if (!g_ftok) {
        const unsigned char *d[3] = { ad4_ttf_med, ad4_ttf_semi, ad4_ttf_cond };
        for (int i = 0; i < 3; i++) stbtt_InitFont(&g_ft[i], d[i], 0);
        g_ftok = 1;
    }
    unsigned h = ((unsigned)cp * 2654435761u ^ (unsigned)sz * 40503u ^ (unsigned)f * 977u) & (U_NG - 1);
    for (int n = 0; n < U_NG; n++, h = (h + 1) & (U_NG - 1)) {
        uglyph *g = &g_gl[h];
        if (g->px && g->f == f && g->px == sz && g->cp == cp) return g;
        if (g->px) continue;
        const stbtt_fontinfo *fi = &g_ft[f];
        float sc = stbtt_ScaleForMappingEmToPixels(fi, (float)sz);
        int adv, lsb;
        stbtt_GetCodepointHMetrics(fi, cp, &adv, &lsb);
        g->f = f; g->px = sz; g->cp = cp;
        g->adv = (int)(adv * sc * 64 + .5f);
        g->bm = stbtt_GetCodepointBitmap(fi, sc, sc, cp, &g->w, &g->h, &g->x0, &g->y0);
        return g;
    }
    return NULL;
}

static int u_dec(const char **p) {
    const unsigned char *s = (const unsigned char *)*p;
    int c = *s++;
    if (c >= 0xf0 && (s[0] & 0xc0) == 0x80 && (s[1] & 0xc0) == 0x80 && (s[2] & 0xc0) == 0x80) {
        c = (c & 7) << 18 | (s[0] & 63) << 12 | (s[1] & 63) << 6 | (s[2] & 63); s += 3;
    } else if (c >= 0xe0 && (s[0] & 0xc0) == 0x80 && (s[1] & 0xc0) == 0x80) {
        c = (c & 15) << 12 | (s[0] & 63) << 6 | (s[1] & 63); s += 2;
    } else if (c >= 0xc0 && (s[0] & 0xc0) == 0x80) {
        c = (c & 31) << 6 | (s[0] & 63); s += 1;
    } else if (c >= 0x80) c = '?';
    *p = (const char *)s;
    return c < 32 ? ' ' : c;
}

static int u_tw(ufont F, const char *t) {
    int w = 0, n = 0;
    while (*t) {
        uglyph *g = u_glyph(F.f, F.px, u_dec(&t));
        if (g) w += g->adv;
        w += F.ls * 64 / 10;
        n++;
    }
    if (n) w -= F.ls * 64 / 10;
    return (w + 32) >> 6;
}

/* y is the baseline; al 0 left, 1 centre, 2 right; returns the right edge.
 * s == NULL measures only. */
static int u_text(const surf *s, int x, int y, ufont F, const char *t, unsigned c, int al) {
    int w = u_tw(F, t);
    if (al == 1) x -= w / 2;
    else if (al == 2) x -= w;
    if (!s) return x + w;
    int pen = x * 64;
    while (*t) {
        uglyph *g = u_glyph(F.f, F.px, u_dec(&t));
        if (!g) continue;
        if (g->bm)
            for (int j = 0; j < g->h; j++)
                for (int i = 0; i < g->w; i++) {
                    int a = g->bm[j * g->w + i];
                    if (a) pxa(s, (pen >> 6) + g->x0 + i, y + g->y0 + j, c, a + (a >> 7));
                }
        pen += g->adv + F.ls * 64 / 10;
    }
    return x + w;
}

/* t, cut with an ellipsis to fit maxw */
static const char *u_fit(ufont F, const char *t, int maxw) {
    static char buf[4][200];
    static int bi;
    if (u_tw(F, t) <= maxw) return t;
    char *b = buf[bi++ & 3];
    int n = snprintf(b, 196, "%s", t);
    if (n > 195) n = 195;
    while (n > 0) {
        n--;
        while (n > 0 && ((unsigned char)b[n] & 0xc0) == 0x80) n--;
        int e = n;
        while (e > 0 && b[e - 1] == ' ') e--;
        memcpy(b + e, "\xe2\x80\xa6", 4);
        if (u_tw(F, b) <= maxw) break;
    }
    return b;
}

/* Greedy wrap, breaking after a space, '_' or '-', else anywhere (file names
 * have no spaces); the last allowed line gets an ellipsis. Returns the lines
 * used; s == NULL measures only. */
static int u_wrap(const surf *s, int x, int y, ufont F, const char *t, unsigned c,
                  int maxw, int lh, int maxl, int al) {
    char ln[200];
    int nl = 0;
    while (*t == ' ') t++;
    while (*t && nl < maxl) {
        int len = (int)strlen(t);
        if (nl == maxl - 1 || u_tw(F, t) <= maxw) {
            u_text(s, x, y, F, u_fit(F, t, maxw), c, al);
            return nl + 1;
        }
        int best = 0, brk = 0;
        for (int i = 1; i <= len && i < 199; i++) {
            if (i < len && ((unsigned char)t[i] & 0xc0) == 0x80) continue;
            memcpy(ln, t, i);
            ln[i] = 0;
            if (u_tw(F, ln) > maxw) break;
            best = i;
            if (t[i - 1] == ' ' || t[i - 1] == '_' || t[i - 1] == '-') brk = i;
        }
        int take = brk ? brk : best ? best : 1;
        memcpy(ln, t, take);
        ln[take] = 0;
        for (int e = take; e > 0 && ln[e - 1] == ' '; ) ln[--e] = 0;
        u_text(s, x, y, F, ln, c, al);
        t += take;
        while (*t == ' ') t++;
        y += lh;
        nl++;
    }
    return nl;
}

/* ---- icons: SVG path data on a 24 grid, stroke 2, round caps and joins.
 * O cx cy r is a circle, R x y w h a rectangle; '!' scales the tools icon
 * down, '#' fills instead of stroking. */
enum { I_HOME, I_PRINT, I_HEAT, I_TOOLS, I_BACK, I_CHEV, I_LIGHT, I_FAN, I_MOVE, I_GAUGE, I_POWER,
       I_INFO, I_SPOOL, I_PAUSE, I_PLAY, I_X, I_REDO, I_TUNE, I_USB, I_LOCK, I_CHECK, I_OBJS,
       I_HOMEAX, I_AU, I_AD, I_AL, I_AR, I_N };
static const char *const k_icons[I_N] = {
    "M3 11l9-8 9 8M5 9.5V20h5v-6h4v6h5V9.5",
    "M12 3l8 4.5v9L12 21l-8-4.5v-9zM4 7.5l8 4.5 8-4.5M12 12v9",
    "M12 3c.5 3 4.5 5 4.5 10a4.5 4.5 0 0 1-9 0c0-2.2 1-3.6 2.2-4.6.1 1.8.8 2.9 1.8 3.4C11.2 9 11 6 12 3z",
    "!M14.5 6.5a4 4 0 0 0 5 5L21 13l-8 8-3-3 1.5-1.5a4 4 0 0 0-5-5L3 10l3-3 3 3 1.5-1.5a4 4 0 0 0 5-5z",
    "M15 4l-8 8 8 8",
    "M9 5l7 7-7 7",
    "M9 18h6M10 21h4M12 3a6 6 0 0 0-3.8 10.6c.8.7 1.3 1.6 1.3 2.4h5c0-.8.5-1.7 1.3-2.4A6 6 0 0 0 12 3z",
    "O12 12 2M12 10c-1-4 0-7 3-7s2.5 4.5-1.5 7.5M14 12c4-1 7 0 7 3s-4.5 2.5-7.5-1.5"
    "M12 14c1 4 0 7-3 7s-2.5-4.5 1.5-7.5M10 12c-4 1-7 0-7-3s4.5-2.5 7.5 1.5",
    "M12 3v18M3 12h18M9 6l3-3 3 3M9 18l3 3 3-3M6 9l-3 3 3 3M18 9l3 3-3 3",
    "M4.5 18a9 9 0 1 1 15 0M12 14l4.5-5O12 14.5 1.3",
    "M12 3v8M7 6.3a7.5 7.5 0 1 0 10 0",
    "O12 12 9M12 11v6M12 7.5v.5",
    "O12 12 9O12 12 3M12 3v6M12 15v6",
    "M9 5v14M15 5v14",
    "M7 4.5l12 7.5-12 7.5z",
    "M6 6l12 12M18 6L6 18",
    "M4 12a8 8 0 1 0 2.5-5.8M4 4v5h5",
    "M4 6h9M17 6h3M4 12h3M11 12h9M4 18h11M19 18h1O15 6 2O9 12 2O17 18 2",
    "M12 21V5M8.5 7.5L12 3l3.5 4.5M12 15l-5-3V9M12 13l5-3V8O7 8 1.2R15.8 6 2.4 2.4",
    "R5 11 14 10M8 11V8a4 4 0 0 1 8 0v3",
    "M5 12.5l4.5 4.5L19 7.5",
    "R3.5 3.5 7 7R13.5 3.5 7 7R3.5 13.5 7 7M14 14l6 6M20 14l-6 6",
    "M4 11l8-7 8 7M6 10v10h12V10",
    "#M12 5L20 17H4z",
    "#M12 19L4 7h16z",
    "#M5 12L17 4v16z",
    "#M19 12L7 20V4z",
};

typedef struct { float x0, y0, x1, y1; } iseg;
static iseg g_is[400];
static int g_nis;
static float g_itk = 1, g_ito;

static void is_add(float x0, float y0, float x1, float y1) {
    if (g_nis < 400)
        g_is[g_nis++] = (iseg){ (x0 + g_ito) * g_itk, (y0 + g_ito) * g_itk,
                                (x1 + g_ito) * g_itk, (y1 + g_ito) * g_itk };
}

static void is_cubic(float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3) {
    float px0 = x0, py0 = y0;
    for (int i = 1; i <= 12; i++) {
        float t = i / 12.0f, u = 1 - t;
        float x = u * u * u * x0 + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t * x3;
        float y = u * u * u * y0 + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t * y3;
        is_add(px0, py0, x, y);
        px0 = x; py0 = y;
    }
}

/* SVG F.6.5: endpoints to centre, no x-axis rotation */
static void is_arc(float x1, float y1, float rx, float ry, int fa, int fs, float x2, float y2) {
    float dx = (x1 - x2) / 2, dy = (y1 - y2) / 2;
    rx = fabsf(rx); ry = fabsf(ry);
    if (rx < 1e-4f || ry < 1e-4f) { is_add(x1, y1, x2, y2); return; }
    float L = dx * dx / (rx * rx) + dy * dy / (ry * ry);
    if (L > 1) { float q = sqrtf(L); rx *= q; ry *= q; }
    float num = rx * rx * ry * ry - rx * rx * dy * dy - ry * ry * dx * dx;
    float den = rx * rx * dy * dy + ry * ry * dx * dx;
    float co = den > 0 && num > 0 ? sqrtf(num / den) : 0;
    if (fa == fs) co = -co;
    float cxp = co * rx * dy / ry, cyp = -co * ry * dx / rx;
    float cx = cxp + (x1 + x2) / 2, cy = cyp + (y1 + y2) / 2;
    float t1 = atan2f((dy - cyp) / ry, (dx - cxp) / rx);
    float t2 = atan2f((-dy - cyp) / ry, (-dx - cxp) / rx), dt = t2 - t1;
    if (fs && dt < 0) dt += 6.2831853f;
    else if (!fs && dt > 0) dt -= 6.2831853f;
    int n = (int)(fabsf(dt) * 6) + 2;
    float px0 = x1, py0 = y1;
    for (int i = 1; i <= n; i++) {
        float t = t1 + dt * i / n, x = cx + rx * cosf(t), y = cy + ry * sinf(t);
        if (i == n) { x = x2; y = y2; }
        is_add(px0, py0, x, y);
        px0 = x; py0 = y;
    }
}

static int ip_num(const char **p, float *v) {
    while (**p == ' ' || **p == ',') (*p)++;
    char *e;
    *v = strtof(*p, &e);
    if (e == *p) return 0;
    *p = e;
    return 1;
}

static void ip_path(const char *d) {
    float cx = 0, cy = 0, sx = 0, sy = 0, qx = 0, qy = 0, a[7];
    char cmd = 0, last = 0;
    g_nis = 0; g_itk = 1; g_ito = 0;
    if (*d == '#') d++;
    if (*d == '!') { g_itk = .9f; g_ito = 1.3f; d++; }
    while (*d) {
        while (*d == ' ' || *d == ',') d++;
        if (!*d) break;
        int lit = ((*d | 32) >= 'a' && (*d | 32) <= 'z');
        if (lit) cmd = *d++;
        else if (cmd == 'M') cmd = 'L';
        else if (cmd == 'm') cmd = 'l';
        char C = cmd & ~32;
        int rel = cmd >= 'a';
        int need = C == 'M' || C == 'L' ? 2 : C == 'H' || C == 'V' ? 1 : C == 'C' ? 6 :
                   C == 'S' ? 4 : C == 'A' ? 7 : C == 'O' ? 3 : C == 'R' ? 4 : 0;
        if (!need && !lit) return;
        for (int n = 0; n < need; n++) if (!ip_num(&d, &a[n])) return;
        float ox = rel ? cx : 0, oy = rel ? cy : 0;
        switch (C) {
        case 'M': cx = sx = a[0] + ox; cy = sy = a[1] + oy; break;
        case 'L': is_add(cx, cy, a[0] + ox, a[1] + oy); cx = a[0] + ox; cy = a[1] + oy; break;
        case 'H': is_add(cx, cy, a[0] + ox, cy); cx = a[0] + ox; break;
        case 'V': is_add(cx, cy, cx, a[0] + oy); cy = a[0] + oy; break;
        case 'C':
            is_cubic(cx, cy, a[0] + ox, a[1] + oy, a[2] + ox, a[3] + oy, a[4] + ox, a[5] + oy);
            qx = a[2] + ox; qy = a[3] + oy; cx = a[4] + ox; cy = a[5] + oy;
            break;
        case 'S': {
            float x1 = last == 'C' || last == 'S' ? 2 * cx - qx : cx;
            float y1 = last == 'C' || last == 'S' ? 2 * cy - qy : cy;
            is_cubic(cx, cy, x1, y1, a[0] + ox, a[1] + oy, a[2] + ox, a[3] + oy);
            qx = a[0] + ox; qy = a[1] + oy; cx = a[2] + ox; cy = a[3] + oy;
            break;
        }
        case 'A':
            is_arc(cx, cy, a[0], a[1], a[3] != 0, a[4] != 0, a[5] + ox, a[6] + oy);
            cx = a[5] + ox; cy = a[6] + oy;
            break;
        case 'Z': is_add(cx, cy, sx, sy); cx = sx; cy = sy; break;
        case 'O':
            for (int i = 0; i < 24; i++) {
                float t0 = i * 6.2831853f / 24, t1 = (i + 1) * 6.2831853f / 24;
                is_add(a[0] + a[2] * cosf(t0), a[1] + a[2] * sinf(t0),
                       a[0] + a[2] * cosf(t1), a[1] + a[2] * sinf(t1));
            }
            break;
        case 'R':
            is_add(a[0], a[1], a[0] + a[2], a[1]);
            is_add(a[0] + a[2], a[1], a[0] + a[2], a[1] + a[3]);
            is_add(a[0] + a[2], a[1] + a[3], a[0], a[1] + a[3]);
            is_add(a[0], a[1] + a[3], a[0], a[1]);
            break;
        default: return;
        }
        last = C;
    }
}

/* coverage of a stroke of half width hw along a segment, into m (sz x sz) */
static void seg_cov(unsigned char *m, int sz, float x0, float y0, float x1, float y1, float hw) {
    int ia = (int)floorf(fminf(x0, x1) - hw - 1), ib = (int)ceilf(fmaxf(x0, x1) + hw + 1);
    int ja = (int)floorf(fminf(y0, y1) - hw - 1), jb = (int)ceilf(fmaxf(y0, y1) + hw + 1);
    if (ia < 0) ia = 0;
    if (ja < 0) ja = 0;
    if (ib > sz) ib = sz;
    if (jb > sz) jb = sz;
    float dx = x1 - x0, dy = y1 - y0, l2 = dx * dx + dy * dy, lim = (hw + .5f) * (hw + .5f);
    for (int j = ja; j < jb; j++)
        for (int i = ia; i < ib; i++) {
            float px0 = i + .5f, py0 = j + .5f, t = 0;
            if (l2 > 0) {
                t = ((px0 - x0) * dx + (py0 - y0) * dy) / l2;
                t = t < 0 ? 0 : t > 1 ? 1 : t;
            }
            float ex = x0 + t * dx - px0, ey = y0 + t * dy - py0, d2 = ex * ex + ey * ey;
            if (d2 >= lim) continue;
            float cv = hw + .5f - sqrtf(d2);
            int v = cv >= 1 ? 255 : (int)(cv * 255);
            if (v > m[j * sz + i]) m[j * sz + i] = (unsigned char)v;
        }
}

static const unsigned char *ic_mask(int id, int sz) {
    static struct { int id, sz; unsigned char *m; } c[160];
    static int nc;
    for (int i = 0; i < nc; i++) if (c[i].id == id && c[i].sz == sz) return c[i].m;
    unsigned char *m = calloc((size_t)sz * sz, 1);
    if (!m) return NULL;
    const char *d = k_icons[id];
    float k = sz / 24.0f;
    ip_path(d);
    if (*d == '#') {
        for (int j = 0; j < sz; j++)
            for (int i = 0; i < sz; i++) {
                int cnt = 0;
                for (int v = 0; v < 4; v++)
                    for (int u = 0; u < 4; u++) {
                        float x = (i + (u + .5f) / 4) / k, y = (j + (v + .5f) / 4) / k;
                        int in = 0;
                        for (int q = 0; q < g_nis; q++) {
                            const iseg *e = &g_is[q];
                            if ((e->y0 <= y) != (e->y1 <= y) &&
                                x < e->x0 + (y - e->y0) * (e->x1 - e->x0) / (e->y1 - e->y0)) in ^= 1;
                        }
                        cnt += in;
                    }
                m[j * sz + i] = (unsigned char)(cnt * 255 / 16);
            }
    } else
        for (int q = 0; q < g_nis; q++)
            seg_cov(m, sz, g_is[q].x0 * k, g_is[q].y0 * k, g_is[q].x1 * k, g_is[q].y1 * k, k);
    if (nc < 160) c[nc++] = (typeof(c[0])){ id, sz, m };
    return m;
}

static void icon(const surf *s, int id, int x, int y, int sz, unsigned c) {
    const unsigned char *m = id >= 0 ? ic_mask(id, sz) : NULL;
    if (!m) return;
    for (int j = 0; j < sz; j++)
        for (int i = 0; i < sz; i++) {
            int a = m[j * sz + i];
            if (a) pxa(s, x + i, y + j, c, a + (a >> 7));
        }
}

/* an anti-aliased line, half width hw, straight onto the surface */
static void aa_line(const surf *s, float x0, float y0, float x1, float y1, float hw, unsigned c) {
    int ia = (int)floorf(fminf(x0, x1) - hw - 1), ib = (int)ceilf(fmaxf(x0, x1) + hw + 1);
    int ja = (int)floorf(fminf(y0, y1) - hw - 1), jb = (int)ceilf(fmaxf(y0, y1) + hw + 1);
    float dx = x1 - x0, dy = y1 - y0, l2 = dx * dx + dy * dy, lim = (hw + .5f) * (hw + .5f);
    for (int j = ja; j < jb; j++)
        for (int i = ia; i < ib; i++) {
            float px0 = i + .5f, py0 = j + .5f, t = 0;
            if (l2 > 0) {
                t = ((px0 - x0) * dx + (py0 - y0) * dy) / l2;
                t = t < 0 ? 0 : t > 1 ? 1 : t;
            }
            float ex = x0 + t * dx - px0, ey = y0 + t * dy - py0, d2 = ex * ex + ey * ey;
            if (d2 >= lim) continue;
            float cv = hw + .5f - sqrtf(d2);
            pxa(s, i, j, c, cv >= 1 ? 256 : (int)(cv * 256));
        }
}

static void poly_fill(const surf *s, float (*p)[2], int n, unsigned c, int a) {
    float y0 = 1e9f, y1 = -1e9f;
    for (int i = 0; i < n; i++) { y0 = fminf(y0, p[i][1]); y1 = fmaxf(y1, p[i][1]); }
    for (int y = (int)y0; y <= (int)y1; y++) {
        float yc = y + .5f, xs[K_MAXP];
        int nx = 0;
        for (int i = 0; i < n; i++) {
            const float *A = p[i], *B = p[(i + 1) % n];
            if ((A[1] <= yc) != (B[1] <= yc))
                xs[nx++] = A[0] + (yc - A[1]) * (B[0] - A[0]) / (B[1] - A[1]);
        }
        for (int i = 1; i < nx; i++)
            for (int j = i; j > 0 && xs[j - 1] > xs[j]; j--) { float t = xs[j]; xs[j] = xs[j - 1]; xs[j - 1] = t; }
        for (int i = 0; i + 1 < nx; i += 2)
            for (int x = (int)(xs[i] + .5f); x < (int)(xs[i + 1] + .5f); x++) pxa(s, x, y, c, a);
    }
}

/* ---- widgets. Each draws and registers its button in one pass, so the
 * touch areas are exactly what is on screen. */
static btn g_b[48];
static int g_nb, g_armed = -1;

static void ui_reg(int x, int y, int w, int h, const char *lab, const char *tok, int dng, int arg) {
    if (g_nb < 48) g_b[g_nb++] = (btn){ x, y, w, h, lab, tok, 0, 0, 0, dng, arg };
}

static int ui_armed(void) { return g_armed >= 0 && g_armed == g_nb; }

enum { V_NORM, V_GO, V_STOP, V_WARN, V_ACC, V_GHOST };
#define B_DNG   1
#define B_OFF   2
#define B_STACK 4

static void ui_btn(const surf *s, int x, int y, int w, int h, const char *lab, const char *tok,
                   int v, int ic, int fl, int arg) {
    static const unsigned bgs[] = { U_SURF2, U_GO, U_STOP, U_WARN, U_ACC, 0 };
    static const unsigned fgs[] = { U_FG, U_GO_INK, U_STOP_INK, U_WARN_INK, U_ACC_INK, U_FG };
    int off = fl & B_OFF, armed = !off && ui_armed();
    unsigned fg = fgs[v];
    const char *t = lab;
    if (off) g_op = 82;
    if (armed) { rr(s, x, y, w, h, 12, 0xffffffu); fg = 0x111111u; t = "Tap again"; ic = -1; }
    else if (v == V_GHOST) rring(s, x, y, w, h, 12, 3, U_LINE);
    else rr(s, x, y, w, h, 12, bgs[v]);
    if ((fl & B_STACK) && ic >= 0) {
        int isz = h >= 90 ? 30 : 26, top = y + (h - isz - 6 - 12) / 2;
        icon(s, ic, x + w / 2 - isz / 2, top, isz, fg);
        u_text(s, x + w / 2, top + isz + 6 + 12, UF(1, 17, 0), u_fit(UF(1, 17, 0), t, w - 8), fg, 1);
    } else {
        int fs = h >= 90 ? 26 : h >= 70 ? 20 : 18;
        if (u_tw(UF(1, 10, 0), t) <= 8) fs = 32;              /* a lone - or + */
        ufont F = UF(1, fs, 0);
        int isz = ic < 0 ? 0 : fs >= 26 ? 30 : fs >= 20 ? 26 : 22, gap = ic >= 0 && *t ? 10 : 0;
        const char *ft = u_fit(F, t, w - 16 - isz - gap);
        int lx = x + (w - u_tw(F, ft) - isz - gap) / 2;
        if (ic >= 0) { icon(s, ic, lx, y + (h - isz) / 2, isz, fg); lx += isz + gap; }
        u_text(s, lx, y + h / 2 + fs * 35 / 100, F, ft, fg, 0);
    }
    g_op = 256;
    if (!off) ui_reg(x, y, w, h, lab, tok, fl & B_DNG, arg);
}

static int ui_pill(const surf *s, int x, int y, const char *t, unsigned c, unsigned bg) {
    ufont F = UF(1, 13, 11);
    int w = u_tw(F, t) + 22;
    rr(s, x, y, w, 28, 14, mix(bg, c, 41));
    u_text(s, x + 11, y + 19, F, t, c, 0);
    return w;
}

static void ui_lab(const surf *s, int x, int y, const char *t, unsigned c, int al) {
    u_text(s, x, y, UF(1, 14, 13), t, c, al);
}

static void ui_header(const surf *s, const char *title, int back, const char *pill, unsigned pc) {
    fill(s, 0, 63, s->w, 1, U_LINE);
    int tx = 16, x = 464;
    if (back) {
        icon(s, I_BACK, 15, 17, 30, U_FG);
        ui_reg(0, 0, 72, 64, "Back", "back", 0, 0);   /* the whole corner, easy to hit */
        tx = 68;
    }
    if (pill) x -= ui_pill(s, x - (u_tw(UF(1, 13, 11), pill) + 22), 18, pill, pc, U_SCR) + 10;
    time_t now = time(NULL) + g_wall;
    if (now > 1700000000) {
        struct tm tm;
        char c[8];
        localtime_r(&now, &tm);
        strftime(c, sizeof c, "%H:%M", &tm);
        x = u_text(s, x, 38, UF(0, 18, 0), c, U_MUT, 2) - u_tw(UF(0, 18, 0), c) - 10;
    }
    u_text(s, tx, 40, UF(1, 24, 0), u_fit(UF(1, 24, 0), title, x - tx), U_FG, 0);
}

static void ui_tabs(const surf *s, int act, unsigned dot, int lock) {
    static const char *const lab[4] = { "Home", "Print", "Heat", "Tools" };
    static const char *const tok[4] = { "home", "files", "heat", "tools" };
    static const int ic[4] = { I_HOME, I_PRINT, I_HEAT, I_TOOLS };
    fill(s, 0, 712, s->w, 88, U_SURF);
    fill(s, 0, 712, s->w, 1, U_LINE);
    for (int i = 0; i < 4; i++) {
        int x = i * 120, off = lock && i;
        unsigned c = i == act ? U_ACC : U_MUT;
        if (off) g_op = 82;
        if (i == act) fill(s, x + 28, 712, 64, 3, U_ACC);
        icon(s, ic[i], x + 45, 731, 30, c);
        u_text(s, x + 60, 779, UF(1, 15, 0), lab[i], c, 1);
        g_op = 256;
        if (!i && dot) { rr(s, x + 69, 727, 16, 16, 8, U_SURF); rr(s, x + 72, 730, 10, 10, 5, dot); }
        if (!off) ui_reg(x, 712, 120, 88, lab[i], tok[i], 0, 0);
    }
}

static void ui_bar(const surf *s, int x, int y, int w, double f, unsigned c) {
    rr(s, x, y, w, 14, 7, U_SURF2);
    int fw = (int)(w * (f < 0 ? 0 : f > 1 ? 1 : f) + .5);
    if (fw > 0) rr(s, x, y, fw, 14, 7, c);
}

static void ui_thumb(const surf *s, int x, int y, int sz, const kimg *im, int ic) {
    rr(s, x, y, sz, sz, 10, U_SURF2);
    if (im && im->px) k_img(s, x + 4, y + 4, sz - 8, im, U_SURF2);
    else icon(s, ic, x + sz / 4, y + sz / 4, sz / 2, U_DIM);
}

/* alternating muted and bold runs: "Done at ", "14:20", " · layer ", "3 / 90" */
static int ui_meta(const surf *s, int x, int y, int n, const char *const *p) {
    for (int i = 0; i < n; i++)
        x = u_text(s, x, y, i & 1 ? UF(1, 16, 0) : UF(0, 16, 0), p[i], i & 1 ? U_FG : U_MUT, 0);
    return x;
}

static void ui_seg(const surf *s, int x, int y, int w, int n, const char *const *lab,
                   const char *const *tok, int on) {
    rr(s, x, y, w, 60, 12, x > 16 ? U_SURF2 : U_SURF);
    int bw = (w - 8 - (n - 1) * 4) / n;
    for (int i = 0; i < n; i++) {
        int bx = x + 4 + i * (bw + 4);
        if (i == on) {
            rr(s, bx, y + 4, bw, 52, 9, x > 16 ? U_LINE : U_SURF2);
            fill(s, bx + 9, y + 54, bw - 18, 2, U_ACC);
        }
        u_text(s, bx + bw / 2, y + 36, UF(1, 18, 0), lab[i], i == on ? U_FG : U_MUT, 1);
        ui_reg(bx, y + 4, bw, 52, lab[i], tok[i], 0, i);
    }
}

static int ui_kv(const surf *s, int y, const char *const *kv, int n) {
    const int h = 50;
    ufont K = UF(0, 17, 0), V = UF(1, 17, 0);
    rr(s, 16, y, 448, n * h, 14, U_SURF);
    for (int i = 0; i < n; i++) {
        int ry = y + i * h, kw = u_tw(K, kv[2 * i]);
        if (i) fill(s, 16, ry, 448, 1, U_LINE);
        u_text(s, 32, ry + 31, K, kv[2 * i], U_MUT, 0);
        u_text(s, 448, ry + 31, V, u_fit(V, kv[2 * i + 1], 448 - 32 - kw - 16), U_FG, 2);
    }
    return y + n * h;
}

static void ui_tile(const surf *s, int x, int y, int ic, const char *b, const char *sub,
                    const char *tok, int fl) {
    const int w = 218, h = 148;
    int off = fl & B_OFF, armed = !off && ui_armed();
    if (off) g_op = 82;
    rr(s, x, y, w, h, 14, armed ? 0xffffffu : U_SURF);
    icon(s, ic, x + 16, y + 16, 36, armed ? 0x111111u : U_ACC);
    u_text(s, x + 16, y + h - 46, UF(1, 22, 0), armed ? "Tap again" : u_fit(UF(1, 22, 0), b, w - 32),
           armed ? 0x111111u : U_FG, 0);
    u_text(s, x + 16, y + h - 18, UF(0, 15, 0), u_fit(UF(0, 15, 0), sub, w - 32),
           armed ? 0x444444u : U_MUT, 0);
    g_op = 256;
    if (!off) ui_reg(x, y, w, h, b, tok, fl & B_DNG, 0);
}

/* a list row: thumbnail (or icon), name, then status and details */
static void ui_li(const surf *s, int y, const kimg *im, int ic, const char *name, unsigned nc,
                  const char *st, unsigned stc, const char *meta, const char *tok, int fl, int arg) {
    const int x = 16, w = 448, h = 88, tx = x + 88, tw = w - 88 - 14 - 22 - 10;
    int armed = ui_armed();
    ufont N = UF(2, 21, 0), M = UF(0, 16, 0);
    rr(s, x, y, w, h, 12, armed ? 0xffffffu : U_SURF);
    ui_thumb(s, x + 10, y + 12, 64, im, ic);
    u_text(s, tx, y + 38, N, u_fit(N, name, tw), armed ? 0x111111u : nc, 0);
    if (armed) u_text(s, tx, y + 66, UF(1, 16, 0), "Tap again to copy and print", 0x111111u, 0);
    else {
        int mx = tx;
        if (st) mx = u_text(s, tx, y + 66, UF(1, 13, 10), st, stc, 0) + 10;
        u_text(s, mx, y + 66, M, u_fit(M, meta, tx + tw - mx), U_MUT, 0);
    }
    icon(s, I_CHEV, x + w - 36, y + 33, 22, U_DIM);
    ui_reg(x, y, w, h, name, tok, fl & B_DNG, arg);
}

static void ui_pager(const surf *s, int page, int n) {
    int np = (n + K_ROWS - 1) / K_ROWS;
    char t[32];
    if (np <= 1) return;
    ui_btn(s, 16, 632, 88, 64, "", "prev", V_NORM, I_BACK, page > 0 ? 0 : B_OFF, 0);
    ui_btn(s, 376, 632, 88, 64, "", "next", V_NORM, I_CHEV, page + 1 < np ? 0 : B_OFF, 0);
    snprintf(t, sizeof t, "Page %d of %d", page + 1, np);
    u_text(s, 240, 670, UF(1, 17, 0), t, U_MUT, 1);
}

static void ui_empty(const surf *s, int y, int ic, const char *b, const char *t) {
    icon(s, ic, 208, y, 64, U_DIM);
    u_text(s, 240, y + 100, UF(1, 22, 0), b, U_FG, 1);
    if (t) u_wrap(s, 240, y + 130, UF(0, 17, 0), t, U_MUT, 380, 23, 8, 1);
}

static void ui_tmini(const surf *s, int y, const kstat *k) {
    for (int i = 0; i < 2; i++) {
        int x = 16 + i * 230;
        double t = i ? k->bt : k->et, g = i ? k->btt : k->ett;
        char v[16], tg[16];
        rr(s, x, y, 218, 60, 12, U_SURF);
        int e = u_text(s, x + 14, y + 40, UF(0, 16, 0), i ? "Bed" : "Nozzle", U_MUT, 0);
        snprintf(v, sizeof v, "%.0f°", t);
        e = u_text(s, e + 10, y + 40, UF(1, 34, 0), v, U_FG, 0);
        if (g > 0) snprintf(tg, sizeof tg, "/ %.0f", g);
        else snprintf(tg, sizeof tg, "off");
        u_text(s, e + 8, y + 40, UF(0, 16, 0), tg, g > 0 ? U_HEAT : U_MUT, 0);
    }
}

static void ui_toast(const surf *s, const char *t) {
    ufont F = UF(1, 18, 0);
    rr(s, 16, 648, 448, 52, 12, 0xf2f5f7u);
    u_text(s, 34, 680, F, u_fit(F, t, 412), 0x111111u, 0);
}

/* ---- state */
static unsigned k_pill(const kstat *k, const char **label) {
    if (!k->online) { *label = "OFFLINE"; return U_STOP; }
    if (!strcmp(k->kstate, "startup")) { *label = "STARTING"; return U_WARN; }
    if (strcmp(k->kstate, "ready")) { *label = "NOT READY"; return U_STOP; }
    if (!strcmp(k->pstate, "printing")) { *label = "PRINTING"; return U_ACC; }
    if (!strcmp(k->pstate, "paused")) { *label = "PAUSED"; return U_WARN; }
    *label = "READY";
    return U_GO;
}

/* "dir/name.gcode" -> "name" */
static const char *k_base(const char *path) {
    static char b[4][128];
    static int bi;
    const char *f = strrchr(path, '/');
    char *o = b[bi++ & 3];
    snprintf(o, 128, "%s", f ? f + 1 : path);
    int l = (int)strlen(o);
    if (l > 6 && !strcmp(o + l - 6, ".gcode")) o[l - 6] = 0;
    return o;
}

static const char *k_hstat(const char *st, unsigned *c) {
    if (!strcmp(st, "completed")) { *c = U_GO; return "DONE"; }
    if (!strcmp(st, "cancelled")) { *c = U_STOP; return "CANCELLED"; }
    if (!strcmp(st, "in_progress")) { *c = U_ACC; return "PRINTING"; }
    *c = U_STOP;
    return "FAILED";
}

static void k_when(double t, char *o, int n) {
    time_t te = (time_t)t;
    struct tm tm;
    o[0] = 0;
    if (te <= 1700000000) return;
    localtime_r(&te, &tm);
    strftime(o, n, "%b %d %H:%M", &tm);
    if (!tm.tm_gmtoff) { size_t l = strlen(o); snprintf(o + l, n - l, " UTC"); }
}

static void k_size(double sz, char *o, int n) {
    if (sz >= 1e6) snprintf(o, n, "%.1f MB", sz / 1e6);
    else snprintf(o, n, "%.0f KB", sz / 1e3);
}

static int k_modei(const kstat *k) {
    int i = 0;
    while (i < 5 && strcmp(k->smode, k_mname[i])) i++;
    return i < 5 ? i : 0;
}

/* LOAD_FILAMENT / UNLOAD_FILAMENT are macros in printer.cfg. During a pause
 * they run at the job's own nozzle target (no TEMP=), so RESUME prints at
 * the temperature the slicer set. A load or unload is running while dir is
 * set and the nozzle has a target: every stop (heater off, shutdown,
 * restart) leaves no target. */
static int k_filrun(const kstat *k) {
    return k->fdir && k->ett > 0;
}

/* The filament tip between the extruder gears and the nozzle, from the
 * extruder's live position: fills on a load, drains on an unload, in mm. */
static void k_filbar(const surf *s, const kstat *k, int y) {
    int load = k->fdir > 0;
    double moved = load ? k->fe - k->fe0 : k->fe0 - k->fe;
    if (moved < 0) moved = 0;
    if (moved > k->flen) moved = k->flen;
    double in = load ? moved : k->flen - moved;
    unsigned col = load ? U_GO : U_WARN;
    char big[16], sub[32], st[48];
    rr(s, 16, y, 448, 170, 14, U_SURF);
    ui_lab(s, 32, y + 30, !load ? "UNLOADING" : k->flen <= 50 ? "PURGING" : "LOADING", col, 0);
    snprintf(big, sizeof big, "%.0f", in);
    snprintf(sub, sizeof sub, " / %.0f mm", k->flen);
    u_text(s, u_text(s, 32, y + 80, UF(1, 44, 0), big, U_FG, 0), y + 80, UF(1, 22, 0), sub, U_MUT, 0);
    ui_bar(s, 32, y + 94, 416, k->flen > 0 ? in / k->flen : 0, col);
    u_text(s, 32, y + 126, UF(1, 12, 12), "GEARS", U_MUT, 0);
    u_text(s, 448, y + 126, UF(1, 12, 12), "NOZZLE", U_MUT, 2);
    if (moved < 0.5 && k->et < k->ett - 2)
        snprintf(st, sizeof st, "Heating %.0f / %.0f °C", k->et, k->ett);
    else if (load)
        snprintf(st, sizeof st, moved < k->flen - 120 ? "Down the tube" : "Through the hot end");
    else
        snprintf(st, sizeof st, moved < 95 ? "Out of the hot end" : "Back up the tube");
    u_text(s, 32, y + 154, UF(1, 18, 0), st, U_FG, 0);
}

/* ---- screens */
static void k_hero(const surf *s, const kstat *k) {
    for (int i = 0; i < 2; i++) {
        int x = i ? 256 : 16;
        double t = i ? k->bt : k->et, g = i ? k->btt : k->ett;
        char v[16], tg[32];
        icon(s, i ? I_SPOOL : I_HEAT, x, 82, 18, U_MUT);
        ui_lab(s, x + 24, 97, i ? "BED" : "NOZZLE", U_MUT, 0);
        snprintf(v, sizeof v, "%.0f", t);
        u_text(s, u_text(s, x - 3, 172, UF(1, 76, -10), v, U_FG, 0) + 4, 172, UF(1, 34, 0), "°C", U_MUT, 0);
        if (g > 0) snprintf(tg, sizeof tg, "%s %.0f °C", fabs(t - g) < 1.5 ? "holding" : "heating to", g);
        else snprintf(tg, sizeof tg, "off");
        u_text(s, x, 204, UF(0, 18, 0), tg, g > 0 ? U_HEAT : U_MUT, 0);
    }
    fill(s, 240, 86, 1, 120, U_LINE);
}

/* J8: result, real time against the estimate, filament and cost */
static int k_endcard(const surf *s, const kstat *k, const kmenu *m, int y) {
    const kmeta *t = &m->jmeta;
    int ok = !strcmp(k->pstate, "complete"), cx = !strcmp(k->pstate, "cancelled");
    const int tx = 142, tw = 306;
    ufont N = UF(2, 24, 0);
    char a[24], b[24], l2[96], pct[16];
    int nl = u_wrap(NULL, tx, 0, N, k_base(k->file), U_FG, tw, 26, 2, 0);
    int h = 16 + 28 + 8 + nl * 26 + 8 + 44 + 14 + 64 + 16;
    rr(s, 16, y, 448, h, 14, U_SURF);
    ui_thumb(s, 32, y + 16, 96, &m->jimg, I_PRINT);
    ui_pill(s, tx, y + 16, ok ? "DONE" : cx ? "CANCELLED" : "FAILED", ok ? U_GO : U_STOP, U_SURF);
    u_wrap(s, tx, y + 16 + 28 + 26, N, k_base(k->file), U_FG, tw, 26, 2, 0);
    int ly = y + 16 + 28 + 8 + nl * 26 + 18;
    if (!ok && !cx)
        u_wrap(s, tx, ly, UF(0, 16, 0), k->pmsg[0] ? k->pmsg : "Klipper stopped the job.", U_MUT, tw, 22, 2, 0);
    else {
        k_hm(k->dur, a, sizeof a);
        if (cx) {
            snprintf(pct, sizeof pct, "%.0f %%", k->prog * 100);
            const char *p[4] = { "Stopped at ", pct, " after ", a };
            ui_meta(s, tx, ly, 4, p);
        } else if (t->est > 0) {
            int d = (int)((k->dur - t->est) * 100 / t->est + (k->dur > t->est ? 0.5 : -0.5));
            snprintf(b, sizeof b, " · estimate %+d %%", d);
            const char *p[3] = { "Printed ", a, b };
            ui_meta(s, tx, ly, 3, p);
        } else {
            const char *p[2] = { "Printed ", a };
            ui_meta(s, tx, ly, 2, p);
        }
        /* grams: the slicer's figure scaled to what was used, else from density */
        double dens = m->rate[1] > 0 ? m->rate[1] : 1.24;
        double g = t->grams > 0 && t->fil > 0 ? t->grams * k->fused / t->fil : k->fused * 2.405e-3 * dens;
        int n = snprintf(l2, sizeof l2, "%.2f m · %.0f g", k->fused / 1000, g);
        if (m->rate[0] > 0) {
            double mat = g / 1000 * m->rate[0];
            snprintf(l2 + n, sizeof l2 - n, m->rate[2] > 0 ? " · $%.2f (fil $%.2f)" : " · $%.2f",
                     mat + k->tot / 3600 * m->rate[2], mat);
        }
        u_text(s, tx, ly + 22, UF(0, 16, 0), u_fit(UF(0, 16, 0), l2, tw), U_MUT, 0);
    }
    int by = y + h - 16 - 64;
    ui_btn(s, 32, by, 202, 64, "Dismiss", "ack", V_GHOST, -1, 0, 0);
    ui_btn(s, 246, by, 202, 64, "Print again", "again", V_GO, I_PLAY, B_DNG, 0);
    return y + h;
}

static void k_homeidle(const surf *s, const kstat *k, const kmenu *m) {
    static const char *const lcmd[3] = { K_GS "SET_PIN%20PIN%3Dlight%20VALUE%3D1",
                                         K_GS "SET_PIN%20PIN%3Dlight%20VALUE%3D0.3",
                                         K_GS "SET_PIN%20PIN%3Dlight%20VALUE%3D0" };
    char lt[16], ms[48];
    int ended = k_ended(k) && !m->ack, heating = k->ett > 0 || k->btt > 0;
    k_hero(s, k);
    if (ended) k_endcard(s, k, m, 222);
    else if (m->nlast) {
        unsigned c;
        char st[32];
        const kjob *j = &m->last;
        int armed = ui_armed();
        snprintf(st, sizeof st, "LAST · %s", k_hstat(j->status, &c));
        rr(s, 16, 222, 448, 88, 12, armed ? 0xffffffu : U_SURF);
        ui_thumb(s, 26, 234, 64, &m->limg, I_PRINT);
        u_text(s, 104, 256, UF(1, 13, 10), st, c, 0);
        u_text(s, 104, 286, UF(2, 21, 0), u_fit(UF(2, 21, 0), k_base(j->name), 324),
               j->exists ? U_FG : U_MUT, 0);
        icon(s, I_CHEV, 428, 255, 22, U_DIM);
        ui_reg(16, 222, 448, 88, "Last job", "hlast", 0, 0);
    }
    int ty = heating && !ended ? 472 : 548;
    if (k->light >= 0) {
        int li = k->light <= 0 ? 0 : k->light >= 1 ? 1 : 2;
        if (li == 2) snprintf(lt, sizeof lt, "%.0f %%", k->light * 100);
        else snprintf(lt, sizeof lt, li ? "On" : "Off");
        ui_tile(s, 16, ty, I_LIGHT, "Light", lt, lcmd[li], 0);
    } else
        ui_tile(s, 16, ty, I_FAN, "Fans", k->pfan > 0 || k->cfan > 0 ? "running" : "both off", "fans", 0);
    int mi = k_modei(k);
    snprintf(ms, sizeof ms, "speed mode · %d mm/s", k->mode[mi][0]);
    ui_tile(s, 246, ty, I_GAUGE, k_mname[mi], ms, "mode", 0);
    if (heating && !ended)
        ui_btn(s, 16, 632, 448, 64, "Cool down", K_GS "TURN_OFF_HEATERS", V_GHOST, I_X, 0, 0);
}

/* name, thumbnail, time left, finish clock, layer, progress, filament, tune */
static void k_printing(const surf *s, const kstat *k, const kmenu *m) {
    const kmeta *t = &m->jmeta;
    char big[16], r[32], ly[32], v[48];
    const char *suf = " min left";
    rr(s, 16, 80, 448, 236, 14, U_SURF);
    ui_thumb(s, 32, 96, 108, &m->jimg, I_PRINT);
    int nl = u_wrap(s, 154, 116, UF(2, 24, 0), k_base(k->file), U_FG, 294, 26, 2, 0);
    /* klipper_estimator's time excludes PRINT_START, as print_duration does;
     * until the first extrusion the job is still heating and homing */
    int heat = k->fused <= 0 && !strcmp(k->pstate, "printing");
    double left = k_left(k, t);
    int mn = (int)(left / 60 + 0.5);
    if (left < 0) { snprintf(big, sizeof big, "--"); suf = " left"; }
    else if (mn >= 60) { snprintf(big, sizeof big, "%d:%02d", mn / 60, mn % 60); suf = " h left"; }
    else if (mn > 0) snprintf(big, sizeof big, "%d", mn);
    else snprintf(big, sizeof big, "< 1");
    int by = nl > 1 ? 186 : 166;
    u_text(s, u_text(s, 154, by, UF(1, 44, 0), big, U_FG, 0) + 4, by, UF(1, 22, 0), suf, U_MUT, 0);
    time_t now = time(NULL) + g_wall;
    struct tm tm;
    /* g_wall comes from Moonraker's Date header; no header, no clock */
    if (left < 0 || now < 1700000000) snprintf(r, sizeof r, "--");
    else {
        now += (time_t)left;
        localtime_r(&now, &tm);
        strftime(r, sizeof r, left > 20 * 3600 ? "%a %H:%M" : "%H:%M", &tm);
        if (!tm.tm_gmtoff) { size_t l = strlen(r); snprintf(r + l, sizeof r - l, " UTC"); }
    }
    if (k->nlayer > 0) snprintf(ly, sizeof ly, "%d / %d", k->layer, k->nlayer);
    else if (t->layers > 0) snprintf(ly, sizeof ly, "? / %.0f", t->layers);
    else snprintf(ly, sizeof ly, "--");
    const char *p[4] = { heat ? "Heating, done " : "Done at ", r, " · layer ", ly };
    ui_meta(s, 154, by + 24, 4, p);
    ui_bar(s, 32, 220, 416, k->prog, U_ACC);
    snprintf(v, sizeof v, "%.0f %%", k->prog * 100);
    u_text(s, 32, 256, UF(1, 16, 0), v, U_MUT, 0);
    if (t->fil > 0) snprintf(v, sizeof v, "%.2f / %.2f m", k->fused / 1000, t->fil / 1000);
    else snprintf(v, sizeof v, "%.2f m", k->fused / 1000);
    u_text(s, 448, 256, UF(1, 16, 0), v, U_MUT, 2);
    char cv[3][24];
    snprintf(cv[0], 24, "%.0f %%", k->speed * 100);
    snprintf(cv[1], 24, "%.0f %%", k->pfan * 100);
    snprintf(cv[2], 24, "%s", k_mname[k_modei(k)]);
    static const char *const cl[3] = { "Speed ", "Fan ", "Mode " };
    ufont C = UF(1, 14, 0);
    for (int i = 0, x = 32; i < 3; i++) {
        int w = u_tw(C, cl[i]) + u_tw(C, cv[i]) + 20;
        rr(s, x, 268, w, 32, 8, U_SURF2);
        u_text(s, u_text(s, x + 10, 289, C, cl[i], U_MUT, 0), 289, C, cv[i], U_FG, 0);
        x += w + 8;
    }
    ui_tmini(s, 328, k);
    ui_btn(s, 16, 604, 141, 92, "Pause", "/printer/print/pause", V_WARN, I_PAUSE, B_STACK, 0);
    ui_btn(s, 169, 604, 142, 92, "Tune", "tune", V_NORM, I_TUNE, B_STACK, 0);
    ui_btn(s, 323, 604, 141, 92, "Cancel", "/printer/print/cancel", V_STOP, I_X, B_STACK | B_DNG, 0);
}

/* the card at the top of a stop, then the temperatures */
static void k_stopcard(const surf *s, const kstat *k, int ic, const char *lab, const char *why, const char *nt) {
    ufont W = UF(1, 30, 0), N = UF(0, 16, 0);
    int n1 = u_wrap(NULL, 32, 0, W, why, U_FG, 416, 34, 2, 0), n2 = u_wrap(NULL, 32, 0, N, nt, U_MUT, 416, 21, 3, 0);
    int h = 16 + 18 + 16 + n1 * 34 + 6 + n2 * 21 + 14;
    rr(s, 16, 80, 448, h, 14, U_SURF);
    icon(s, ic, 32, 96, 18, U_WARN);
    ui_lab(s, 56, 111, lab, U_WARN, 0);
    u_wrap(s, 32, 154, W, why, U_FG, 416, 34, 2, 0);
    u_wrap(s, 32, 154 + (n1 - 1) * 34 + 30, N, nt, U_MUT, 416, 21, 3, 0);
    ui_tmini(s, 80 + h + 12, k);
}

/* M1, Q6: FILAMENT_SWAP stopped the job and unloads. Resume appears once the
 * new filament is loaded. */
static void k_swap(const surf *s, const kstat *k) {
    int run = k_filrun(k);
    k_stopcard(s, k, I_SPOOL, k->swap == 2 ? "OUT OF FILAMENT" : "FILAMENT CHANGE", k->dmsg,
               run ? "Wait for the filament to stop moving."
                   : k->fed ? "Purge until the new colour runs clean, then tap Resume."
                            : "Pull the old filament out, feed the new one into the gears, then tap Load.");
    if (run) { k_filbar(s, k, 526); return; }
    if (k->fed) ui_btn(s, 16, 528, 448, 92, "Resume", "/printer/print/resume", V_GO, I_PLAY, 0, 0);
    else ui_btn(s, 16, 528, 448, 92, "Load", K_GS "LOAD_FILAMENT", V_GO, I_SPOOL, 0, 0);
    ui_btn(s, 16, 632, 103, 64, "Unload", K_GS "UNLOAD_FILAMENT", V_NORM, I_SPOOL, B_STACK, 0);
    ui_btn(s, 131, 632, 103, 64, "Purge 20", K_GS "LOAD_FILAMENT%20LENGTH%3D20", V_NORM, I_HEAT,
           B_STACK | (k->fed ? 0 : B_OFF), 0);
    ui_btn(s, 246, 632, 103, 64, "Tune", "tune", V_NORM, I_TUNE, B_STACK, 0);
    ui_btn(s, 361, 632, 103, 64, "Cancel", "/printer/print/cancel", V_STOP, I_X, B_STACK | B_DNG, 0);
}

/* B2: a plate stop (PLATE_NEXT_OBJECT, PLATE_PAUSE_LAYER) or a plain pause;
 * printer.cfg's PAUSE has lifted clear and brought the bed forward */
static void k_paused(const surf *s, const kstat *k) {
    if (k->swap) { k_swap(s, k); return; }
    k_stopcard(s, k, I_PAUSE, k->dmsg[0] ? "WAITING FOR YOU" : "PAUSED", k->dmsg[0] ? k->dmsg : "Paused",
               k->dmsg[0] ? "Set up, then tap Ready." : "The head is parked and the bed forward. Tap Resume to carry on.");
    ui_btn(s, 16, 528, 448, 92, k->dmsg[0] ? "Ready" : "Resume", "/printer/print/resume", V_GO, I_PLAY, 0, 0);
    /* REDO = printer.cfg's PLATE_REDO (B4) */
    ui_btn(s, 16, 632, 103, 64, "Filament", "heat", V_NORM, I_SPOOL, B_STACK, 0);
    ui_btn(s, 131, 632, 103, 64, "Redo", K_GS "PLATE_REDO", V_NORM, I_REDO, B_STACK | B_DNG, 0);
    ui_btn(s, 246, 632, 103, 64, "Tune", "tune", V_NORM, I_TUNE, B_STACK, 0);
    ui_btn(s, 361, 632, 103, 64, "Cancel", "/printer/print/cancel", V_STOP, I_X, B_STACK | B_DNG, 0);
}

/* Q8: printer.cfg's CLEAR_CLOG holds Klipper for its whole run, so Stop has to
 * be the emergency stop; the panel's shutdown recovery restarts Klipper. */
static void k_clog(const surf *s, const kstat *k) {
    static const char *const st[6] = { "Heating up.", "Working the filament up and down.", "Cooling the nozzle.",
                                       "Pulling the plug out cold.", "Test purge." };
    char why[32], nt[96];
    if (k->cstep >= 6) {
        k_stopcard(s, k, I_HEAT, "CLOG RUN FINISHED", "Did plastic come out?",
                   "A steady strand from the nozzle means it is clear. No strand: run it again.");
        if (k_filrun(k)) { k_filbar(s, k, 526); return; }
        ui_btn(s, 16, 528, 448, 92, "Done", K_GS "CLEAR_CLOG_DONE", V_GO, I_PLAY, 0, 0);
        ui_btn(s, 16, 632, 218, 64, "Run again", K_GS "CLEAR_CLOG", V_NORM, I_REDO, B_STACK | B_DNG, 0);
        ui_btn(s, 246, 632, 218, 64, "Purge 20", K_GS "LOAD_FILAMENT%20LENGTH%3D20", V_NORM, I_HEAT, B_STACK, 0);
        return;
    }
    if (k->cstep == 5) snprintf(why, sizeof why, "Last step");
    else snprintf(why, sizeof why, "Pass %d of %d", k->cpass, k->cpasses);
    snprintf(nt, sizeof nt, "%s Each pass takes several minutes, nothing to do here.", st[k->cstep - 1]);
    k_stopcard(s, k, I_HEAT, "CLEARING CLOG", why, nt);
    ui_btn(s, 16, 632, 448, 64, "Emergency stop", "/printer/emergency_stop", V_STOP, I_X, B_DNG, 0);
}

static void k_notready(const surf *s, const kstat *k) {
    char msg[200];
    if (!k->online) {
        ui_empty(s, 200, I_POWER, "Klipper host not answering", "Waiting for it.");
        return;
    }
    if (!strcmp(k->kstate, "startup")) {
        ui_empty(s, 200, I_INFO, "Klipper is starting", "One moment.");
        return;
    }
    snprintf(msg, sizeof msg, "%s", k->kmsg[0] ? k->kmsg : k->kstate);
    for (char *q = msg; *q; q++) if (*q == '\n') *q = ' ';
    ui_empty(s, 140, I_INFO, "Klipper isn't ready", msg);
    ui_btn(s, 16, 604, 448, 92, "Firmware restart", "/printer/firmware_restart", V_ACC, I_REDO, 0, 0);
}

static void k_print(const surf *s, const kmenu *m) {
    static const char *const lab[3] = { "Files", "History", "USB" }, *const tok[3] = { "files", "hist", "usb" };
    char meta[96], hm[24], sz[24], when[32];
    ui_seg(s, 16, 80, 448, 3, lab, tok, m->scr == K_FILES ? 0 : m->scr == K_HIST ? 1 : 2);
    if (m->scr == K_FILES) {
        if (m->nfiles <= 0) {
            ui_empty(s, 240, I_PRINT, m->nfiles < 0 ? "Can't read the file list" : "No files yet",
                     m->nfiles < 0 ? "Moonraker did not answer." : "Send one from the slicer or Mainsail.");
            return;
        }
        for (int i = 0; i < K_ROWS && m->page * K_ROWS + i < nkf; i++) {
            int f = m->page * K_ROWS + i;
            const kmeta *t = &m->rmeta[i];
            /* time and grams from the slicer; the size when the file has neither */
            if (t->est > 0) {
                k_hm(t->est, hm, sizeof hm);
                if (t->grams > 0) snprintf(meta, sizeof meta, "%s · %.0f g", hm, t->grams);
                else snprintf(meta, sizeof meta, "%s", hm);
            } else
                k_size(kf[f].size, meta, sizeof meta);
            ui_li(s, 152 + i * 96, &m->rimg[i], I_PRINT, k_base(kf[f].name), U_FG, NULL, 0, meta, "file", 0, f);
        }
        ui_pager(s, m->page, nkf);
    } else if (m->scr == K_USB) {
        if (m->nusb <= 0) {
            ui_empty(s, 240, I_USB, m->nusb == -1 ? "No stick found" : m->nusb == -2 ? "Can't read the stick"
                     : "No G-code files",
                     m->nusb == -1 ? "Stick in the printer · FAT32 · read only"
                     : m->nusb == -2 ? "It has to be FAT32." : "Only the top folder of the stick is read.");
            return;
        }
        /* two taps on a file copy it to the printer and print it */
        for (int i = 0; i < K_ROWS && m->page * K_ROWS + i < nku; i++) {
            int f = m->page * K_ROWS + i;
            char nm[128];
            const char *x = strrchr(ku[f].name, '.');
            snprintf(nm, sizeof nm, "%.*s", x ? (int)(x - ku[f].name) : (int)strlen(ku[f].name), ku[f].name);
            k_size(ku[f].size, sz, sizeof sz);
            k_when(ku[f].mod, when, sizeof when);
            snprintf(meta, sizeof meta, when[0] ? "%s · %s" : "%s", sz, when);
            ui_li(s, 152 + i * 96, NULL, I_USB, nm, U_FG, NULL, 0, meta, "ufile", B_DNG, f);
        }
        if (nku > K_ROWS) ui_pager(s, m->page, nku);
        else u_text(s, 240, 670, UF(0, 16, 0), "Tap a file twice to copy and print", U_MUT, 1);
    } else {
        if (m->nhist <= 0) {
            ui_empty(s, 240, I_PRINT, m->nhist < 0 ? "Can't read the job history" : "No jobs yet", NULL);
            return;
        }
        for (int i = 0; i < m->nhist; i++) {
            const kjob *j = &kh[i];
            unsigned c;
            const char *st = k_hstat(j->status, &c);
            k_hm(j->dur, hm, sizeof hm);
            k_when(j->end, when, sizeof when);
            snprintf(meta, sizeof meta, "%s%s%s%s", hm, when[0] ? " · " : "", when, j->exists ? "" : " · deleted");
            ui_li(s, 152 + i * 96, &m->rimg[i], I_PRINT, k_base(j->name), j->exists ? U_FG : U_MUT,
                  st, c, meta, "hview", 0, i);
        }
    }
}

static void k_filescr(const surf *s, const kstat *k, const kmenu *m) {
    const kfile *f = &kf[m->sel];
    const kmeta *t = &m->meta;
    char v[6][64], hm[24];
    const char *kv[12];
    int n = 0;
    rr(s, 16, 80, 448, 182, 14, U_SURF);
    ui_thumb(s, 32, 96, 150, &m->img, I_PRINT);
    int nl = u_wrap(s, 196, 126, UF(2, 28, 0), k_base(f->name), U_FG, 252, 30, 3, 0);
    if (t->est > 0) {
        k_hm(t->est, hm, sizeof hm);
        u_text(s, 196, 126 + (nl - 1) * 30 + 50, UF(1, 36, 0), hm, U_FG, 0);
    }
    /* slicers fill in different subsets; show what this one gave */
    if (t->fil > 0) {
        snprintf(v[n], 64, "%s%s%.2f m · %.0f g", t->type, t->type[0] ? " · " : "", t->fil / 1000, t->grams);
        kv[2 * n] = "Filament"; kv[2 * n + 1] = v[n]; n++;
    }
    if (t->ext > 0) {
        snprintf(v[n], 64, "%.0f / %.0f °C", t->ext, t->bed);
        kv[2 * n] = "Temperatures"; kv[2 * n + 1] = v[n]; n++;
    }
    if (t->layers > 0) {
        snprintf(v[n], 64, "%.0f × %.2f mm", t->layers, t->lh);
        kv[2 * n] = "Layers"; kv[2 * n + 1] = v[n]; n++;
    }
    if (t->height > 0) {
        snprintf(v[n], 64, "%.1f mm", t->height);
        kv[2 * n] = "Height"; kv[2 * n + 1] = v[n]; n++;
    }
    snprintf(v[n], 64, "%s · %d mm/s", k_mname[k_modei(k)], k->mode[k_modei(k)][0]);
    kv[2 * n] = "Speed mode"; kv[2 * n + 1] = v[n]; n++;
    k_size(f->size, v[n], 64);
    kv[2 * n] = "Size"; kv[2 * n + 1] = v[n]; n++;
    ui_kv(s, 274, kv, n);
    ui_btn(s, 16, 604, 448, 92, "Print", "print", V_GO, I_PLAY, B_DNG | (k_busy(k) ? B_OFF : 0), 0);
}

static void k_hjobscr(const surf *s, const kstat *k, const kmenu *m) {
    const kjob *j = m->hsrc ? &m->last : &kh[m->hjob];
    const kmeta *t = m->hsrc ? &m->lmeta : &m->rmeta[m->hjob];
    char v[4][64];
    const char *kv[8];
    int n = 0;
    unsigned c;
    rr(s, 16, 80, 448, 182, 14, U_SURF);
    ui_thumb(s, 32, 96, 150, m->hsrc ? &m->limg : &m->rimg[m->hjob], I_PRINT);
    const char *st = k_hstat(j->status, &c);
    ui_pill(s, 196, 96, st, c, U_SURF);
    u_wrap(s, 196, 160, UF(2, 28, 0), k_base(j->name), j->exists ? U_FG : U_MUT, 252, 30, 3, 0);
    k_when(j->end, v[n], 64);
    if (v[n][0]) { kv[2 * n] = "Finished"; kv[2 * n + 1] = v[n]; n++; }
    k_hm(j->dur, v[n], 64);
    kv[2 * n] = "Printed for"; kv[2 * n + 1] = v[n]; n++;
    double g = t->grams > 0 && t->fil > 0 ? t->grams * j->fused / t->fil : 0;
    if (g > 0) snprintf(v[n], 64, "%.2f m · %.0f g", j->fused / 1000, g);
    else snprintf(v[n], 64, "%.2f m", j->fused / 1000);
    kv[2 * n] = "Filament"; kv[2 * n + 1] = v[n]; n++;
    if (t->layers > 0) {
        snprintf(v[n], 64, "%.0f × %.2f mm", t->layers, t->lh);
        kv[2 * n] = "Layers"; kv[2 * n + 1] = v[n]; n++;
    }
    int y = ui_kv(s, 274, kv, n);
    if (!j->exists) u_text(s, 32, y + 34, UF(0, 16, 0), "The file is gone from the printer.", U_WARN, 0);
    ui_btn(s, 16, 604, 448, 92, "Print again", "hjob", V_GO, I_PLAY,
           B_DNG | (k_busy(k) || !j->exists ? B_OFF : 0), m->hjob);
}

static void k_heat(const surf *s, const kstat *k) {
    static const char *const pn[4] = { "PLA", "PETG", "ABS", "TPU" };
    static const int pe[4] = { 210, 240, 245, 225 }, pb[4] = { 60, 70, 100, 50 };
    static const char *const pc[4] = { K_GS "M104%20S210%0AM140%20S60", K_GS "M104%20S240%0AM140%20S70",
                                       K_GS "M104%20S245%0AM140%20S100", K_GS "M104%20S225%0AM140%20S50" };
    static char adj[2][2][48], ld[96], ul[96], pu[96];
    int printing = !strcmp(k->pstate, "printing"), busy = k_busy(k);
    char v[16];
    for (int i = 0; i < 2; i++) {
        int y = 80 + i * 108, lo = i ? 40 : 180, hi = i ? 110 : 260;
        double t = i ? k->bt : k->et, g = i ? k->btt : k->ett;
        int gv = (int)(g + 0.5);
        int dn = gv <= 0 ? -1 : gv <= lo ? 0 : gv - 5, up = gv <= 0 ? lo : gv + 5 > hi ? -1 : gv + 5;
        snprintf(adj[i][0], 48, K_GS "%s%%20S%d", i ? "M140" : "M104", dn);
        snprintf(adj[i][1], 48, K_GS "%s%%20S%d", i ? "M140" : "M104", up);
        rr(s, 16, y, 448, 96, 14, U_SURF);
        ui_lab(s, 28, y + 30, i ? "BED" : "NOZZLE", U_MUT, 0);
        snprintf(v, sizeof v, "%.0f°", t);
        u_text(s, 26, y + 78, UF(1, 46, 0), v, U_FG, 0);
        ui_btn(s, 180, y + 12, 72, 72, "−", adj[i][0], V_NORM, -1, printing || dn < 0 ? B_OFF : 0, 0);
        u_text(s, 316, y + 38, UF(1, 12, 12), "TARGET", U_MUT, 1);
        if (g > 0) snprintf(v, sizeof v, "%.0f°", g);
        else snprintf(v, sizeof v, "off");
        u_text(s, 316, y + 74, UF(1, 30, 0), v, g > 0 ? U_HEAT : U_MUT, 1);
        ui_btn(s, 380, y + 12, 72, 72, "+", adj[i][1], V_NORM, -1, printing || up < 0 ? B_OFF : 0, 0);
    }
    ui_lab(s, 16, 314, "PREHEAT", U_MUT, 0);
    for (int p = 0; p < 4; p++) {
        int x = 16 + p % 3 * 153, y = 326 + p / 3 * 72;
        int on = (int)(k->ett + 0.5) == pe[p] && (int)(k->btt + 0.5) == pb[p];
        char sub[16];
        if (busy) g_op = 82;
        rr(s, x, y, 141, 64, 12, U_SURF2);
        if (on) rring(s, x, y, 141, 64, 12, 4, U_HEAT);
        u_text(s, x + 70, y + 30, UF(1, 18, 0), pn[p], U_FG, 1);
        snprintf(sub, sizeof sub, "%d / %d", pe[p], pb[p]);
        g_op = busy ? 62 : 192;
        u_text(s, x + 70, y + 52, UF(0, 15, 0), sub, U_FG, 1);
        g_op = 256;
        if (!busy) ui_reg(x, y, 141, 64, pn[p], pc[p], 0, 0);
    }
    ui_btn(s, 169, 398, 141, 64, "Off", K_GS "TURN_OFF_HEATERS", V_GHOST, I_X,
           busy || (k->ett <= 0 && k->btt <= 0) ? B_OFF : 0, 0);
    if (printing)
        u_text(s, 16, 494, UF(0, 16, 0), "Change temperatures during a print in Home › Tune.", U_MUT, 0);
    if (k_filrun(k)) { k_filbar(s, k, 526); return; }
    int ft = k->ett > 0 ? (int)(k->ett + 0.5) : 220;
    if (busy) {
        snprintf(ld, sizeof ld, K_GS "LOAD_FILAMENT");
        snprintf(ul, sizeof ul, K_GS "UNLOAD_FILAMENT");
        snprintf(pu, sizeof pu, K_GS "LOAD_FILAMENT%%20LENGTH%%3D20");
    } else {
        snprintf(ld, sizeof ld, K_GS "LOAD_FILAMENT%%20TEMP%%3D%d", ft);
        snprintf(ul, sizeof ul, K_GS "UNLOAD_FILAMENT%%20TEMP%%3D%d", ft);
        snprintf(pu, sizeof pu, K_GS "LOAD_FILAMENT%%20TEMP%%3D%d%%20LENGTH%%3D20", ft);
    }
    ui_lab(s, 16, 620, !busy && k->ett <= 0 ? "FILAMENT · HEATS TO 220 °C FIRST" : "FILAMENT", U_MUT, 0);
    ui_btn(s, 16, 632, 141, 64, "Load", ld, V_GO, I_SPOOL, B_STACK | (printing ? B_OFF : 0), 0);
    ui_btn(s, 169, 632, 142, 64, "Unload", ul, V_NORM, I_SPOOL, B_STACK | (printing ? B_OFF : 0), 0);
    ui_btn(s, 323, 632, 141, 64, "Purge 20", pu, V_NORM, I_HEAT, B_STACK | (printing ? B_OFF : 0), 0);
}

static void k_tools(const surf *s, const kstat *k) {
    static const char *const lcmd[3] = { K_GS "SET_PIN%20PIN%3Dlight%20VALUE%3D1",
                                         K_GS "SET_PIN%20PIN%3Dlight%20VALUE%3D0.3",
                                         K_GS "SET_PIN%20PIN%3Dlight%20VALUE%3D0" };
    char fan[48], lt[16];
    int busy = k_busy(k), li = k->light <= 0 ? 0 : k->light >= 1 ? 1 : 2;
    if (k->pfan > 0 || k->cfan > 0)
        snprintf(fan, sizeof fan, "part %.0f %% · chamber %.0f %%", k->pfan * 100, k->cfan * 100);
    else snprintf(fan, sizeof fan, "both off");
    if (k->light < 0) snprintf(lt, sizeof lt, "not fitted");
    else if (li == 2) snprintf(lt, sizeof lt, "%.0f %%", k->light * 100);
    else snprintf(lt, sizeof lt, li ? "On" : "Off");
    ui_tile(s, 16, 80, I_MOVE, "Move", busy ? "not while printing" : strlen(k->homed) == 3 ? "homed" : "not homed",
            "move", busy ? B_OFF : 0);
    ui_tile(s, 246, 80, I_FAN, "Fans", fan, "fans", 0);
    ui_tile(s, 16, 240, I_LIGHT, "Light", lt, lcmd[li], k->light < 0 ? B_OFF : 0);
    ui_tile(s, 246, 240, I_GAUGE, "Speed mode", k_mname[k_modei(k)], "mode", 0);
    ui_tile(s, 16, 400, I_POWER, "Motors off", busy ? "not while printing" : "frees the axes", K_GS "M84",
            B_DNG | (busy ? B_OFF : 0));
    ui_tile(s, 246, 400, I_INFO, "System", "network · versions · restart", "about", 0);
    ui_btn(s, 16, 560, 448, 80, "Clear clog", K_GS "CLEAR_CLOG", V_NORM, I_HEAT, B_DNG | (busy ? B_OFF : 0), 0);
}

static void k_fans(const surf *s, const kstat *k) {
    static const char *const lab[4] = { "Off", "30 %", "50 %", "100 %" };
    static const char *const cmd[2][4] = {
        { K_GS "M107", K_GS "M106%20S77", K_GS "M106%20S128", K_GS "M106%20S255" },
        { K_GS "SET_FAN_SPEED%20FAN%3Dchamber_fan%20SPEED%3D0", K_GS "SET_FAN_SPEED%20FAN%3Dchamber_fan%20SPEED%3D0.3",
          K_GS "SET_FAN_SPEED%20FAN%3Dchamber_fan%20SPEED%3D0.5", K_GS "SET_FAN_SPEED%20FAN%3Dchamber_fan%20SPEED%3D1" } };
    static const char *const nt[2] = { "The slicer sets this during prints.",
                                       "Prints set it from the material: PLA 100 %, PETG 50 %, ABS off." };
    int y = 80;
    for (int i = 0; i < 2; i++) {
        int v = (int)((i ? k->cfan : k->pfan) * 100 + 0.5);
        int on = !v ? 0 : abs(v - 30) <= 2 ? 1 : abs(v - 50) <= 2 ? 2 : v >= 98 ? 3 : -1;
        int nl = u_wrap(NULL, 32, 0, UF(0, 16, 0), nt[i], U_MUT, 416, 21, 2, 0);
        int h = 16 + 30 + 12 + 60 + 12 + nl * 21 + 12;
        char val[16];
        rr(s, 16, y, 448, h, 14, U_SURF);
        ui_lab(s, 32, y + 40, i ? "CHAMBER FAN" : "PART FAN", U_MUT, 0);
        if (v) snprintf(val, sizeof val, "%d %%", v);
        else snprintf(val, sizeof val, "Off");
        u_text(s, 448, y + 42, UF(1, 30, 0), val, U_FG, 2);
        ui_seg(s, 32, y + 58, 416, 4, lab, cmd[i], on);
        u_wrap(s, 32, y + 58 + 60 + 28, UF(0, 16, 0), nt[i], U_MUT, 416, 21, 2, 0);
        y += h + 12;
    }
}

static void k_move(const surf *s, const kstat *k, const kmenu *m) {
    /* relative jogs inside a saved gcode state; XY 50 mm/s, Z 5 mm/s */
    static const char *const lab[6] = { "X−", "X+", "Y−", "Y+", "Z−", "Z+" };
    static const int jx[6] = { 16, 228, 122, 122, 368, 368 }, jy[6] = { 282, 282, 388, 176, 388, 176 };
    static const char *const sl[4] = { "0.1 mm", "1 mm", "10 mm", "50 mm" };
    static const char *const st[4] = { "step", "step", "step", "step" };
    static char jog[6][160];
    double d = k_step[m->step];
    char v[16];
    rr(s, 16, 80, 448, 56, 12, U_SURF);
    for (int i = 0; i < 3; i++) {
        static const char *const ax[3] = { "X", "Y", "Z" };
        snprintf(v, sizeof v, "%.1f", k->pos[i]);
        u_text(s, u_text(s, 34 + i * 149, 117, UF(0, 24, 0), ax[i], U_MUT, 0) + 10, 117, UF(1, 24, 0), v, U_FG, 0);
    }
    int homed = strlen(k->homed) == 3;
    u_text(s, 16, 166, UF(0, 16, 0), homed ? "Y moves the bed; X and Z move the head." : "Not homed. Tap Home first.",
           homed ? U_MUT : U_WARN, 0);
    for (int i = 0; i < 6; i++) {
        char a = "XXYYZZ"[i];
        snprintf(jog[i], sizeof jog[i], K_GS "SAVE_GCODE_STATE%%20NAME%%3Djog%%0AG91%%0A"
                 "G1%%20%c%s%g%%20F%d%%0ARESTORE_GCODE_STATE%%20NAME%%3Djog",
                 a, i % 2 ? "" : "-", d, i < 4 ? 3000 : 300);
        ui_btn(s, jx[i], jy[i], 96, 96, lab[i], jog[i], V_NORM, -1, strchr(k->homed, a | 32) ? 0 : B_OFF, 0);
    }
    ui_btn(s, 122, 282, 96, 96, "Home", K_GS "HOME_XYZ", V_ACC, I_HOMEAX, B_STACK, 0);
    u_text(s, 416, 335, UF(1, 15, 10), "Z", U_MUT, 1);
    ui_lab(s, 16, 624, "STEP", U_MUT, 0);
    ui_seg(s, 16, 636, 448, 4, sl, st, m->step);
}

static void k_tune(const surf *s, const kstat *k) {
    static const char *const lab[5] = { "SPEED", "FLOW", "Z OFFSET", "NOZZLE", "PART FAN" };
    static const double step[5] = { 10, 2, 0.02, 5, 10 };
    static const double lo[5] = { 20, 80, -1, 180, 0 }, hi[5] = { 800, 200, 1, 260, 100 };
    static const double def[5] = { 100, 100, 0, -1, -1 };
    static char cmd[5][2][96];
    double val[5] = { k->speed * 100, k->flow * 100, k->zoff, k->ett, k->pfan * 100 };
    for (int i = 0; i < 5; i++) {
        int y = 80 + i * 86;
        for (int d = 0; d < 2; d++) {
            double v = val[i] + (d ? step[i] : -step[i]);
            v = (long)(v / step[i] + (v < 0 ? -0.5 : 0.5)) * step[i];
            int ok = v >= lo[i] - 1e-6 && v <= hi[i] + 1e-6;
            switch (i) {
            case 0: snprintf(cmd[i][d], 96, K_GS "M220%%20S%.0f", v); break;
            case 1: snprintf(cmd[i][d], 96, K_GS "M221%%20S%.0f", v); break;
            case 2: snprintf(cmd[i][d], 96, K_GS "SET_GCODE_OFFSET%%20Z_ADJUST%%3D%.2f%%20MOVE%%3D1",
                             d ? step[i] : -step[i]); break;
            case 3: snprintf(cmd[i][d], 96, K_GS "M104%%20S%.0f", v); break;
            case 4: snprintf(cmd[i][d], 96, K_GS "M106%%20S%.0f", v * 2.55); break;
            }
            if (i == 3 && k->ett <= 0) ok = 0;   /* not re-heating a nozzle that is off */
            ui_btn(s, d ? 384 : 16, y, 80, 76, d ? "+" : "−", cmd[i][d], V_NORM, -1, ok ? 0 : B_OFF, 0);
        }
        char t[24];
        if (i == 2) snprintf(t, sizeof t, "%+.2f mm", val[i]);
        else if (i == 3) snprintf(t, sizeof t, val[i] > 0 ? "%.0f °C" : "off", val[i]);
        else snprintf(t, sizeof t, "%.0f %%", val[i]);
        double dv = val[i] - def[i];
        rr(s, 106, y, 268, 76, 12, U_SURF);
        u_text(s, 240, y + 26, UF(1, 13, 13), lab[i], U_MUT, 1);
        u_text(s, 240, y + 62, UF(1, 30, 0), t, def[i] >= -0.5 && (dv > 0.001 || dv < -0.001) ? U_WARN : U_FG, 1);
    }
    ui_btn(s, 16, 632, 218, 64, "Speed mode", "mode", V_NORM, I_GAUGE, 0, 0);
    ui_btn(s, 246, 632, 218, 64, "Objects", "objs", V_NORM, I_OBJS, 0, 0);
}

static void k_objsscr(const surf *s, const kmenu *m) {
    static char cmd[K_MAXO][240];
    const int x0 = 130, y0 = 92;
    rr(s, 16, 80, 448, 224, 14, U_SURF);
    rr(s, x0, y0, 220, 200, 8, U_SURF2);
    if (m->nobj <= 0) {
        u_wrap(s, 240, 180, UF(0, 16, 0), m->nobj < 0 ? "Could not read the objects."
               : "No object labels in this file. Turn on label objects in the slicer.", U_MUT, 196, 21, 4, 1);
        return;
    }
    for (int i = 0; i < m->nobj; i++) {
        const kobj *o = &ko[i];
        float p[K_MAXP][2];
        for (int j = 0; j < o->np; j++) { p[j][0] = x0 + (float)o->p[j][0]; p[j][1] = y0 + 200 - (float)o->p[j][1]; }
        if (!o->excl && o->np > 2) poly_fill(s, p, o->np, o->cur ? U_ACC : U_MUT, o->cur ? 90 : 64);
        for (int j = 0; j < o->np; j++) {
            const float *A = p[j], *B = p[(j + 1) % o->np];
            if (!o->excl) { aa_line(s, A[0], A[1], B[0], B[1], 1, o->cur ? U_ACC : U_DIM); continue; }
            float dx = B[0] - A[0], dy = B[1] - A[1], L = sqrtf(dx * dx + dy * dy);
            for (float d = 0; d < L; d += 9) {
                float e = d + 5 < L ? d + 5 : L;
                aa_line(s, A[0] + dx * d / L, A[1] + dy * d / L, A[0] + dx * e / L, A[1] + dy * e / L, 1, U_DIM);
            }
        }
        char nb[4];
        snprintf(nb, sizeof nb, "%d", i + 1);
        u_text(s, x0 + (int)o->cx, y0 + 200 - (int)o->cy + 6, UF(1, 17, 0), nb, o->excl ? U_DIM : U_FG, 1);
    }
    for (int i = 0; i < m->nobj; i++) {
        const kobj *o = &ko[i];
        int y = 316 + i * 62;
        char nm[64], nb[4], en[160];
        k_oname(o->name, nm, sizeof nm);
        snprintf(nb, sizeof nb, "%d", i + 1);
        rr(s, 16, y, 448, 56, 12, U_SURF);
        u_text(s, 32, y + 25, UF(1, 18, 0), nb, U_FG, 0);
        u_text(s, 58, y + 25, UF(0, 18, 0), u_fit(UF(0, 18, 0), nm, 260), o->excl ? U_MUT : U_FG, 0);
        u_text(s, 58, y + 46, UF(1, 13, 10), o->excl ? "SKIPPED" : o->cur ? "PRINTING NOW" : "WAITING",
               o->excl ? U_STOP : o->cur ? U_ACC : U_MUT, 0);
        urlenc(o->name, en, sizeof en);
        snprintf(cmd[i], sizeof cmd[i], K_GS "EXCLUDE_OBJECT%%20NAME%%3D%s", en);
        if (!o->excl && m->nobj > 1)   /* never the last one: that is CANCEL */
            ui_btn(s, 340, y + 6, 112, 44, "Skip", cmd[i], V_STOP, -1, B_DNG, 0);
    }
}

/* SPEED MODE: a row per mode of printer.cfg's SPEED_MODE, saved for the
 * next job and applied at once during one. BRACE sits behind the gamepad. */
static void k_modescr(const surf *s, const kstat *k) {
    static char cmd[4][64];
    char sub[64], num[16];
    ufont U = UF(0, 15, 0);
    int wu = u_tw(U, " mm/s");
    for (int i = 0; i < 5; i++) {
        const int *v = k->mode[i];
        int y = 80 + i * 84, on = !strcmp(k->smode, k_mname[i]) || (!k->smode[0] && i == 0);
        unsigned c = i == 4 ? U_STOP : U_ACC;
        rr(s, 16, y, 448, 76, 12, i == 4 ? mix(U_SCR, U_STOP, 31) : U_SURF);
        if (on) rring(s, 16, y, 448, 76, 12, 4, c);
        u_text(s, 32, y + 33, UF(2, 22, 10), i == 4 ? "BRACE YOURSELF" : k_mname[i], i == 4 ? U_STOP : on ? U_ACC : U_FG, 0);
        int sx = 32;
        if (i == 4) { icon(s, I_LOCK, 32, y + 46, 15, U_MUT); sx = 52; }
        if (v[2] > 0) snprintf(sub, sizeof sub, "%saccel %d · %d °C · flow %d %%", i == 4 ? "locked · " : "", v[1], v[2], v[3]);
        else snprintf(sub, sizeof sub, "accel %d · slicer temp · flow %d %%", v[1], v[3]);
        u_text(s, sx, y + 59, U, sub, U_MUT, 0);
        snprintf(num, sizeof num, v[0] > 0 ? "%d" : "--", v[0]);
        u_text(s, 448 - wu, y + 49, UF(1, 30, 0), num, U_FG, 2);
        u_text(s, 448 - wu, y + 49, U, " mm/s", U_MUT, 0);
        if (i == 4) { ui_reg(16, y, 448, 76, "BRACE", "pad", 0, 0); break; }
        snprintf(cmd[i], sizeof cmd[i], K_GS "SPEED_MODE%%20NAME%%3D%s", k_mname[i]);
        ui_reg(16, y, 448, 76, k_mname[i], cmd[i], 0, 0);
    }
    char nt[96];
    snprintf(nt, sizeof nt, "%s Saved until you change it.",
             !strcmp(k->pstate, "printing") ? "Applies now." : !strcmp(k->pstate, "paused")
             ? "Applies when you tap Ready." : "Applies to the next print.");
    u_text(s, 16, 680, UF(0, 16, 0), nt, U_MUT, 0);
}

/* the gamepad that unlocks BRACE: the Konami code, checked in k_nav */
static void k_padscr(const surf *s, const kmenu *m) {
    static const char *const tok[4] = { "padU", "padL", "padR", "padD" };
    static const int dx[4] = { 108, 36, 180, 108 }, dy[4] = { 224, 296, 296, 368 }, ai[4] = { I_AU, I_AL, I_AR, I_AD };
    int n = (int)strlen(m->pad);
    ui_lab(s, 240, 112, "ENTER THE CODE", U_MUT, 1);
    for (int i = 0; i < 10; i++)
        rr(s, 115 + i * 26, 128, 16, 16, 8, m->padbad ? U_STOP : i < n ? U_ACC : U_SURF2);
    rr(s, 16, 200, 448, 260, 22, U_SURF);
    rr(s, 108, 296, 68, 68, 10, U_SURF2);
    for (int i = 0; i < 4; i++) {
        rr(s, dx[i], dy[i], 68, 68, 10, U_SURF2);
        icon(s, ai[i], dx[i] + 18, dy[i] + 18, 32, U_FG);
        ui_reg(dx[i], dy[i], 68, 68, "", tok[i], 0, 0);
    }
    for (int i = 0; i < 2; i++) {
        int cx = i ? 400 : 304, cy = i ? 296 : 364;
        rr(s, cx - 42, cy - 42, 84, 84, 42, U_STOP);
        u_text(s, cx, cy + 10, UF(1, 28, 0), i ? "A" : "B", 0xffffffu, 1);
        ui_reg(cx - 42, cy - 42, 84, 84, "", i ? "padA" : "padB", 0, 0);
    }
    if (m->padbad) u_text(s, 240, 506, UF(1, 18, 0), "Wrong. Start over.", U_STOP, 1);
}

static void k_bracescr(const surf *s, const kstat *k) {
    static const char *const p1 = "The machine's limit, for fast prints that need neither looks nor strength. Stay at the printer.";
    static const char *const p2 = "It locks again when the job ends; the next one runs MCQUEEN.";
    const int *v = k->mode[4];
    ufont P = UF(0, 17, 0);
    char sp[80];
    int n1 = u_wrap(NULL, 36, 0, P, p1, U_FG, 408, 23, 5, 0), n2 = u_wrap(NULL, 36, 0, P, p2, U_FG, 408, 23, 4, 0);
    int h = 20 + 30 + 16 + 20 + 16 + n1 * 23 + 10 + n2 * 23 + 12;
    rr(s, 16, 80, 448, h, 14, mix(U_SCR, U_STOP, 36));
    rring(s, 16, 80, 448, h, 14, 3, mix(U_SCR, U_STOP, 128));
    u_text(s, 36, 128, UF(2, 32, 10), "BRACE YOURSELF", U_STOP, 0);
    snprintf(sp, sizeof sp, "%d mm/s · accel %d · %d °C · flow %d %%", v[0], v[1], v[2], v[3]);
    u_text(s, 36, 164, UF(1, 18, 0), u_fit(UF(1, 18, 0), sp, 408), U_FG, 0);
    u_wrap(s, 36, 198, P, p1, U_FG, 408, 23, 5, 0);
    u_wrap(s, 36, 198 + n1 * 23 + 10, P, p2, U_FG, 408, 23, 4, 0);
    ui_btn(s, 16, 604, 448, 92, "Proceed", K_GS "SPEED_MODE%20NAME%3DBRACE%20CODE%3DUUDDLRLRBA", V_STOP, -1, B_DNG, 0);
}

static void k_about(const surf *s, const kstat *k, const char *eth, const char *wifi, const char *hostport) {
    char e[48];
    snprintf(e, sizeof e, "eth0 %s", eth);
    const char *kv[10] = { "Printer link", e, "Wi-Fi", wifi, "Moonraker", hostport,
                           "Klipper", k->ver[0] ? k->ver : "?", "Source", "github.com/Mcsketch/ad4-klipper" };
    ui_kv(s, 80, kv, 5);
    int off = k_busy(k) ? B_OFF : 0;
    ui_btn(s, 16, 632, 218, 64, "Restart Klipper", "/printer/restart", V_NORM, -1, B_DNG | off, 0);
    ui_btn(s, 246, 632, 218, 64, "Firmware restart", "/printer/firmware_restart", V_NORM, -1, B_DNG | off, 0);
}

static int k_tab(int scr) {
    return scr == K_HOME || scr == K_TUNE || scr == K_OBJS ? 0
         : scr == K_FILES || scr == K_FILE || scr == K_HIST || scr == K_USB || scr == K_HJOB ? 1
         : scr == K_HEAT ? 2 : 3;
}

static void k_paint(const surf *s, const kstat *k, const kmenu *m, const char *eth, const char *wifi,
                    const char *note, const char *hostport) {
    static const char *const title[] = {
        [K_HOME] = "Adventurer 4", [K_FILES] = "Print", [K_FILE] = "Print file", [K_HEAT] = "Heat & filament",
        [K_TOOLS] = "Tools", [K_ABOUT] = "System", [K_MOVE] = "Move", [K_TUNE] = "Tune", [K_OBJS] = "Objects",
        [K_HIST] = "Print", [K_USB] = "Print", [K_MODE] = "Speed mode", [K_PAD] = "Unlock",
        [K_BRACE] = "Brace yourself", [K_FANS] = "Fans", [K_HJOB] = "Past job" };
    const char *lab, *t = title[m->scr];
    unsigned pc = k_pill(k, &lab);
    int ok = k->online && !strcmp(k->kstate, "ready"), busy = ok && k_busy(k);
    int paused = busy && !strcmp(k->pstate, "paused");
    int root = m->scr == K_HOME || m->scr == K_FILES || m->scr == K_HIST || m->scr == K_USB ||
               m->scr == K_HEAT || m->scr == K_TOOLS;
    if (m->scr == K_HOME && k->cstep) t = "Clear clog";
    if (m->scr == K_HOME && busy) t = paused ? (k->swap ? "Filament" : k->dmsg[0] ? "Plate stop" : "Paused") : "Printing";
    g_nb = 0;
    fill(s, 0, 0, s->w, s->h, U_SCR);
    ui_header(s, t, !root, lab, pc);
    switch (m->scr) {
    case K_HOME:
        if (!ok) k_notready(s, k);
        else if (paused) k_paused(s, k);
        else if (busy) k_printing(s, k, m);
        else if (k->cstep) k_clog(s, k);
        else k_homeidle(s, k, m);
        break;
    case K_FILES: case K_HIST: case K_USB: k_print(s, m); break;
    case K_FILE: k_filescr(s, k, m); break;
    case K_HJOB: k_hjobscr(s, k, m); break;
    case K_HEAT: k_heat(s, k); break;
    case K_TOOLS: k_tools(s, k); break;
    case K_FANS: k_fans(s, k); break;
    case K_MOVE: k_move(s, k, m); break;
    case K_TUNE: k_tune(s, k); break;
    case K_OBJS: k_objsscr(s, m); break;
    case K_MODE: k_modescr(s, k); break;
    case K_PAD: k_padscr(s, m); break;
    case K_BRACE: k_bracescr(s, k); break;
    case K_ABOUT: k_about(s, k, eth, wifi, hostport); break;
    }
    int tab = k_tab(m->scr == K_MODE || m->scr == K_PAD || m->scr == K_BRACE ? m->mback : m->scr);
    ui_tabs(s, tab, busy ? (paused ? U_WARN : U_ACC) : 0, !ok);
    if (note[0]) ui_toast(s, note);
}

/* home's last job, from the history: refreshed while home is idle */
static void k_last(const char *host, int port, kmenu *m) {
    static kmeta hm[K_ROWS];
    int n = k_hist(host, port, hm);
    if (n <= 0) { m->nlast = 0; return; }
    int same = m->nlast && !strcmp(m->last.name, kh[0].name) && m->limg.px;
    m->last = kh[0];
    m->lmeta = hm[0];
    m->nlast = 1;
    if (!same) k_thumb(host, port, kh[0].name, &m->lmeta, &m->limg);
}


static void if_ip(const char *ifn, char *out, int n) {
    struct ifreq r;
    memset(&r, 0, sizeof r);
    strncpy(r.ifr_name, ifn, IFNAMSIZ - 1);
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd >= 0 && ioctl(fd, SIOCGIFADDR, &r) == 0)
        inet_ntop(AF_INET, &((struct sockaddr_in *)&r.ifr_addr)->sin_addr, out, n);
    else
        snprintf(out, n, "not connected");
    if (fd >= 0) close(fd);
}

/* A tap on a button that changes screens rather than sending anything. */
/* the Print list's visible rows: metadata and thumbnail, once per page */
static void k_rows(const char *host, int port, kmenu *m) {
    for (int i = 0; i < K_ROWS; i++) {
        int f = m->page * K_ROWS + i;
        if (f < nkf) k_meta(host, port, kf[f].name, &m->rmeta[i]);
        else memset(&m->rmeta[i], 0, sizeof m->rmeta[i]);
        k_thumb(host, port, f < nkf ? kf[f].name : "", &m->rmeta[i], &m->rimg[i]);
    }
}

/* Cost rates for the end-of-job card, from Moonraker's database (namespace
 * ad4, key cost: price_kg, density, machine_h); unset leaves cost off. */
static void k_rates(const char *host, int port, kmenu *m) {
    static char buf[1024];
    const char *b, *e;
    memset(m->rate, 0, sizeof m->rate);
    if (http_req(host, port, "GET", "/server/database/item?namespace=ad4&key=cost", buf, sizeof buf) != 200 ||
        !(b = jobj(buf, "value", &e)))
        return;
    m->rate[0] = jnum(b, e, "price_kg");
    m->rate[1] = jnum(b, e, "density");
    m->rate[2] = jnum(b, e, "machine_h");
}

static void k_nav(const char *host, int port, kmenu *m, const btn *b) {
    const char *t = b->token;
    if (!strcmp(t, "home")) m->scr = K_HOME;
    else if (!strcmp(t, "files")) { m->scr = K_FILES; m->page = 0; m->nfiles = k_files(host, port); k_rows(host, port, m); }
    else if (!strcmp(t, "ack")) { m->ack = 1; m->lt = 0; }
    else if (!strcmp(t, "usb")) { m->scr = K_USB; m->page = 0; m->nusb = k_usb(); }
    else if (!strcmp(t, "hist")) {
        m->scr = K_HIST;
        m->nhist = k_hist(host, port, m->rmeta);
        for (int i = 0; i < K_ROWS; i++)
            k_thumb(host, port, i < m->nhist ? kh[i].name : "", &m->rmeta[i], &m->rimg[i]);
    }
    else if (!strcmp(t, "hview") || !strcmp(t, "hlast")) {
        m->hsrc = t[1] == 'l';
        if (!m->hsrc && b->arg >= m->nhist) return;
        if (m->hsrc && !m->nlast) return;
        m->hjob = b->arg;
        m->hback = m->scr;
        m->scr = K_HJOB;
    }
    else if (!strcmp(t, "file")) {
        m->sel = b->arg;
        k_meta(host, port, kf[m->sel].name, &m->meta);
        k_thumb(host, port, kf[m->sel].name, &m->meta, &m->img);
        m->scr = K_FILE;
    }
    else if (!strcmp(t, "heat")) m->scr = K_HEAT;
    else if (!strcmp(t, "tools")) m->scr = K_TOOLS;
    else if (!strcmp(t, "about")) m->scr = K_ABOUT;
    else if (!strcmp(t, "fans")) m->scr = K_FANS;
    else if (!strcmp(t, "move")) m->scr = K_MOVE;
    else if (!strcmp(t, "tune")) m->scr = K_TUNE;
    else if (!strcmp(t, "mode")) { m->mback = m->scr; m->scr = K_MODE; }
    else if (!strcmp(t, "pad")) { m->scr = K_PAD; m->pad[0] = 0; m->padbad = 0; }
    else if (!strncmp(t, "pad", 3)) {
        /* up up down down left right left right B A; a wrong press starts over */
        static const char code[] = "UUDDLRLRBA";
        int n = (int)strlen(m->pad);
        m->padbad = code[n] != t[3];
        if (m->padbad) m->pad[0] = 0;
        else { m->pad[n] = t[3]; m->pad[n + 1] = 0; }
        if (!strcmp(m->pad, code)) { m->scr = K_BRACE; m->pad[0] = 0; }
    }
    else if (!strcmp(t, "objs")) { m->scr = K_OBJS; m->nobj = k_objs(host, port); }
    else if (!strcmp(t, "step")) m->step = b->arg;
    else if (!strcmp(t, "prev")) { m->page--; if (m->scr == K_FILES) k_rows(host, port, m); }
    else if (!strcmp(t, "next")) { m->page++; if (m->scr == K_FILES) k_rows(host, port, m); }
    else if (!strcmp(t, "back"))
        m->scr = m->scr == K_FILE ? K_FILES
               : m->scr == K_HJOB ? m->hback
               : m->scr == K_ABOUT || m->scr == K_MOVE || m->scr == K_FANS ? K_TOOLS
               : m->scr == K_OBJS ? K_TUNE
               : m->scr == K_PAD || m->scr == K_BRACE ? K_MODE
               : m->scr == K_MODE ? m->mback : K_HOME;
}

/* the copy from the stick: drawn straight to the screen between chunks */
typedef struct { surf *s, *fb; size_t len; const char *name; } kcopy;

static void k_copyprog(void *v, double done, double size) {
    kcopy *c = v;
    char ln[64], a[24], b[24];
    fill(c->s, 0, 0, c->s->w, c->s->h, U_SCR);
    g_nb = 0;
    ui_header(c->s, "Copying from USB", 0, "COPYING", U_ACC);
    rr(c->s, 16, 80, 448, 200, 14, U_SURF);
    icon(c->s, I_USB, 32, 96, 36, U_ACC);
    int n = u_wrap(c->s, 32, 170, UF(2, 24, 0), c->name, U_FG, 416, 26, 2, 0);
    k_size(done, a, sizeof a);
    k_size(size, b, sizeof b);
    snprintf(ln, sizeof ln, "%s of %s", a, b);
    u_text(c->s, 32, 170 + n * 26 + 8, UF(0, 17, 0), ln, U_MUT, 0);
    ui_bar(c->s, 32, 250, 416, size > 0 ? done / size : 0, U_ACC);
    u_text(c->s, 240, 340, UF(0, 17, 0), "Leave the stick in.", U_MUT, 1);
    if (c->fb) memcpy(c->fb->mem, c->s->mem, c->len);
}

static int cmd_klipper(const char *dump, const char *hostport, const char *screen) {
    char host[64];
    int port = 8095;
    snprintf(host, sizeof host, "%s", hostport ? hostport : "192.168.100.10");
    char *c = strchr(host, ':');
    if (c) { *c = 0; port = atoi(c + 1); }
    char hp[80];
    snprintf(hp, sizeof hp, "%s:%d", host, port);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);         /* the end-of-job cue plays in a child */

    surf fbs, s;
    int fbfd = -1;
#ifdef AD4_DUMP
    (void)fbfd;
    if (!dump) { fprintf(stderr, "host build: use --dump\n"); return 1; }
    memset(&fbs, 0, sizeof fbs);
    fbs.fw = 800; fbs.fh = 480; fbs.w = 480; fbs.h = 800;
    fbs.bpp = 16; fbs.stride = 960;
    fbs.ro = 11; fbs.rl = 5; fbs.go = 5; fbs.gl = 6; fbs.bo = 0; fbs.bl = 5;
#else
    if ((fbfd = fb_open(&fbs)) < 0) return 1;
#endif
    /* Paint off-screen and copy once a frame, or the gradient redraw flickers. */
    size_t len = (size_t)fbs.stride * (fbs.rot ? fbs.fh : fbs.h);
    s = fbs;
    s.mem = malloc(len);
    if (!s.mem) return 1;

    tscal tc;
    int tfd = dump ? -1 : ts_open(&tc);
    kstat k;
    kmenu m;
    memset(&m, 0, sizeof m);
    /* --dump only: open a sub-screen for the preview */
    if (dump && screen) {
        btn nav = { 0 };
        nav.token = !strcmp(screen, "file") ? "files" : screen;
        k_nav(host, port, &m, &nav);
        if (!strcmp(screen, "hjob")) { nav.token = "hist"; k_nav(host, port, &m, &nav); nav.token = "hview"; }
        else if (!strcmp(screen, "file") && nkf > 0) nav.token = "file";
        else nav.token = NULL;
        if (nav.token) k_nav(host, port, &m, &nav);
        if (!strcmp(screen, "brace")) m.scr = K_BRACE;
    }
    m.step = 2;
    btn *b = g_b;
    int nb = 0, armed = -1, pressed = 0, was = 0;
    long next = 0, armed_t = 0, note_t = 0, rst_t = -60000, touch_t = now_ms();
    char eth[32] = "", wifi[32] = "", note[48] = "";
    for (;;) {
        long t = now_ms();
        if (armed >= 0 && t - armed_t > 5000) { armed = -1; next = 0; }
        if (note[0] && t - note_t > 8000) note[0] = 0;
        if (t >= next) {
            k_poll(host, port, &k);
            if_ip("eth0", eth, sizeof eth);
            if_ip("wlan0", wifi, sizeof wifi);
            if (k_linkloss(&k) && t - rst_t > 20000) {
                char r[1024];
                int rc = http_req(host, port, "POST", "/printer/firmware_restart", r, sizeof r);
                fprintf(stderr, "klipper: link loss, firmware_restart -> %d\n", rc);
                snprintf(note, sizeof note, "auto firmware restart");
                note_t = rst_t = t;
            }
            /* the job's metadata and thumbnail: once per file, or every 10 s
             * until Moonraker has finished reading the file */
            if ((k_busy(&k) || k_ended(&k)) &&
                (strcmp(m.jfile, k.file) || (!m.jmeta.ok && t - m.jt > 10000))) {
                snprintf(m.jfile, sizeof m.jfile, "%s", k.file);
                k_meta(host, port, k.file, &m.jmeta);
                k_thumb(host, port, k.file, &m.jmeta, &m.jimg);
                k_rates(host, port, &m);
                m.jt = t;
            }
            /* a job just ended: show its card, and the riff when it finished */
            int started = k_busy(&k) && !was;
            if (k_busy(&k)) m.ack = 0;
            else if (was && k_ended(&k)) {
                k_rates(host, port, &m);
                m.lt = 0;
#ifndef AD4_DUMP
                if (!strcmp(k.pstate, "complete") && fork() == 0) _exit(buzz_cue(AD4_RIFF) ? 1 : 0);
                if (!strcmp(k.pstate, "error") && fork() == 0) _exit(buzz_cue(AD4_FAIL) ? 1 : 0);
#endif
            }
            was = k_busy(&k);
#ifndef AD4_DUMP
            static int swapped;
            if (k.swap && !swapped && fork() == 0) _exit(buzz_cue(AD4_FAIL) ? 1 : 0);
            swapped = k.swap;
            static int cleared;
            if (k.cstep == 6 && cleared != 6 && fork() == 0) _exit(buzz_cue(AD4_FAIL) ? 1 : 0);
            cleared = k.cstep;
#endif
            /* a job starting, a fault, or two idle minutes: back home; TUNE
             * and OBJECTS live only during a job, MOVE only outside one */
            int ok = k.online && !strcmp(k.kstate, "ready");
            int stay = !ok ? 0 : m.scr == K_TUNE || m.scr == K_OBJS ? k_busy(&k) : m.scr == K_MOVE ? !k_busy(&k) : 1;
            if ((started && !dump) || k.cstep) stay = 0;
            if (m.scr == K_OBJS && stay) m.nobj = k_objs(host, port);
            if (m.scr != K_HOME && (!stay || (!dump && t - touch_t > 120000))) {
                m.scr = K_HOME;
                armed = -1;
            }
            if (m.scr != K_USB) k_usbumount();
            /* home's last job: once a minute while idle */
            if (m.scr == K_HOME && k_idle(&k) && !k_ended(&k) && (!m.lt || t - m.lt > 60000)) {
                k_last(host, port, &m);
                m.lt = t ? t : 1;
            }
            g_armed = armed;
            k_paint(&s, &k, &m, eth, wifi, note, hp);
            nb = g_nb;
            if (armed >= nb) armed = -1;
#ifdef AD4_DUMP
            if (dump) { dump_ppm(&s, dump); return 0; }
#endif
            memcpy(fbs.mem, s.mem, len);
            next = now_ms() + 1000;
        }
        int wait = (int)(next - now_ms());
        if (wait < 0) wait = 0;
        if (tfd < 0) { poll(NULL, 0, wait); continue; }
        struct pollfd p = { .fd = tfd, .events = POLLIN };
        if (poll(&p, 1, wait) <= 0) continue;
        int x, y, r;
        /* ts_read reports every sample of a press; act on the first only */
        while ((r = ts_read(tfd, &tc, &s, &x, &y)) >= 0) {
            if (r == 1) { pressed = 0; continue; }
            if (pressed) continue;
            pressed = 1;
            touch_t = now_ms();
            note[0] = 0;
            int hit = -1;
            for (int i = 0; i < nb; i++)
                if (x >= b[i].x && x < b[i].x + b[i].w &&
                    y >= b[i].y && y < b[i].y + b[i].h) hit = i;
            if (hit < 0 || (armed >= 0 && hit != armed)) {
                if (armed >= 0) { armed = -1; next = 0; }
                continue;
            }
            if (b[hit].danger && armed != hit) {
                armed = hit;
                armed_t = now_ms();
                buzz_cue(AD4_MARK);
                next = 0;
                continue;
            }
            buzz_cue(AD4_TAP);
            armed = -1;
            next = 0;
            const char *tok = b[hit].token;
            char path[640], en[400];
            if (!strcmp(tok, "ufile")) {
                char dst[256], rb[2048];
                kcopy cc = { &s, &fbs, len, ku[b[hit].arg].name };
                int rc = k_upload(host, port, ku[b[hit].arg].name, dst, sizeof dst, k_copyprog, &cc);
                fprintf(stderr, "klipper: usb %s -> %d %s\n", ku[b[hit].arg].name, rc, dst);
                k_usbumount();
                if ((rc != 200 && rc != 201) || !dst[0]) {
                    snprintf(note, sizeof note, "copy failed");
                    note_t = now_ms();
                    m.nusb = k_usb();
                    continue;
                }
                urlenc(dst, en, sizeof en);
                snprintf(path, sizeof path, "/printer/print/start?filename=%s", en);
                rc = http_req(host, port, "POST", path, rb, sizeof rb);
                fprintf(stderr, "klipper: %s -> %d\n", path, rc);
                snprintf(note, sizeof note, "PRINT %s", rc == 200 ? "ok" : "failed");
                note_t = now_ms();
                m.scr = rc == 200 ? K_HOME : K_FILES;
                if (m.scr == K_FILES) { m.page = 0; m.nfiles = k_files(host, port); k_rows(host, port, &m); }
                continue;
            }
            if (!strcmp(tok, "print") || !strcmp(tok, "again") || !strcmp(tok, "hjob")) {
                urlenc(tok[0] == 'p' ? kf[m.sel].name : tok[0] != 'h' ? k.file
                       : m.hsrc ? m.last.name : kh[b[hit].arg].name, en, sizeof en);
                snprintf(path, sizeof path, "/printer/print/start?filename=%s", en);
                tok = path;
            } else if (tok[0] != '/') {
                k_nav(host, port, &m, &b[hit]);
                continue;
            }
            char rb[2048], lab[24];
            int rc = http_req(host, port, "POST", tok, rb, sizeof rb);
            snprintf(lab, sizeof lab, "%s", b[hit].label);
            for (char *q = lab; *q; q++) if (*q == '\n') *q = ' ';
            fprintf(stderr, "klipper: %s -> %d\n", tok, rc);
            snprintf(note, sizeof note, "%s %s", lab,
                     rc == 200 ? "ok" : rc == -2 ? "running" : "failed");
            note_t = now_ms();
            if (rc == 200 && m.scr == K_BRACE) m.scr = K_MODE;
            if (rc == 200 && (!strcmp(b[hit].token, "print") || !strcmp(b[hit].token, "again") ||
                              !strcmp(b[hit].token, "hjob"))) m.scr = K_HOME;
        }
    }
}
#endif /* __linux__ */

/* sh(1) cannot put a real newline inside a word, so the caller writes "\n". */
static void unesc(char *d, const char *s, size_t n) {
    size_t j = 0;
    for (size_t i = 0; s[i] && j + 1 < n; i++) {
        if (s[i] == '\\' && s[i + 1] == 'n') { d[j++] = '\n'; i++; continue; }
        d[j++] = s[i];
    }
    d[j] = 0;
}

static void usage(void) {
    fprintf(stderr,
        "usage: ad4-screen splash|done|fail TITLE [SUB]\n"
        "       ad4-screen status TITLE [SUB] [PCT]\n"
        "       ad4-screen menu [--auto TOKEN SECS] [--dump FILE.ppm]\n"
        "       ad4-screen confirm TITLE BODY [GO_LABEL] [SECS]\n"
        "       ad4-screen touch [SECS]\n"
        "       ad4-screen calib [SECS]\n"
        "       ad4-screen klipper [HOST[:PORT]] [--dump FILE.ppm [files|file|hist|hjob|usb|heat|\n"
        "                tools|about|fans|move|tune|objs|mode|pad|brace]]\n"
        "env: AD4_MCU AD4_IP AD4_PORT AD4_INSTALLED AD4_STICK AD4_ROT AD4_CAL\n");
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : NULL;
    const char *dump = NULL, *autotok = NULL;
    const char *pos[6];
    int autosec = 0, npos = 0;
    /* flags may appear anywhere; everything else is positional, in order */
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--dump") && i + 1 < argc) dump = argv[++i];
        else if (!strcmp(argv[i], "--auto") && i + 2 < argc) {
            autotok = argv[++i]; autosec = atoi(argv[++i]);
        } else if (npos < 6) pos[npos++] = argv[i];
    }
    if (!mode) { usage(); return 2; }
    if (!strcmp(mode, "menu")) return cmd_menu(dump, autotok, autosec);
    if (!strcmp(mode, "splash")) return cmd_splash(dump);
    if (!strcmp(mode, "boot")) return cmd_boot(dump);
    if (!strcmp(mode, "confirm")) {
        char title[200] = "", body[600] = "";
        if (npos > 0) unesc(title, pos[0], sizeof title);
        if (npos > 1) unesc(body, pos[1], sizeof body);
        return cmd_confirm(dump, title, body, npos > 2 ? pos[2] : "GO",
                           npos > 3 ? atoi(pos[3]) : 120);
    }
    if (!strcmp(mode, "touch")) return cmd_touch(dump, npos > 0 ? atoi(pos[0]) : 0);
    if (!strcmp(mode, "calib")) return cmd_calib(npos > 0 ? atoi(pos[0]) : 0);
#ifdef __linux__
    if (!strcmp(mode, "klipper")) return cmd_klipper(dump, npos > 0 ? pos[0] : NULL, npos > 1 ? pos[1] : NULL);
#endif
    if (!strcmp(mode, "status") || !strcmp(mode, "done") || !strcmp(mode, "fail")) {
        /* a trailing all-digits word is the percentage, never the title */
        int pct = -1;
        if (npos > 1) {
            const char *last = pos[npos - 1];
            char *e = 0;
            strtol(last, &e, 10);
            if (e != last && !*e) { pct = atoi(last); npos--; }
        }
        char title[200] = "", sub[400] = "";
        if (npos > 0) unesc(title, pos[0], sizeof title);
        if (npos > 1) unesc(sub, pos[1], sizeof sub);
        return cmd_simple(mode, dump, title, sub, pct);
    }
    usage();
    return 2;
}
