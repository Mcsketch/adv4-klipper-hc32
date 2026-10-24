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
 *   status TITLE [SUB] [PCT]   progress screen
 *   done   TITLE [SUB]         success screen
 *   fail   TITLE [SUB]         error screen
 *   touch  [SECS]              diagnostic: live crosshair + tap-accuracy targets
 *   calib  [SECS]              four-mark touch calibration, writes AD4_CAL/CAL_PATH
 *
 * menu exit codes: 0 = a choice was made, 1 = no touchscreen (caller falls
 * back to the flag files), 2 = the user chose skip/abort.
 *
 * Accepted touches beep on the piezo (ad4_buzzer.h). The completion riff is
 * NOT played here: flashforge_init.sh runs `ad4-buzz ok` for that, so it
 * sounds even on a printer with no panel.
 *
 * The pixel layout is read from FBIOGET_VSCREENINFO rather than assumed, so
 * the same binary works on RGB565/BGR565/RGB888. The touch axis range comes
 * from EVIOCGABS, likewise not guessed.
 *
 * Build for the SoC:  zig build-exe -target arm-linux-musleabi -mcpu=cortex_a7 \
 *                       -lc -static -OReleaseSmall ad4-screen.c -femit-bin=ad4-screen
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

/* Two-line label: the button labels use '\n'. */
static void textc_wrap(const surf *s, int cx, int y, const char *t, int sc,
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
#ifdef __linux__
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
/* 0 = a position in *x/*y, 1 = the pen lifted, -1 = nothing left to read. */
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
    if (fx < 0) fx = 0; if (fx >= s->fw) fx = s->fw - 1;
    if (fy < 0) fy = 0; if (fy >= s->fh) fy = s->fh - 1;
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
                 unsigned top, bot; int sc; int danger; } btn;

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
    if (sub && *sub) textc_wrap(&s, s.w / 2, 240, sub, 2, C_DIM, 26);

    if (!strcmp(kind, "status")) {
        progress(&s, 360, pct < 0 ? 0 : pct, acc);
        /* only when the caller had nothing else to say */
        if (!sub || !*sub) textc(&s, s.w / 2, 410, "do not power off the printer", 2, C_WARN);
    } else if (!strcmp(kind, "done")) {
        /* two strokes meeting at a vertex, otherwise it reads as a "V" */
        int cx = s.w / 2 - 10, cy = 360, t = 11;
        for (int i = 0; i < 20; i++) fill(&s, cx - 40 + i, cy + i, t, t, C_OK);
        for (int i = 0; i < 46; i++) fill(&s, cx - 21 + i, cy + 20 - i, t, t, C_OK);
    } else if (!strcmp(kind, "fail")) {
        int cx = s.w / 2, cy = 380, t = 11;
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
        "       ad4-screen touch [SECS]\n"
        "       ad4-screen calib [SECS]\n"
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
    if (!strcmp(mode, "touch")) return cmd_touch(dump, npos > 0 ? atoi(pos[0]) : 0);
    if (!strcmp(mode, "calib")) return cmd_calib(npos > 0 ? atoi(pos[0]) : 0);
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
