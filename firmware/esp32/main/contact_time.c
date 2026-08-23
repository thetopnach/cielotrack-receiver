/* gmtime_r is POSIX, not C11, and the host test build compiles with -std=c11 — where
 * it is not declared unless asked for. ESP-IDF's own build is gnu11 and never noticed. */
#define _POSIX_C_SOURCE 200809L

#include "contact_time.h"

#include <stdio.h>
#include <time.h>

void contact_stamp(int64_t now_wall_us, int64_t now_mono_us, int64_t decoded_us,
                   char *out, size_t size) {
    if (out == NULL || size == 0) return;

    /* A contact from the future is a contact whose monotonic reading was taken after
     * this one — impossible within a batch, but cheap to refuse rather than to trust
     * into a negative age and a stamp ahead of the clock. */
    int64_t age_us = 0;
    if (decoded_us > 0 && now_mono_us > decoded_us) {
        age_us = now_mono_us - decoded_us;
    }

    int64_t when_us = now_wall_us - age_us;
    /* Before the epoch means the clock is not set yet. Nothing honest to say, so say
     * the epoch rather than a negative time that formats as garbage. */
    if (when_us < 0) when_us = 0;

    time_t seconds = (time_t)(when_us / 1000000);
    int millis = (int)((when_us % 1000000) / 1000);

    struct tm utc;
    gmtime_r(&seconds, &utc);
    char base[24];
    strftime(base, sizeof base, "%Y-%m-%dT%H:%M:%S", &utc);
    snprintf(out, size, "%s.%03dZ", base, millis);
}
