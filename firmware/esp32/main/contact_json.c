#include "contact_json.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

int contact_append_number(char *out, size_t size, int used, const char *key, double value) {
    if (isnan(value)) return used;
    return used + snprintf(out + used, size > (size_t)used ? size - used : 0,
                           ",\"%s\":%.7f", key, value);
}

int contact_append_string(char *out, size_t size, int used, const char *key,
                          const char *value) {
    if (value == NULL || value[0] == '\0') return used;
    return used + snprintf(out + used, size > (size_t)used ? size - used : 0,
                           ",\"%s\":\"%s\"", key, value);
}

int contact_append_fields(char *out, size_t size, int used, const uplink_contact_t *c) {
    used = contact_append_string(out, size, used, "mac", c->mac);
    used = contact_append_string(out, size, used, "uas_id", c->uas_id);
    used = contact_append_string(out, size, used, "ua_type", c->ua_type);
    used = contact_append_number(out, size, used, "lat", c->lat);
    used = contact_append_number(out, size, used, "lon", c->lon);
    used = contact_append_number(out, size, used, "altitude_m", c->altitude_m);
    /* Right after altitude_m so the pair is never split: the number is meaningless to
     * the server without knowing what it is measured against. */
    used = contact_append_string(out, size, used, "altitude_ref", c->altitude_ref);
    used = contact_append_number(out, size, used, "height_m", c->height_m);
    used = contact_append_string(out, size, used, "height_ref", c->height_ref);
    used = contact_append_number(out, size, used, "speed_mps", c->speed_mps);
    return used;
}
