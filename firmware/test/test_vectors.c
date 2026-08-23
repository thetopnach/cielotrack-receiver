/* Runs the C decoder against the shared conformance vectors, on the host.
 *
 * This is the point of the whole exercise: the parser is pure logic, so it can be
 * proved correct with a compiler and no radio. When the ESP32 later disagrees with the
 * Pi about what flew overhead, this having passed is what makes that a difference in
 * what each one *heard* rather than in what each one *understood*.
 *
 *     make -C firmware test
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../odid_decode.h"
#include "odid_vectors.h"

static int passed, failed;

static const char *altitude_ref_name(const odid_location_t *loc) {
    if (!loc->has_altitude) return "N/A";
    return loc->altitude_ref == ODID_ALT_AGL ? "agl" : "absolute";
}

static const char *height_ref_name(const odid_location_t *loc) {
    if (!loc->has_height) return "N/A";
    return loc->height_ref == ODID_HEIGHT_GROUND ? "ground" : "takeoff";
}

/* Looks a field up by the name the fixture uses, so the C and the Python are compared
 * through the same vocabulary rather than through two structs that happen to line up. */
static bool lookup(const odid_vector_t *vector, const char *field,
                   bool *is_text, const char **text, double *number) {
    *is_text = false;
    *text = NULL;
    *number = 0;

    if (strcmp(vector->decoder, "location") == 0) {
        odid_location_t loc = odid_decode_location(vector->message);
        if (strcmp(field, "lat") == 0)           { *number = loc.lat; return true; }
        if (strcmp(field, "lon") == 0)           { *number = loc.lon; return true; }
        if (strcmp(field, "speed_mps") == 0)     { *number = loc.speed_mps; return true; }
        if (strcmp(field, "vspeed_mps") == 0)    { *number = loc.vspeed_mps; return true; }
        if (strcmp(field, "direction_deg") == 0) { *number = loc.direction_deg; return true; }
        if (strcmp(field, "altitude_ref") == 0)  { *is_text = true; *text = altitude_ref_name(&loc); return true; }
        if (strcmp(field, "height_ref") == 0)    { *is_text = true; *text = height_ref_name(&loc); return true; }
        if (strcmp(field, "altitude_m") == 0) {
            if (!loc.has_altitude) { *is_text = true; *text = "N/A"; return true; }
            *number = loc.altitude_m; return true;
        }
        if (strcmp(field, "height_m") == 0) {
            if (!loc.has_height) { *is_text = true; *text = "N/A"; return true; }
            *number = loc.height_m; return true;
        }
    } else if (strcmp(vector->decoder, "basic_id") == 0) {
        odid_basic_id_t id = odid_decode_basic_id(vector->message);
        if (strcmp(field, "uas_id") == 0) {
            *is_text = true;
            *text = id.valid ? id.uas_id : "N/A";
            return true;
        }
    } else if (strcmp(vector->decoder, "system") == 0) {
        odid_system_t sys = odid_decode_system(vector->message);
        if (strcmp(field, "operator_lat") == 0) { *number = sys.operator_lat; return true; }
        if (strcmp(field, "operator_lon") == 0) { *number = sys.operator_lon; return true; }
        if (strcmp(field, "operator_altitude_m") == 0) {
            *number = sys.operator_altitude_m; return true;
        }
    }
    return false;
}

static bool decoded_nothing(const odid_vector_t *vector) {
    if (strcmp(vector->decoder, "location") == 0) {
        return !odid_decode_location(vector->message).valid;
    }
    if (strcmp(vector->decoder, "system") == 0) {
        return !odid_decode_system(vector->message).valid;
    }
    if (strcmp(vector->decoder, "basic_id") == 0) {
        return !odid_decode_basic_id(vector->message).valid;
    }
    return false;
}

int main(void) {
    printf("ODID conformance vectors, C implementation\n\n");

    for (int i = 0; i < ODID_VECTOR_COUNT; i++) {
        const odid_vector_t *vector = &odid_vectors[i];
        bool ok = true;
        char detail[256];
        detail[0] = '\0';

        if (vector->expect_count == 0) {
            /* An empty expectation means the message carries nothing usable. */
            ok = decoded_nothing(vector);
            if (!ok) snprintf(detail, sizeof detail, "expected nothing decoded");
        } else {
            for (int e = 0; e < vector->expect_count; e++) {
                const odid_expect_t *want = &vector->expects[e];
                bool is_text;
                const char *text;
                double number;
                if (!lookup(vector, want->field, &is_text, &text, &number)) {
                    ok = false;
                    snprintf(detail, sizeof detail, "no such field: %s", want->field);
                    break;
                }
                if (want->is_text != is_text) {
                    ok = false;
                    snprintf(detail, sizeof detail, "%s: expected %s, got %s",
                             want->field,
                             want->is_text ? want->text : "a number",
                             is_text ? text : "a number");
                    break;
                }
                if (is_text) {
                    if (strcmp(want->text, text) != 0) {
                        ok = false;
                        snprintf(detail, sizeof detail, "%s: expected \"%s\", got \"%s\"",
                                 want->field, want->text, text);
                        break;
                    }
                } else if (fabs(want->number - number) > 1e-6) {
                    ok = false;
                    snprintf(detail, sizeof detail, "%s: expected %g, got %g",
                             want->field, want->number, number);
                    break;
                }
            }
        }

        printf("  %s  %s", ok ? "PASS" : "FAIL", vector->name);
        if (!ok) printf("  - %s", detail);
        printf("\n");
        ok ? passed++ : failed++;
    }

    printf("\n%d/%d passed\n", passed, passed + failed);
    return failed == 0 ? 0 : 1;
}
