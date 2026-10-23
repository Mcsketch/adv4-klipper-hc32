/*
 * ad4_buzzer.h - the piezo speaker on the Adventurer 4's SoC.
 *
 * The piezo hangs off the vendor sunxi PWM driver, exposed as the char
 * device /dev/pwm. This kernel (3.4.39) exports no /sys/class/pwm pwmchipN,
 * so the ioctls below are the only path that actually makes sound. They were
 * decoded from the vendor `play` binary (buzzerPlay::play), whose sequence
 * this mirrors: open, init, then per note stop/config/start.
 *
 * Two deliberate departures from the vendor code:
 *   - It treats freq==0 as "skip the note AND its delay", so its tunes can
 *     never contain a rest. Here a rest silences and then waits, which is
 *     what makes the riff's phrase break audible.
 *   - It plays each note as one continuous tone. A piezo driven by this
 *     driver has no continuous modulation, so the guitar vibrato in the riff
 *     is faked by re-issuing stop/config/start per slice along a sine.
 *
 * Build for the SoC:  zig build-exe -target arm-linux-musleabi -mcpu=cortex_a7 \
 *                       -lc -static -OReleaseSmall ad4-buzz.c -femit-bin=ad4-buzz
 */
#ifndef AD4_BUZZER_H
#define AD4_BUZZER_H

/* hz 0 = rest. vib 1 = this note is long enough to carry vibrato. */
typedef struct { unsigned hz, ms, vib; } ad4_note;

/* indochine-l_aventurier.midi: first 17 notes of the 'Boris Jardel' track,
 * eighth=120ms, notes capped at 700ms. */
/* regenerate: python3 -I tools/gen-riff.py indochine-l_aventurier.midi */
static const ad4_note AD4_RIFF[] = {
    { 208, 240, 0 },   /* G#3 */  { 277, 120, 0 },  /* C#4 */
    { 311, 120, 0 },   /* D#4 */  { 370, 700, 1 },  /* F#4, vibrato */
    { 0, 150, 0 },                /* rest */
    { 208, 240, 0 }, { 277, 120, 0 }, { 311, 120, 0 }, { 370, 700, 1 },
    { 0, 150, 0 },
    { 415, 700, 1 },               /* G#4, top note of the sustained chord */
    { 208, 240, 0 }, { 277, 120, 0 }, { 311, 120, 0 }, { 370, 240, 0 },
    { 311, 240, 0 }, { 277, 120, 0 }, { 247, 240, 0 },  /* B3 */
    { 208, 600, 1 },
};

static const ad4_note AD4_TAP[]   = { { 1500, 40, 0 } };
static const ad4_note AD4_MARK[]  = { { 900, 70, 0 } };
static const ad4_note AD4_START[] = { { 523, 110, 0 }, { 784, 170, 0 } };
static const ad4_note AD4_FAIL[]  = { { 392, 220, 0 }, { 0, 120, 0 }, { 262, 420, 0 } };

#define buzz_cue(t) buzz_play(t, sizeof(t) / sizeof((t)[0]))

#ifdef __linux__

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

/* This kernel's _IOC layout: (dir<<30)|(size<<16)|(type<<8)|nr, type 'p'. */
#define PWM_IOC_INIT   0x7002u      /* _IO ('p', 2) */
#define PWM_IOC_EXIT   0x7003u      /* _IO ('p', 3) */
#define PWM_IOC_START  0x7006u      /* _IO ('p', 6) */
#define PWM_IOC_STOP   0x7007u      /* _IO ('p', 7) */
#define PWM_IOC_CONFIG 0x40087009u  /* _IOW('p', 9, struct {u32 duty_ns, u32 period_ns}) */

/* Vibrato: 8 slices per cycle puts the wobble at 5 Hz, about the rate a
 * hand puts on a held guitar note. */
#define VIB_SLICE 25   /* ms between re-programs */
#define VIB_HEAD  140  /* ms played straight before the wobble starts */
#define VIB_RAMP  250  /* ms to reach full depth */
#define VIB_DEPTH 18   /* +/- 1.8 % of pitch, in permille */

static const short VIB_SIN[32] = {
       0,  195,  383,  556,  707,  831,  924,  981,
    1000,  981,  924,  831,  707,  556,  383,  195,
       0, -195, -383, -556, -707, -831, -924, -981,
   -1000, -981, -924, -831, -707, -556, -383, -195,
};

static void buzz_wait(unsigned ms) {
    while (ms) {                      /* usleep is only defined under 1 s */
        unsigned chunk = ms > 500 ? 500 : ms;
        usleep(chunk * 1000u);
        ms -= chunk;
    }
}

static void buzz_slice(int fd, unsigned hz, unsigned ms) {
    unsigned cfg[2];
    unsigned period = 1000000000u / hz;
    cfg[0] = (period + (period >> 31)) >> 1;   /* 50 % duty, vendor rounding */
    cfg[1] = period;
    ioctl(fd, PWM_IOC_STOP, 0);
    ioctl(fd, PWM_IOC_CONFIG, cfg);
    ioctl(fd, PWM_IOC_START, 0);
    buzz_wait(ms);
}

static void buzz_note(int fd, const ad4_note *n) {
    if (!n->hz) {
        ioctl(fd, PWM_IOC_STOP, 0);
        buzz_wait(n->ms);
        return;
    }
    if (!n->vib || n->ms <= VIB_HEAD + VIB_SLICE) {
        buzz_slice(fd, n->hz, n->ms);
        return;
    }
    buzz_slice(fd, n->hz, VIB_HEAD);
    unsigned t = VIB_HEAD;
    int k = 0;
    while (t < n->ms) {
        unsigned d = n->ms - t < VIB_SLICE ? n->ms - t : VIB_SLICE;
        int ramp = (int)(t - VIB_HEAD);
        if (ramp > VIB_RAMP) ramp = VIB_RAMP;
        unsigned f = (unsigned)((long)n->hz *
                       (1000 + ramp * VIB_DEPTH / VIB_RAMP * VIB_SIN[k] / 1000) / 1000);
        buzz_slice(fd, f, d);
        k = (k + 4) & 31;
        t += d;
    }
}

/* Plays and always leaves the piezo silent and the device closed: a PWM left
 * started would keep screaming after this process is gone. */
static int buzz_play(const ad4_note *notes, unsigned n) {
    int fd = open("/dev/pwm", O_RDWR | O_CLOEXEC);
    if (fd < 0) return -1;
    ioctl(fd, PWM_IOC_INIT, 0);   /* EBUSY is normal; the notes still go through */
    usleep(200000);
    for (unsigned i = 0; i < n; i++) buzz_note(fd, &notes[i]);
    ioctl(fd, PWM_IOC_STOP, 0);
    ioctl(fd, PWM_IOC_EXIT, 0);
    close(fd);
    return 0;
}

#else  /* host preview build: no piezo to drive */

static int buzz_play(const ad4_note *notes, unsigned n) {
    (void)notes; (void)n;
    return -1;
}

#endif /* __linux__ */

#endif /* AD4_BUZZER_H */
