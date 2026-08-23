/* A fault flag reports how a receiver is, not how it once was.
 *
 * The BLE board dropped nineteen contacts when a dense pass outran the uploader, and
 * then reported "detections are being heard but cannot be queued" for the rest of its
 * uptime. Sampled three and a half minutes at the time: the counter sat still at 19, the
 * queue was empty, and the radio was seeing a hundred advertisements a second. Nothing
 * was wrong, and the board said something was.
 *
 * Build and run:  make -C firmware test
 */
#include "../esp32/main/health.h"

#include <stdio.h>

static int failures = 0;

static void check(const char *name, int condition, const char *detail) {
    printf("  %s  %s%s%s\n", condition ? "PASS" : "FAIL", name,
           detail && detail[0] ? "  - " : "", detail ? detail : "");
    if (!condition) failures++;
}

int main(void) {
    printf("health_counter_moved:\n");

    /* The incident, as a sequence of check-ins. The counter climbs once and then holds,
     * which is exactly what a transient overflow looks like from the outside. */
    uint32_t mark = 0;
    const uint32_t dropped[] = {0, 0, 19, 19, 19, 19};
    int flagged[6];
    for (int i = 0; i < 6; i++) flagged[i] = health_counter_moved(&mark, dropped[i]);

    check("a quiet receiver reports nothing", !flagged[0] && !flagged[1], "");
    check("the check-in where contacts were lost reports it", flagged[2], "");
    check("and the next one does not, because nothing more was lost",
          !flagged[3] && !flagged[4] && !flagged[5], "");

    /* The bug, stated as the thing that must not happen again. */
    int still_flagging_later = flagged[5];
    check("a fault does not latch on for the rest of the receiver's uptime",
          !still_flagging_later, "");

    /* Losing more later is a new fault and must be reported again — clearing the flag
     * must not mean suppressing it. */
    check("a second burst of losses is reported", health_counter_moved(&mark, 25), "");
    check("and clears again once it stops", !health_counter_moved(&mark, 25), "");

    /* What the same helper does for the BLE stream check, which has always worked this
     * way: a radio still seeing advertisements has moved, a dead one has not. */
    uint32_t seen = 0;
    health_counter_moved(&seen, 1000);
    check("a counter that keeps climbing keeps reading as alive",
          health_counter_moved(&seen, 1400), "");
    check("and one that stops does not", !health_counter_moved(&seen, 1400), "");

    /* Unsigned counters are free to wrap. A wrapped counter has still moved, and going
     * quiet at that moment would hide a receiver that had been up long enough to matter. */
    uint32_t wrapped = 0xFFFFFFF0u;
    check("a counter that wraps past its maximum still counts as moving",
          health_counter_moved(&wrapped, 5), "");

    check("a null mark is refused rather than dereferenced",
          !health_counter_moved(NULL, 7), "");

    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
