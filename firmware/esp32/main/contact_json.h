/* The JSON shape of a decoded contact, in one place.
 *
 * A contact is serialised twice on the way to the server: the master POSTs its own
 * catches, and the sensor board relays its catches to the master over the peer link for
 * the master to forward. Those were two hand-written field lists, which is exactly the
 * arrangement where a field gets added to one and forgotten in the other — and one was:
 * altitude_ref and the broadcast height were computed by the decoder, printed to the
 * log, and then dropped on the floor because neither writer carried them. Every
 * height-above-takeoff catch therefore reached the server as a bare altitude with no
 * reference, was read as absolute, and rendered at zero once the ground was subtracted.
 *
 * The aircraft-descriptor fields now live in one function both writers call, so the next
 * field is added once and cannot diverge.
 */
#ifndef CONTACT_JSON_H
#define CONTACT_JSON_H

#include <stddef.h>

#include "uplink.h"

/* Append `,"key":value` at `used`, unless the value is absent — NAN for a number, NULL
 * or "" for a string. A field left out says "we do not know"; a field sent as null says
 * "we know it is nothing", and the two are different claims about an aircraft. Returns
 * the new length, which may exceed `size`; the caller checks, as the writers always did. */
int contact_append_number(char *out, size_t size, int used, const char *key, double value);
int contact_append_string(char *out, size_t size, int used, const char *key,
                          const char *value);

/* Appends the aircraft-descriptor fields shared by every serialisation of a contact, in
 * order: mac, uas_id, ua_type, lat, lon, altitude_m, altitude_ref, height_m, height_ref,
 * speed_mps. Each is emitted only when present, so an object built from these alone is
 * valid JSON once wrapped. The caller supplies the surrounding braces and whatever
 * framing is its own (a timestamp, an rssi, an identity source), because that part
 * genuinely differs between the master's uplink and the sensor's relay. */
int contact_append_fields(char *out, size_t size, int used, const uplink_contact_t *c);

#endif /* CONTACT_JSON_H */
