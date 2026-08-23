/* Wi-Fi Remote ID capture, for a board dedicated to it.
 *
 * One radio, one channel at a time. Remote ID broadcasts here on channel 6 — measured,
 * not assumed: 65 Wing decodes on this street, every one of them on 6 — and the house
 * access points are on channel 4. A board cannot sniff one and hold an association on
 * the other, so it alternates: listen on 6, and every so often hop to the access point's
 * channel, send what it has collected in a batch, and come back.
 *
 * The cost is a second or two of blind time per cycle. That is the price of a receiver
 * that does not need a cable, and it is small enough to measure rather than argue about
 * — the count of frames seen keeps running across the gap, so the blind window shows up
 * in the numbers rather than hiding in them.
 *
 * Deliberately mutually exclusive with BLE capture. Two radios' work on one radio is
 * what the second board exists to avoid.
 */
#ifndef WIFI_CAPTURE_H
#define WIFI_CAPTURE_H

#include <stdint.h>

/* Starts promiscuous capture and the upload cycle on their own task. Returns at once.
 * Expects the station to be held (see uplink_hold_station) — this owns the channel. */
void wifi_capture_start(void);

uint32_t wifi_capture_frames(void);      /* 802.11 frames the radio handed over */
uint32_t wifi_capture_decoded(void);     /* those that carried Remote ID */
uint32_t wifi_capture_queued(void);      /* detections waiting for the next upload */
uint32_t wifi_capture_dropped(void);     /* detections the queue had no room for */

#endif /* WIFI_CAPTURE_H */
