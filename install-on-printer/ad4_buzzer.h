/*
 * ad4_buzzer.h - the piezo speaker on the Adventurer 4's SoC.
 *
 * The piezo hangs off the vendor sunxi PWM driver, exposed as the char
 * device /dev/pwm. This kernel (3.4.39) exports no /sys/class/pwm pwmchipN,
 * so the ioctls below are the only path that actually makes sound. They were
 * decoded from the vendor `play` binary (buzzerPlay::play), whose sequence
 * this mirrors: open, init, then per note stop/config/start.
 *
 * That per-note stop/config/start is not a quirk of the vendor code, it is
 * what the driver requires: CONFIG is latched by START. Proven the hard way -
 * arming once and moving only the period between notes produced two ticks
 * (START and STOP) and silence in between, and re-programming mid-note does
 * nothing at all. So vibrato is out: this driver cannot change pitch while a
 * note sounds, and re-arming every 25 ms would be all attack and no tone.
 *
 * One deliberate departure from the vendor code: it treats freq==0 as "skip
 * the note AND its delay", so its tunes can never contain a rest. Here a rest
 * silences and then waits, which is what makes the break in the fail cue
 * audible.
 *
 * Frequency floor: the PWM clock divider cannot represent a period longer than
 * roughly 2.7 ms, so anything under ~370 Hz comes back as a click or as a
 * wrapped, wrong pitch. Every ioctl still returns success, so nothing reports
 * it - the note is just silently wrong. Keep the tables above 400 Hz;
 * tools/gen-riff.py transposes automatically to enforce that.
 *
 * Build for the SoC:  zig build-exe -target arm-linux-musleabi -mcpu=cortex_a7 \
 *                       -lc -static -OReleaseSmall ad4-buzz.c -femit-bin=ad4-buzz
 */
#ifndef AD4_BUZZER_H
#define AD4_BUZZER_H

/* hz 0 = rest. */
typedef struct { unsigned hz, ms; } ad4_note;

/* indochine-l_aventurier.midi: first 32 notes of the 'Boris Jardel' track,
 * eighth=185ms, legato, +1 oct. */
/* regenerate: python3 -I tools/gen-riff.py indochine-l_aventurier.midi */
static const ad4_note AD4_RIFF[] = {
    { 415, 370 },
    { 554, 185 },
    { 622, 185 },
    { 740, 370 },
    { 622, 370 },
    { 554, 185 },
    { 494, 370 },
    { 415, 925 },
    { 494, 370 },
    { 554, 185 },
    { 622, 185 },
    { 740, 370 },
    { 622, 370 },
    { 554, 185 },
    { 622, 185 },
    { 554, 185 },
    { 622, 925 },
    { 415, 370 },
    { 554, 185 },
    { 622, 185 },
    { 740, 370 },
    { 622, 370 },
    { 554, 185 },
    { 494, 370 },
    { 415, 925 },
    { 494, 370 },
    { 554, 185 },
    { 622, 185 },
    { 740, 370 },
    { 622, 370 },
    { 554, 185 },
    { 622, 184 },
};

static const ad4_note AD4_TAP[]   = { { 1500, 40 } };
static const ad4_note AD4_MARK[]  = { { 900, 70 } };
static const ad4_note AD4_START[] = { { 523, 110 }, { 784, 170 } };
static const ad4_note AD4_FAIL[]  = { { 784, 220 }, { 0, 120 }, { 523, 420 } };

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

static void buzz_wait(unsigned ms) {
    while (ms) {                      /* usleep is only defined under 1 s */
        unsigned chunk = ms > 500 ? 500 : ms;
        usleep(chunk * 1000u);
        ms -= chunk;
    }
}

static void buzz_freq(int fd, unsigned hz) {
    unsigned cfg[2];
    unsigned period = 1000000000u / hz;
    cfg[0] = (period + (period >> 31)) >> 1;   /* 50 % duty, vendor rounding */
    cfg[1] = period;
    ioctl(fd, PWM_IOC_CONFIG, cfg);
}

/* Plays and always leaves the piezo silent and the device closed: a PWM left
 * started would keep screaming after this process is gone. */
static int buzz_play(const ad4_note *notes, unsigned n) {
    int fd = open("/dev/pwm", O_RDWR | O_CLOEXEC);
    if (fd < 0) return -1;
    ioctl(fd, PWM_IOC_INIT, 0);   /* EBUSY is normal; the notes still go through */
    usleep(200000);
    for (unsigned i = 0; i < n; i++) {
        if (!notes[i].hz) {                   /* rest */
            ioctl(fd, PWM_IOC_STOP, 0);
            buzz_wait(notes[i].ms);
            continue;
        }
        /* Re-arm for every note: START is what latches the period CONFIG
         * asked for, so a period set while running is simply never heard. */
        ioctl(fd, PWM_IOC_STOP, 0);
        buzz_freq(fd, notes[i].hz);
        ioctl(fd, PWM_IOC_START, 0);
        buzz_wait(notes[i].ms);
    }
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
