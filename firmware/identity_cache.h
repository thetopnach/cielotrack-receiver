/* Remembering which aircraft a transmitter belongs to, so a position can be named.
 *
 * BLE Remote ID rotates one message per advertisement: a Basic ID saying who, then a
 * Location saying where, then another. Reported as they arrive, every position row
 * carries coordinates and no identity, and every identity row carries no position. On
 * the dashboard that is an aircraft called "Unknown Aircraft" at a precise location,
 * with its serial sitting in a different row that never reaches the map. Measured on the
 * bench board over 24 hours: 19 positions, 0 of them identified, against 13 of 14 on the
 * Pi beside it, which has done this for months.
 *
 * The semantics here deliberately match the Pi's rather than being reinvented, because
 * the whole value of a second receiver is that its rows are comparable:
 *
 *   - Conflict-aware, not latest-wins. A MAC that has reported two different UAS IDs is
 *     randomised, reassigned, or spoofed, and is quarantined rather than used to label
 *     anything. Overwriting instead produces confidently mislabelled aircraft.
 *   - Two horizons. One sighting is weak evidence and expires in minutes; an association
 *     seen repeatedly with no conflict is trusted far longer. That names the aircraft
 *     whose pass only yielded a Location, without letting one stray decode mislabel a
 *     different airframe for a week.
 *
 * A caller that uses a recalled identity must report it as inferred, never as decoded.
 * Claiming to have heard a serial that was not in this frame is the kind of quiet
 * fiction that makes a dataset untrustworthy.
 */
#ifndef IDENTITY_CACHE_H
#define IDENTITY_CACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Matches radio_tracker.py: IDENTITY_WEAK_TTL_SECONDS, IDENTITY_STRONG_TTL_SECONDS,
 * IDENTITY_STRONG_SIGHTINGS. The strong horizon is a day rather than the Pi's week
 * because this cache is in RAM and does not survive a reboot anyway. */
#define IDENTITY_WEAK_TTL_SECONDS    (15 * 60)
#define IDENTITY_STRONG_TTL_SECONDS  (24 * 3600)
#define IDENTITY_STRONG_SIGHTINGS    3
#define IDENTITY_CACHE_SIZE          24

/* Records an identity that was actually decoded from this transmitter just now.
 * Quarantines the entry instead if this MAC has previously reported a different id. */
void identity_remember(const char *mac, const char *uas_id, const char *ua_type,
                       uint32_t now_seconds);

/* Fills uas_id/ua_type from an earlier decode of this MAC and returns true, or leaves
 * them untouched and returns false. Quarantined and expired entries never match. */
bool identity_recall(const char *mac, char *uas_id, size_t uas_id_size,
                     char *ua_type, size_t ua_type_size, uint32_t now_seconds);

/* For tests and for counters worth reporting. */
void identity_cache_reset(void);
int identity_cache_quarantined(void);

#endif /* IDENTITY_CACHE_H */
