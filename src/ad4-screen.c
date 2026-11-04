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
    char ver[48];                     /* Klipper software_version */
    char homed[8];                    /* toolhead homed_axes */
    double et, ett, bt, btt, prog, dur, tot, fused, pfan, cfan, light, pos[3];
    double speed, flow, zoff;         /* gcode_move: M220, M221, SET_GCODE_OFFSET */
    int layer, nlayer;                /* SET_PRINT_STATS_INFO, 0 = not sent */
} kstat;

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
                 "/printer/objects/query?print_stats=state,filename,print_duration,total_duration,filament_used,info"
                 "&extruder=temperature,target&heater_bed=temperature,target"
                 "&display_status=progress&fan=speed&fan_generic%20chamber_fan=speed&output_pin%20light=value"
                 "&toolhead=position,homed_axes&gcode_move=speed_factor,extrude_factor,homing_origin",
                 buf, sizeof buf) != 200)
        return;
    if ((b = jobj(buf, "print_stats", &e))) {
        jstr(b, e, "state", k->pstate, sizeof k->pstate);
        jstr(b, e, "filename", k->file, sizeof k->file);
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
    if ((b = jobj(buf, "display_status", &e))) k->prog = jnum(b, e, "progress");
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

static unsigned k_chip(const kstat *k, const char **label) {
    if (!k->online) { *label = "HOST OFFLINE"; return C_ERR; }
    if (!strcmp(k->kstate, "startup")) { *label = "STARTING"; return C_WARN; }
    if (strcmp(k->kstate, "ready")) { *label = "NOT READY"; return C_ERR; }
    if (!strcmp(k->pstate, "printing")) { *label = "PRINTING"; return C_ACC; }
    if (!strcmp(k->pstate, "paused")) { *label = "PAUSED"; return C_WARN; }
    *label = "READY";
    return C_OK;
}

/* ---- local files: Moonraker's gcodes root, newest first */
#define K_MAXF 64
#define K_ROWS 6
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

typedef struct {
    int ok;
    double est, fil, grams, lh, ext, bed, layers, height;
    char type[24], slicer[32];
    char thumb[160];                  /* relative to the file's folder */
} kmeta;

static void k_meta(const char *host, int port, const char *name, kmeta *m) {
    static char buf[16384];
    char path[640], en[400];
    const char *b, *e;
    memset(m, 0, sizeof *m);
    urlenc(name, en, sizeof en);
    snprintf(path, sizeof path, "/server/files/metadata?filename=%s", en);
    if (http_req(host, port, "GET", path, buf, sizeof buf) != 200 ||
        !(b = jobj(buf, "result", &e)))
        return;
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

/* sz x sz, nearest-neighbour scaled, alpha over bg */
static void k_img(const surf *s, int x, int y, int sz, const kimg *im, unsigned bg) {
    for (int j = 0; j < sz; j++)
        for (int i = 0; i < sz; i++) {
            const unsigned char *q = im->px + 4 * ((size_t)(j * im->h / sz) * im->w + i * im->w / sz);
            px(s, x + i, y + j, mix(bg, (unsigned)q[0] << 16 | q[1] << 8 | q[2], q[3] + (q[3] >> 7)));
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
    if (l > 9) l = 9;
    snprintf(o, sz, "%.*s", l, n);
}

static void k_line(const surf *s, int x0, int y0, int x1, int y1, unsigned c) {
    int dx = abs(x1 - x0), dy = -abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    for (int err = dx + dy;;) {
        fill(s, x0, y0, 2, 2, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* the bed, 220 x 200 mm at 1.2 px/mm, back edge at the top */
#define K_BX 108
#define K_BY 108
#define K_BMX(x) (K_BX + (int)((x) * 1.2))
#define K_BMY(y) (K_BY + (int)((200 - (y)) * 1.2))

enum { K_HOME, K_FILES, K_FILE, K_HEAT, K_TOOLS, K_ABOUT, K_FIL, K_MOVE, K_TUNE, K_OBJS };
typedef struct {
    int scr, page, sel, nfiles, ft, step, nobj;
    kmeta meta; kimg img;             /* the file picked in PRINT */
    char jfile[96]; kmeta jmeta; kimg jimg; long jt;   /* the running job */
} kmenu;

/* FILAMENT's TEMP tile and MOVE's STEP tile cycle through these */
static const int k_ftemp[] = { 220, 240, 250, 210 };
static const double k_step[] = { 10, 1, 0.1 };

#define KB(x, w, lab, tok, top, bot, dng) \
    ((btn){ x, 600, w, 120, lab, tok, top, bot, 3, dng, 0 })
#define KT(x, y, w, lab, tok, top, bot) \
    ((btn){ x, y, w, 130, lab, tok, top, bot, 3, 0, 0 })
#define K_G1 0x5a6a80u
#define K_G2 0x39465au
#define K_GS "/printer/gcode/script?script="

/* TUNE: one row per setting, - and + either side of the value. Speed only
 * goes down: the 60 mm/s cap lives in the slicer, max_velocity is higher. */
#define K_NTUNE 5
static const char *const k_tlab[K_NTUNE] = { "SPEED", "FLOW", "Z OFFSET", "NOZZLE", "PART FAN" };

static double k_tval(const kstat *k, int i) {
    double v[K_NTUNE] = { k->speed * 100, k->flow * 100, k->zoff, k->ett, k->pfan * 100 };
    return v[i];
}

static int k_tune(const kstat *k, btn *b) {
    static const double step[K_NTUNE] = { 10, 2, 0.02, 5, 10 };
    static const double lo[K_NTUNE] = { 20, 80, -1, 180, 0 }, hi[K_NTUNE] = { 100, 120, 1, 260, 100 };
    static char cmd[K_NTUNE][2][96];
    int n = 0;
    for (int i = 0; i < K_NTUNE; i++)
        for (int d = 0; d < 2; d++) {
            double cur = k_tval(k, i), v = cur + (d ? step[i] : -step[i]);
            v = (long)(v / step[i] + (v < 0 ? -0.5 : 0.5)) * step[i];   /* no libm */
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
            b[n++] = (btn){ d ? 346 : 24, 108 + i * 96, 110, 88, d ? "+" : "-",
                            ok ? cmd[i][d] : "none", ok ? C_ACC : K_G2, ok ? C_ACCD : K_G2, 4, 0, 0 };
        }
    b[n++] = KB(24, 208, "BACK", "back", K_G1, K_G2, 0);
    b[n++] = KB(248, 208, "OBJECTS", "objs", C_ACC, C_ACCD, 0);
    return n;
}

/* OBJECTS: two columns of the job's objects; a tap (and a confirm) skips one */
static int k_objbtn(const kmenu *m, btn *b) {
    static char lab[K_MAXO][32], cmd[K_MAXO][240];
    int n = 0;
    for (int i = 0; i < m->nobj; i++) {
        char nm[16], en[160];
        k_oname(ko[i].name, nm, sizeof nm);
        if (ko[i].excl) snprintf(lab[i], sizeof lab[i], "%d skipped", i + 1);
        else snprintf(lab[i], sizeof lab[i], "%d %s", i + 1, nm);
        urlenc(ko[i].name, en, sizeof en);
        snprintf(cmd[i], sizeof cmd[i], K_GS "EXCLUDE_OBJECT%%20NAME%%3D%s", en);
        int on = !ko[i].excl && m->nobj > 1;   /* never the last one: that is CANCEL */
        b[n++] = (btn){ 24 + i % 2 * 224, 372 + i / 2 * 76, 208, 68, lab[i], on ? cmd[i] : "none",
                        ko[i].excl ? K_G2 : ko[i].cur ? C_ACC : K_G1,
                        ko[i].excl ? K_G2 : ko[i].cur ? C_ACCD : K_G2, 2, on, 0 };
    }
    b[n++] = KB(24, 432, "BACK", "back", K_G1, K_G2, 0);
    return n;
}

static int k_buttons(const kstat *k, const kmenu *m, btn *b) {
    static char pf[32], cf[32], lf[32];
    int n = 0;
    if (!k->online || !strcmp(k->kstate, "startup")) return 0;
    if (strcmp(k->kstate, "ready")) {
        b[0] = KB(24, 432, "FIRMWARE RESTART", "/printer/firmware_restart", C_ACC, C_ACCD, 0);
        return 1;
    }
    if (k_busy(k)) {
        if (m->scr == K_TUNE) return k_tune(k, b);
        if (m->scr == K_OBJS) return k_objbtn(m, b);
        if (!strcmp(k->pstate, "printing"))
            b[0] = KB(24, 136, "PAUSE", "/printer/print/pause", C_WARN, 0xa86f22u, 0);
        else
            b[0] = KB(24, 136, "RESUME", "/printer/print/resume", C_OK, 0x1f8a60u, 0);
        b[1] = KB(172, 136, "TUNE", "tune", C_ACC, C_ACCD, 0);
        b[2] = KB(320, 136, "CANCEL", "/printer/print/cancel", C_ERRD, 0x6d2530u, 1);
        for (int i = 0; i < 3; i++) b[i].sc = 2;   /* three across: CANCEL fits at 2 */
        return 3;
    }
    switch (m->scr) {
    case K_HOME:
        b[n++] = (btn){ 24, 290, 432, 130, "PRINT", "files", C_OK, 0x1f8a60u, 4, 0, 0 };
        b[n++] = KT(24, 436, 208, "PREHEAT", "heat", C_WARN, 0xa86f22u);
        b[n++] = KT(248, 436, 208, "FILAMENT", "fil", C_ACC, C_ACCD);
        b[n++] = KB(24, 208, "TOOLS", "tools", C_ACC, C_ACCD, 0);
        b[n++] = KB(248, 208, "COOLDOWN", K_GS "TURN_OFF_HEATERS", K_G1, K_G2, 0);
        break;
    case K_FILES:
        for (int i = 0; i < K_ROWS && m->page * K_ROWS + i < nkf; i++) {
            int f = m->page * K_ROWS + i;
            b[n++] = (btn){ 24, 108 + i * 80, 432, 72, kf[f].name, "file", 0, 0, 0, 0, f };
        }
        b[n++] = KB(24, 136, "BACK", "back", K_G1, K_G2, 0);
        if (m->page > 0) b[n++] = KB(172, 136, "PREV", "prev", C_ACC, C_ACCD, 0);
        if ((m->page + 1) * K_ROWS < nkf) b[n++] = KB(320, 136, "NEXT", "next", C_ACC, C_ACCD, 0);
        break;
    case K_FILE:
        b[n++] = KB(24, 208, "BACK", "back", K_G1, K_G2, 0);
        b[n++] = KB(248, 208, "PRINT", "print", C_OK, 0x1f8a60u, 1);
        break;
    case K_HEAT:
        b[n++] = KT(24, 290, 208, "PLA\n210/60", K_GS "M104%20S210%0AM140%20S60", C_WARN, 0xa86f22u);
        b[n++] = KT(248, 290, 208, "PETG\n240/70", K_GS "M104%20S240%0AM140%20S70", C_WARN, 0xa86f22u);
        b[n++] = KT(24, 436, 208, "ABS\n245/100", K_GS "M104%20S245%0AM140%20S100", C_WARN, 0xa86f22u);
        b[n++] = KT(248, 436, 208, "TPU\n225/50", K_GS "M104%20S225%0AM140%20S50", C_WARN, 0xa86f22u);
        b[n++] = KB(24, 208, "BACK", "back", K_G1, K_G2, 0);
        b[n++] = KB(248, 208, "COOLDOWN", K_GS "TURN_OFF_HEATERS", K_G1, K_G2, 0);
        break;
    case K_TOOLS:
        if (k->pfan > 0) snprintf(pf, sizeof pf, "PART FAN\n%.0f%%", k->pfan * 100);
        else snprintf(pf, sizeof pf, "PART FAN\noff");
        snprintf(cf, sizeof cf, "CHAMBER\nFAN %s", k->cfan > 0 ? "on" : "off");
        b[n++] = KT(24, 290, 208, pf, k->pfan > 0 ? K_GS "M107" : K_GS "M106%20S255",
                    k->pfan > 0 ? C_ACC : K_G1, k->pfan > 0 ? C_ACCD : K_G2);
        b[n++] = KT(248, 290, 208, cf, k->cfan > 0
                        ? K_GS "SET_FAN_SPEED%20FAN%3Dchamber_fan%20SPEED%3D0"
                        : K_GS "SET_FAN_SPEED%20FAN%3Dchamber_fan%20SPEED%3D1",
                    k->cfan > 0 ? C_ACC : K_G1, k->cfan > 0 ? C_ACCD : K_G2);
        b[n++] = KT(24, 436, 208, "MOVE", "move", C_ACC, C_ACCD);
        b[n++] = KT(248, 436, 208, "MOTORS\nOFF", K_GS "M84", K_G1, K_G2);
        if (k->light < 0) {
            b[n++] = KB(24, 208, "BACK", "back", K_G1, K_G2, 0);
            b[n++] = KB(248, 208, "ABOUT", "about", K_G1, K_G2, 0);
            break;
        }
        /* LIGHT cycles off -> on -> 30 % -> off */
        if (k->light <= 0) snprintf(lf, sizeof lf, "LIGHT\noff");
        else if (k->light >= 1) snprintf(lf, sizeof lf, "LIGHT\non");
        else snprintf(lf, sizeof lf, "LIGHT\n%.0f%%", k->light * 100);
        b[n++] = KB(24, 136, "BACK", "back", K_G1, K_G2, 0);
        b[n++] = (btn){ 172, 600, 136, 120, lf,
                        k->light <= 0 ? K_GS "SET_PIN%20PIN%3Dlight%20VALUE%3D1"
                        : k->light >= 1 ? K_GS "SET_PIN%20PIN%3Dlight%20VALUE%3D0.3"
                        : K_GS "SET_PIN%20PIN%3Dlight%20VALUE%3D0",
                        k->light > 0 ? C_WARN : K_G1, k->light > 0 ? 0xa86f22u : K_G2, 2, 0, 0 };
        b[n++] = KB(320, 136, "ABOUT", "about", K_G1, K_G2, 0);
        break;
    case K_ABOUT:
        b[n++] = KB(24, 208, "BACK", "back", K_G1, K_G2, 0);
        b[n++] = KB(248, 208, "FIRMWARE\nRESTART", "/printer/firmware_restart", C_ACC, C_ACCD, 1);
        break;
    case K_FIL: {
        /* LOAD_FILAMENT / UNLOAD_FILAMENT are macros in printer.cfg */
        static char ld[96], ul[96], pu[96], tl[16];
        int ft = k_ftemp[m->ft];
        snprintf(ld, sizeof ld, K_GS "LOAD_FILAMENT%%20TEMP%%3D%d", ft);
        snprintf(ul, sizeof ul, K_GS "UNLOAD_FILAMENT%%20TEMP%%3D%d", ft);
        snprintf(pu, sizeof pu, K_GS "LOAD_FILAMENT%%20TEMP%%3D%d%%20LENGTH%%3D20", ft);
        snprintf(tl, sizeof tl, "TEMP\n%d C", ft);
        b[n++] = KT(24, 290, 208, "LOAD", ld, C_OK, 0x1f8a60u);
        b[n++] = KT(248, 290, 208, "UNLOAD", ul, C_WARN, 0xa86f22u);
        b[n++] = KT(24, 436, 208, "PURGE\n20 mm", pu, C_ACC, C_ACCD);
        b[n++] = KT(248, 436, 208, tl, "ftemp", K_G1, K_G2);
        b[n++] = KB(24, 208, "BACK", "back", K_G1, K_G2, 0);
        b[n++] = KB(248, 208, "COOLDOWN", K_GS "TURN_OFF_HEATERS", K_G1, K_G2, 0);
        break;
    }
    case K_MOVE: {
        /* relative jogs inside a saved gcode state; XY 50 mm/s, Z 5 mm/s */
        static const char *const lab[6] = { "X-", "X+", "Y-", "Y+", "Z-", "Z+" };
        static const int gx[6] = { 0, 2, 1, 1, 3, 3 }, gy[6] = { 1, 1, 2, 0, 2, 0 };
        static char jog[6][160], st[24];
        double d = k_step[m->step];
        for (int i = 0; i < 6; i++) {
            snprintf(jog[i], sizeof jog[i], K_GS "SAVE_GCODE_STATE%%20NAME%%3Djog%%0AG91%%0A"
                     "G1%%20%c%s%g%%20F%d%%0ARESTORE_GCODE_STATE%%20NAME%%3Djog",
                     lab[i][0], lab[i][1] == '-' ? "-" : "", d, i < 4 ? 3000 : 300);
            b[n++] = (btn){ 24 + gx[i] * 110, 180 + gy[i] * 112, 102, 104, lab[i], jog[i],
                            C_CARD2, C_CARD, 3, 0, 0 };
        }
        snprintf(st, sizeof st, "STEP\n%g mm", d);
        b[n++] = (btn){ 134, 292, 102, 104, "HOME", K_GS "HOME_XYZ", C_ACC, C_ACCD, 3, 0, 0 };
        b[n++] = (btn){ 354, 292, 102, 104, st, "step", K_G1, K_G2, 2, 0, 0 };
        b[n++] = KB(24, 432, "BACK", "back", K_G1, K_G2, 0);
        break;
    }
    }
    return n;
}

static void k_temp(const surf *s, int x, int y, const char *label,
                   double cur, double tgt) {
    char v[16], t[32];
    rrect(s, x, y, 208, 150, 18, C_CARD, C_CARD2);
    text(s, x + 16, y + 14, label, 2, C_DIM);
    snprintf(v, sizeof v, "%.0f", cur);
    text(s, x + 16, y + 44, v, 6, C_TXT);
    text(s, x + 16 + tw(v, 6) + 6, y + 44, "C", 2, C_DIM);
    if (tgt > 0) snprintf(t, sizeof t, "target %.0f", tgt);
    else snprintf(t, sizeof t, "off");
    text(s, x + 16, y + 114, t, 2, tgt > 0 ? C_WARN : C_DIM);
}

static void k_size(double sz, char *o, int n) {
    if (sz >= 1e6) snprintf(o, n, "%.1f MB", sz / 1e6);
    else snprintf(o, n, "%.0f KB", sz / 1e3);
}

static void k_file_row(const surf *s, const btn *b) {
    char nm[32], sz[24];
    const kfile *f = &kf[b->arg];
    int l = (int)strlen(f->name);
    if (l > 6 && !strcmp(f->name + l - 6, ".gcode")) l -= 6;
    if (l > 25) snprintf(nm, sizeof nm, "%.23s..", f->name);
    else snprintf(nm, sizeof nm, "%.*s", l, f->name);
    k_size(f->size, sz, sizeof sz);
    rrect(s, b->x, b->y, b->w, b->h, 14, C_CARD, C_CARD2);
    text(s, b->x + 16, b->y + 14, nm, 2, C_TXT);
    text(s, b->x + 16, b->y + 44, sz, 2, C_DIM);
}

static void k_file_card(const surf *s, const kmenu *m) {
    const kfile *f = &kf[m->sel];
    const kmeta *t = &m->meta;
    char l[8][48], sz[24];
    int n = 0, y;
    rrect(s, 24, 108, 432, 470, 18, C_CARD, C_CARD2);
    if (m->img.px) {
        rrect(s, 308, 124, 128, 128, 12, C_BG1, C_BG1);
        k_img(s, 308, 124, 128, &m->img, C_BG1);
        y = text_wrap(s, 44, 128, f->name, 2, C_TXT, 252) + 16;
        if (y < 268) y = 268;
    } else
        y = text_wrap(s, 44, 128, f->name, 2, C_TXT, 392) + 16;
    /* slicers fill in different subsets; show what this one gave */
    int mn = (int)(t->est / 60 + 0.5);
    if (mn >= 60) snprintf(l[n++], 48, "time      %d h %02d min", mn / 60, mn % 60);
    else if (mn > 0) snprintf(l[n++], 48, "time      %d min", mn);
    if (t->fil > 0) snprintf(l[n++], 48, "filament  %.2f m  %.0f g", t->fil / 1000, t->grams);
    if (t->type[0]) snprintf(l[n++], 48, "material  %s", t->type);
    if (t->ext > 0) snprintf(l[n++], 48, "nozzle    %.0f C", t->ext);
    if (t->bed > 0) snprintf(l[n++], 48, "bed       %.0f C", t->bed);
    if (t->layers > 0) snprintf(l[n++], 48, "layers    %.0f x %.2f mm", t->layers, t->lh);
    if (t->height > 0) snprintf(l[n++], 48, "height    %.1f mm", t->height);
    k_size(f->size, sz, sizeof sz);
    snprintf(l[n++], 48, "size      %s", sz);
    for (int i = 0; i < n; i++, y += 32) text(s, 44, y, l[i], 2, C_DIM);
}

/* name, thumbnail, time left, finish clock, layer, progress, elapsed, fan, filament */
static void k_job(const surf *s, const kstat *k, const kmenu *m, unsigned col) {
    const kmeta *t = &m->jmeta;
    char nm[32], v[32], r[32];
    const char *f = strrchr(k->file, '/');
    f = f ? f + 1 : k->file;
    int l = (int)strlen(f);
    if (l > 6 && !strcmp(f + l - 6, ".gcode")) l -= 6;
    if (l > 24) snprintf(nm, sizeof nm, "%.22s..", f);
    else snprintf(nm, sizeof nm, "%.*s", l, f);
    text(s, 44, 298, nm, 2, C_TXT);

    rrect(s, 44, 326, 160, 160, 12, C_BG1, C_BG1);
    if (m->jimg.px) k_img(s, 44, 326, 160, &m->jimg, C_BG1);
    else { textc(s, 124, 390, "NO", 2, C_DIM); textc(s, 124, 412, "PREVIEW", 2, C_DIM); }

    /* klipper_estimator's time excludes PRINT_START, as print_duration does;
     * until the first extrusion the job is still heating and homing */
    int heat = k->fused <= 0 && !strcmp(k->pstate, "printing");
    double left = k_left(k, t);
    const char *lab[3] = { heat ? "HEATING, THEN" : "TIME LEFT", "DONE AT", "LAYER" };
    if (left < 0) snprintf(v, sizeof v, "--");
    else k_hm(left, v, sizeof v);
    time_t now = time(NULL) + g_wall;
    struct tm tm;
    /* g_wall comes from Moonraker's Date header; no header, no clock */
    if (left < 0 || now < 1700000000) snprintf(r, sizeof r, "--");
    else {
        now += (time_t)left;
        localtime_r(&now, &tm);
        strftime(r, sizeof r, left > 20 * 3600 ? "%a %H:%M" : "%H:%M", &tm);
        if (!tm.tm_gmtoff) strcat(r, " UTC");
    }
    char ly[32];
    if (k->nlayer > 0) snprintf(ly, sizeof ly, "%d / %d", k->layer, k->nlayer);
    else if (t->layers > 0) snprintf(ly, sizeof ly, "? / %.0f", t->layers);
    else snprintf(ly, sizeof ly, "--");
    const char *val[3] = { v, r, ly };
    for (int i = 0; i < 3; i++) {
        int y = 326 + i * 56;
        text(s, 224, y, lab[i], 2, i == 0 && heat ? C_WARN : C_DIM);
        text(s, 224, y + 20, val[i], tw(val[i], 3) <= 212 ? 3 : 2, C_TXT);
    }

    progress(s, 514, (int)(k->prog * 100 + 0.5), col);
    int sec = (int)k->tot;
    snprintf(v, sizeof v, "elapsed %d:%02d:%02d", sec / 3600, sec / 60 % 60, sec % 60);
    text(s, 24, 550, v, 2, C_DIM);
    snprintf(v, sizeof v, "fan %.0f%%", k->pfan * 100);
    text(s, 456 - tw(v, 2), 550, v, 2, C_DIM);
    if (t->fil > 0) snprintf(v, sizeof v, "filament %.2f / %.2f m", k->fused / 1000, t->fil / 1000);
    else snprintf(v, sizeof v, "filament %.2f m", k->fused / 1000);
    text(s, 24, 574, v, 2, C_DIM);
}

static void k_paint(const surf *s, const kstat *k, const kmenu *m, const btn *b,
                    int nb, int armed, const char *eth, const char *wifi,
                    const char *note, const char *hostport) {
    static const char *const title[] = {
        "ADVENTURER 4", "PRINT", "PRINT", "PREHEAT", "TOOLS", "ABOUT", "FILAMENT", "MOVE", "TUNE", "OBJECTS" };
    const char *lab, *foot = "";
    char pg[32];
    unsigned col = k_chip(k, &lab);
    background(s);
    header(s, title[m->scr], lab, col);
    switch (m->scr) {
    case K_HOME:
    case K_HEAT:
    case K_TOOLS:
    case K_FIL:
        k_temp(s, 24, 118, "NOZZLE", k->et, k->ett);
        k_temp(s, 248, 118, "BED", k->bt, k->btt);
        if (m->scr != K_HOME || k_idle(k)) break;
        rrect(s, 24, 282, 432, k_busy(k) && k->online && !strcmp(k->kstate, "ready") ? 220 : 306,
              18, C_CARD, C_CARD2);
        if (!k->online) {
            text_wrap(s, 44, 302, "Klipper host not answering. Waiting for it.",
                      2, C_TXT, 392);
        } else if (strcmp(k->kstate, "ready")) {
            text_wrap(s, 44, 302, k->kmsg[0] ? k->kmsg : k->kstate, 2, C_TXT, 392);
        } else
            k_job(s, k, m, col);
        break;
    case K_FILES:
        if (m->nfiles <= 0) {
            rrect(s, 24, 108, 432, 160, 18, C_CARD, C_CARD2);
            text_wrap(s, 44, 128, m->nfiles < 0 ? "Could not read the file list."
                      : "No files yet. Send one from Elegoo Slicer or Mainsail.",
                      2, C_DIM, 392);
        }
        if (nkf > K_ROWS) {
            snprintf(pg, sizeof pg, "page %d / %d", m->page + 1, (nkf + K_ROWS - 1) / K_ROWS);
            foot = pg;
        }
        break;
    case K_FILE:
        k_file_card(s, m);
        break;
    case K_MOVE: {
        char ps[64];
        snprintf(ps, sizeof ps, "X %.1f  Y %.1f  Z %.1f", k->pos[0], k->pos[1], k->pos[2]);
        text(s, 24, 112, ps, 2, C_TXT);
        if (strlen(k->homed) == 3) text(s, 24, 140, "homed", 2, C_OK);
        else text(s, 24, 140, "not homed: tap HOME first", 2, C_WARN);
        break;
    }
    case K_TUNE:
        /* a value off its default is drawn amber */
        for (int i = 0; i < K_NTUNE; i++) {
            static const double def[K_NTUNE] = { 100, 100, 0, -1, -1 };
            double v = k_tval(k, i);
            char t[24];
            int y = 108 + i * 96;
            if (i == 2) snprintf(t, sizeof t, "%+.2f mm", v);
            else if (i == 3) snprintf(t, sizeof t, v > 0 ? "%.0f C" : "off", v);
            else snprintf(t, sizeof t, "%.0f%%", v);
            rrect(s, 142, y, 196, 88, 14, C_CARD, C_CARD2);
            textc(s, 240, y + 12, k_tlab[i], 2, C_DIM);
            double dv = v - def[i];
            textc(s, 240, y + 40, t, 3, def[i] >= -0.5 && (dv > 0.001 || dv < -0.001) ? C_WARN : C_TXT);
        }
        if (!strcmp(k->pstate, "paused")) foot = "paused";
        break;
    case K_OBJS: {
        rrect(s, K_BX - 4, K_BY - 4, 272, 248, 10, C_BG1, C_BG1);
        if (m->nobj <= 0) {
            textc_wrap(s, 240, 180, m->nobj < 0 ? "Could not read the objects."
                       : "No object labels in this file. Turn on label objects in the slicer.",
                       2, C_DIM, 240);
            break;
        }
        for (int i = 0; i < m->nobj; i++) {
            const kobj *o = &ko[i];
            unsigned c = o->excl ? C_CARD2 : o->cur ? C_ACC : C_TXT;
            for (int j = 0; j < o->np; j++) {
                int a = (j + 1) % o->np;
                k_line(s, K_BMX(o->p[j][0]), K_BMY(o->p[j][1]), K_BMX(o->p[a][0]), K_BMY(o->p[a][1]), c);
            }
            char nb[4];
            snprintf(nb, sizeof nb, "%d", i + 1);
            textc(s, K_BMX(o->cx), K_BMY(o->cy) - 8, nb, 2, o->excl ? C_DIM : c);
        }
        break;
    }
    case K_ABOUT: {
        const char *kv[] = {
            "PRINTER ETH0", eth, "PRINTER WIFI", wifi, "MOONRAKER", hostport,
            "KLIPPER", k->ver[0] ? k->ver : "?",
            "SOURCE", "github.com/Mcsketch/ad4-klipper" };
        int y = 118;
        for (int i = 0; i < 10; i += 2) {
            text(s, 24, y, kv[i], 2, C_DIM);
            y = text_wrap(s, 40, y + 26, kv[i + 1], 2, C_TXT, 416) + 18;
        }
        break;
    }
    }

    for (int i = 0; i < nb; i++) {
        if (!strcmp(b[i].token, "file")) { k_file_row(s, &b[i]); continue; }
        btn t = b[i];
        if (i == armed) { t.top = C_ERR; t.bot = C_ERRD; }
        draw_btn(s, &t);
    }
    if (armed >= 0) textc(s, s->w / 2, 724, "tap again to confirm", 2, C_ERR);
    else if (note[0]) textc(s, s->w / 2, 724, note, 2, C_WARN);
    else if (foot[0]) textc(s, s->w / 2, 724, foot, 2, C_DIM);
    /* the printer's own addresses, so a new install can be found from the panel */
    char f[64];
    snprintf(f, sizeof f, "eth0 %s", eth);
    textc(s, s->w / 2, 754, f, 2, C_DIM);
    snprintf(f, sizeof f, "wifi %s", wifi);
    textc(s, s->w / 2, 778, f, 2, C_DIM);
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
static void k_nav(const char *host, int port, kmenu *m, const btn *b) {
    const char *t = b->token;
    if (!strcmp(t, "files")) { m->scr = K_FILES; m->page = 0; m->nfiles = k_files(host, port); }
    else if (!strcmp(t, "file")) {
        m->sel = b->arg;
        k_meta(host, port, kf[m->sel].name, &m->meta);
        k_thumb(host, port, kf[m->sel].name, &m->meta, &m->img);
        m->scr = K_FILE;
    }
    else if (!strcmp(t, "heat")) m->scr = K_HEAT;
    else if (!strcmp(t, "tools")) m->scr = K_TOOLS;
    else if (!strcmp(t, "about")) m->scr = K_ABOUT;
    else if (!strcmp(t, "fil")) m->scr = K_FIL;
    else if (!strcmp(t, "move")) m->scr = K_MOVE;
    else if (!strcmp(t, "tune")) m->scr = K_TUNE;
    else if (!strcmp(t, "objs")) { m->scr = K_OBJS; m->nobj = k_objs(host, port); }
    else if (!strcmp(t, "ftemp")) m->ft = (m->ft + 1) % 4;
    else if (!strcmp(t, "step")) m->step = (m->step + 1) % 3;
    else if (!strcmp(t, "prev")) m->page--;
    else if (!strcmp(t, "next")) m->page++;
    else if (!strcmp(t, "back"))
        m->scr = m->scr == K_FILE ? K_FILES
               : m->scr == K_ABOUT || m->scr == K_MOVE ? K_TOOLS
               : m->scr == K_OBJS ? K_TUNE : K_HOME;
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
        if (!strcmp(screen, "file") && nkf > 0) { nav.token = "file"; k_nav(host, port, &m, &nav); }
    }
    btn b[12];
    int nb = 0, armed = -1, pressed = 0;
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
            if (k_busy(&k) && (strcmp(m.jfile, k.file) || (!m.jmeta.ok && t - m.jt > 10000))) {
                snprintf(m.jfile, sizeof m.jfile, "%s", k.file);
                k_meta(host, port, k.file, &m.jmeta);
                k_thumb(host, port, k.file, &m.jmeta, &m.jimg);
                m.jt = t;
            }
            /* a job started elsewhere, a fault, or two idle minutes: back home;
             * TUNE and OBJECTS are the screens that live during a job */
            int stay = m.scr == K_TUNE || m.scr == K_OBJS
                     ? k_busy(&k) && k.online && !strcmp(k.kstate, "ready") : k_idle(&k);
            if (m.scr == K_OBJS && stay) m.nobj = k_objs(host, port);
            if (m.scr != K_HOME && (!stay || (!dump && t - touch_t > 120000))) {
                m.scr = K_HOME;
                armed = -1;
            }
            nb = k_buttons(&k, &m, b);
            if (armed >= nb) armed = -1;
            k_paint(&s, &k, &m, b, nb, armed, eth, wifi, note, hp);
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
            if (!strcmp(tok, "print")) {
                urlenc(kf[m.sel].name, en, sizeof en);
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
            if (rc == 200 && !strcmp(b[hit].token, "print")) m.scr = K_HOME;
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
        "       ad4-screen klipper [HOST[:PORT]] [--dump FILE.ppm [files|file|heat|tools|about|fil|move]]\n"
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
