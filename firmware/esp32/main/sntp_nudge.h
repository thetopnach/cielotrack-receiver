#ifndef CIELOTRACK_SNTP_NUDGE_H
#define CIELOTRACK_SNTP_NUDGE_H

#include <stdint.h>

/* The re-nudge schedule after the network comes up.
 *
 * SNTP's own retry interval is an hour (CONFIG_LWIP_SNTP_UPDATE_DELAY): right for
 * correcting drift, wrong for a first sync that missed because DNS or the radio was not
 * settled the instant the station got its address. uplink_ready() needs a clock, so a
 * board whose boot attempt missed holds a working connection every cycle and still
 * refuses to send — up to an hour until SNTP tries again on its own.
 *
 * So after network-up the firmware asks for the time immediately and then a few more
 * times on this backoff, which is deliberately bounded: a slow-to-resolve network sets
 * its clock inside a minute or two, and then it stops. Once a sync has actually
 * succeeded the hourly interval is correct again and nothing here runs.
 *
 * Header-only and free of any ESP-IDF dependency on purpose: the schedule is a pure
 * decision and is proved on the host (see test/test_sntp_nudge.c), while the timer glue
 * that drives it stays in uplink.c where the radio does.
 */

/* How many re-nudges follow the immediate one. */
#define SNTP_NUDGE_MAX_ATTEMPTS 5

/* Milliseconds to wait before re-nudge number `attempt` (1-based), or 0 to stop.
 *
 * Doubling from 2s: 2, 4, 8, 16, 32 — five tries landing at roughly 2, 6, 14, 30 and
 * 62 seconds after network-up, so the whole sequence is done inside about a minute and
 * never hammers the time server. Any attempt past the last returns 0, which the caller
 * reads as "give up until the next network-up". */
static inline uint32_t sntp_nudge_backoff_ms(int attempt) {
    if (attempt < 1 || attempt > SNTP_NUDGE_MAX_ATTEMPTS) return 0;
    return (uint32_t)2000u << (attempt - 1);
}

#endif /* CIELOTRACK_SNTP_NUDGE_H */
