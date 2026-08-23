/* A contact's stamp says when it was heard, not when it was sent.
 *
 * The bug this covers was visible on the map before it was visible in the code: a pass
 * heard by the ESP32 and by two Pis drew as a line that reversed direction four times.
 * The batch stamped every contact in it with one time_t taken at upload, so nine
 * positions spanning a whole reporting window all claimed the same second, and the
 * server — which orders a flight's path by time — interleaved them with the Pis'
 * correctly-stamped rows in whatever order they happened to be inserted.
 *
 * Build and run:  make -C firmware test
 */
#define _POSIX_C_SOURCE 200809L

#include "../esp32/main/contact_time.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

static void check(const char *name, int condition, const char *detail) {
    printf("  %s  %s%s%s\n", condition ? "PASS" : "FAIL", name,
           detail && detail[0] ? "  - " : "", detail ? detail : "");
    if (!condition) failures++;
}

/* 2026-08-20T19:07:31.000Z, the moment one of the real batches was uploaded. */
#define WALL_US 1787252851000000LL
#define MONO_US 600000000LL   /* ten minutes of uptime */

int main(void) {
    char now[40], four_ago[40], ten_ago[40];

    printf("contact_stamp:\n");

    contact_stamp(WALL_US, MONO_US, MONO_US, now, sizeof now);
    check("a contact decoded just now carries the current time",
          strcmp(now, "2026-08-20T19:07:31.000Z") == 0, now);

    contact_stamp(WALL_US, MONO_US, MONO_US - 4000000LL, four_ago, sizeof four_ago);
    check("a contact decoded four seconds ago is stamped four seconds back",
          strcmp(four_ago, "2026-08-20T19:07:27.000Z") == 0, four_ago);

    contact_stamp(WALL_US, MONO_US, MONO_US - 10500000LL, ten_ago, sizeof ten_ago);
    check("the age carries sub-second precision",
          strcmp(ten_ago, "2026-08-20T19:07:20.500Z") == 0, ten_ago);

    /* The property the map actually depends on: contacts heard at different moments
     * must sort into the order they were heard. Whole-second stamps could not do this,
     * because a BLE pass yields several contacts per second. */
    char a[40], b[40], c[40];
    contact_stamp(WALL_US, MONO_US, MONO_US - 1200000LL, a, sizeof a);
    contact_stamp(WALL_US, MONO_US, MONO_US -  900000LL, b, sizeof b);
    contact_stamp(WALL_US, MONO_US, MONO_US -  600000LL, c, sizeof c);
    check("three contacts inside one second still sort by when they were heard",
          strcmp(a, b) < 0 && strcmp(b, c) < 0, a);

    /* Every contact of a batch used to get the send time. That is now only what a
     * contact with no recorded decode time gets. */
    char unrecorded[40];
    contact_stamp(WALL_US, MONO_US, 0, unrecorded, sizeof unrecorded);
    check("a contact with no recorded decode time falls back to the send time",
          strcmp(unrecorded, "2026-08-20T19:07:31.000Z") == 0, unrecorded);

    /* SNTP sets the clock long after boot, so a contact heard before the correction has
     * a decode reading larger than any wall time. Refused rather than trusted into a
     * negative age and a stamp in the future. */
    char future[40];
    contact_stamp(WALL_US, MONO_US, MONO_US + 5000000LL, future, sizeof future);
    check("a decode reading from after the batch's own is not stamped into the future",
          strcmp(future, "2026-08-20T19:07:31.000Z") == 0, future);

    /* An age larger than the wall clock itself, which is what an unset clock looks like
     * at boot. Must not format a negative time. */
    char unset[40];
    contact_stamp(1000000LL, MONO_US, MONO_US - 500000000LL, unset, sizeof unset);
    check("an age beyond the clock itself clamps to the epoch rather than going negative",
          strncmp(unset, "1970-01-01T00:00:00", 19) == 0, unset);

    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
