/*
 * ad4-buzz - play one installer cue on the Adventurer 4's piezo.
 *
 *   ad4-buzz [tap|mark|start|ok|fail]     "ok" (the default) is the riff
 *   ad4-buzz tone HZ [MS]                one raw note, for hearing tests
 *
 * Sound is a bonus on this box, so a missing /dev/pwm exits 0 quietly and a
 * caller can invoke it unconditionally.
 */
#include <stdlib.h>
#include <string.h>
#include "ad4_buzzer.h"

int main(int argc, char **argv) {
    const char *cue = argc > 1 ? argv[1] : "ok";
    if (!strcmp(cue, "tone") && argc > 2) {
        ad4_note t;
        t.hz = (unsigned)atoi(argv[2]);
        t.ms = argc > 3 ? (unsigned)atoi(argv[3]) : 300;
        buzz_play(&t, 1);
        return 0;
    }
    if (!strcmp(cue, "tap"))        buzz_cue(AD4_TAP);
    else if (!strcmp(cue, "mark"))  buzz_cue(AD4_MARK);
    else if (!strcmp(cue, "start")) buzz_cue(AD4_START);
    else if (!strcmp(cue, "fail"))  buzz_cue(AD4_FAIL);
    else                            buzz_cue(AD4_RIFF);
    return 0;
}
