/* The one field list every contact is serialised through.
 *
 * This is the regression that shipped: the decoder read a drone's height above takeoff
 * correctly, but the writer that put a contact on the wire carried only altitude_m and
 * neither the reference nor the broadcast height. The server, given a bare number with
 * no reference, read it as absolute WGS84, subtracted the terrain, and every low pass
 * over ~200 m of Texas rendered at zero feet. So the thing worth proving on the host is
 * not that a number serialises — it is that altitude_m never travels without saying what
 * it is measured against, and that "absolute" reaches the server as the word it branches
 * on rather than the "abs" the console log uses.
 */
#include "../esp32/main/contact_json.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int passed, failed;

static void check(const char *name, bool ok, const char *detail) {
    printf("  %s  %s", ok ? "PASS" : "FAIL", name);
    if (!ok && detail) printf("  - %s", detail);
    printf("\n");
    if (ok) passed++; else failed++;
}

static const char *build(const uplink_contact_t *c, char *out, size_t size) {
    int used = snprintf(out, size, "{\"detected_at\":\"t\"");
    used = contact_append_fields(out, size, used, c);
    snprintf(out + used, size > (size_t)used ? size - used : 0, "}");
    return out;
}

int main(void) {
    printf("contact json fields\n");
    char json[512];

    /* A height-above-takeoff catch — the MK30 case. altitude_m holds the AGL value and
     * the reference must ride with it, and the broadcast height is its own field. */
    uplink_contact_t agl = { .lat = 32.99, .lon = -96.67, .altitude_m = 106.5,
                             .height_m = 106.5, .speed_mps = 12.0 };
    snprintf(agl.altitude_ref, sizeof agl.altitude_ref, "agl");
    snprintf(agl.height_ref, sizeof agl.height_ref, "takeoff");
    build(&agl, json, sizeof json);
    check("agl contact carries altitude_ref \"agl\"",
          strstr(json, "\"altitude_ref\":\"agl\"") != NULL, json);
    check("agl contact carries altitude_m", strstr(json, "\"altitude_m\":106.5") != NULL, json);
    check("agl contact carries the broadcast height",
          strstr(json, "\"height_m\":106.5") != NULL, json);
    check("agl contact carries height_ref \"takeoff\"",
          strstr(json, "\"height_ref\":\"takeoff\"") != NULL, json);

    /* An absolute-altitude catch. The reference must be the server's exact word. */
    uplink_contact_t abs_alt = { .lat = 32.99, .lon = -96.67, .altitude_m = 250.0,
                                 .height_m = NAN, .speed_mps = 5.0 };
    snprintf(abs_alt.altitude_ref, sizeof abs_alt.altitude_ref, "absolute");
    build(&abs_alt, json, sizeof json);
    check("absolute contact carries altitude_ref \"absolute\"",
          strstr(json, "\"altitude_ref\":\"absolute\"") != NULL, json);
    check("absolute is spelled in full, never \"abs\"",
          strstr(json, "\"abs\"") == NULL, json);
    check("a contact with no broadcast height omits height_m entirely",
          strstr(json, "height_m") == NULL, json);
    check("a contact with no broadcast height omits height_ref entirely",
          strstr(json, "height_ref") == NULL, json);

    /* A Basic ID frame — identity, no position, no altitude. Absent fields are omitted,
     * not sent as nulls: "we do not know" is a different claim from "it is nothing". */
    uplink_contact_t bare = { .lat = NAN, .lon = NAN, .altitude_m = NAN, .height_m = NAN,
                              .speed_mps = NAN };
    snprintf(bare.uas_id, sizeof bare.uas_id, "1786501045");
    build(&bare, json, sizeof json);
    check("a positionless contact omits altitude_m", strstr(json, "altitude_m") == NULL, json);
    check("a positionless contact omits altitude_ref", strstr(json, "altitude_ref") == NULL, json);
    check("a positionless contact still carries its uas_id",
          strstr(json, "\"uas_id\":\"1786501045\"") != NULL, json);
    check("no field is ever emitted as null", strstr(json, ":null") == NULL, json);

    printf("\n%d/%d passed\n", passed, passed + failed);
    return failed ? 1 : 0;
}
