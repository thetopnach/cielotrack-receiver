#pragma once

#include <stddef.h>
#include <stdint.h>

/* When a contact was heard, as an ISO-8601 UTC stamp with milliseconds.
 *
 * Derived from how long ago it was decoded rather than from a wall-clock time recorded
 * at decode. Two reasons, and the second is why this is a function rather than a call
 * to time():
 *
 *   - SNTP can correct the clock between hearing something and uploading it. A stamp
 *     taken before the correction is wrong by the size of the correction; an age is
 *     not, because the monotonic clock is never corrected.
 *   - A batch is stamped once and sent once, but its contacts were heard at different
 *     moments. Stamping them all with the send time compressed a whole reporting window
 *     onto one instant — nine positions of one pass arrived bearing the same second —
 *     and the server, which orders a flight's path by time, drew them in whatever order
 *     they happened to be inserted. Multi-receiver flights zigzagged as a result.
 *
 * Milliseconds are included because whole seconds are not enough to order a BLE pass:
 * contacts arrive several to the second, and a path ordered on a stamp that cannot tell
 * them apart is ordered arbitrarily.
 *
 * `decoded_us` of 0 means "not recorded" and yields the current time, which is what
 * every contact got before they carried their own.
 */
void contact_stamp(int64_t now_wall_us, int64_t now_mono_us, int64_t decoded_us,
                   char *out, size_t size);
