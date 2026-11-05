/* The re-nudge schedule after network-up must be prompt, bounded, and self-terminating.
 *
 * A freshly provisioned receiver whose boot-time SNTP attempt missed holds a working
 * connection but refuses to send anything — uplink_ready() needs a clock — until SNTP's
 * own hour-long retry comes round. The firmware asks again the moment there is a network
 * and then a few more times on this backoff; the schedule here is what makes "a few more
 * times" prompt enough to beat a slow DNS and bounded enough never to hammer the time
 * server or to run forever.
 *
 * Build and run:  make -C firmware test
 */
#include "../esp32/main/sntp_nudge.h"

#include <stdio.h>

static int failures = 0;

static void check(const char *name, int condition, const char *detail) {
    printf("  %s  %s%s%s\n", condition ? "PASS" : "FAIL", name,
           detail && detail[0] ? "  - " : "", detail ? detail : "");
    if (!condition) failures++;
}

int main(void) {
    printf("sntp_nudge_backoff_ms:\n");

    /* Attempt numbers are 1-based; anything outside the sequence means "stop". */
    check("attempt 0 stops", sntp_nudge_backoff_ms(0) == 0, "");
    check("a negative attempt stops", sntp_nudge_backoff_ms(-1) == 0, "");

    /* Every real attempt asks for a positive, strictly increasing wait, so each try
     * gives the network a little longer to resolve than the one before. */
    uint32_t total = 0;
    uint32_t prev = 0;
    int increasing = 1;
    int all_positive = 1;
    for (int a = 1; a <= SNTP_NUDGE_MAX_ATTEMPTS; a++) {
        uint32_t d = sntp_nudge_backoff_ms(a);
        if (d == 0) all_positive = 0;
        if (a > 1 && d <= prev) increasing = 0;
        prev = d;
        total += d;
    }
    check("every attempt in range waits a positive time", all_positive, "");
    check("the waits strictly increase", increasing, "");

    /* The first nudge must be soon — a slow DNS is a second or two, not a minute — and
     * no single wait may balloon into SNTP's own hour. */
    check("the first re-nudge comes within a couple of seconds",
          sntp_nudge_backoff_ms(1) <= 2000, "");
    check("no single wait exceeds a minute", prev <= 60000, "");

    /* Bounded: the whole sequence lands inside roughly one to two minutes, long enough
     * for a stubborn resolver and short enough that a board is never silent for long. */
    check("the whole sequence fits in about one to two minutes",
          total >= 30000 && total <= 120000, "");

    /* Self-terminating: the attempt past the last, and well past it, both stop. */
    check("the attempt after the last stops",
          sntp_nudge_backoff_ms(SNTP_NUDGE_MAX_ATTEMPTS + 1) == 0, "");
    check("a wildly out-of-range attempt stops", sntp_nudge_backoff_ms(1000) == 0, "");

    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
