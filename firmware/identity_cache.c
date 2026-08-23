#include "identity_cache.h"

#include <stdio.h>
#include <stddef.h>
#include <string.h>

typedef struct {
    char mac[18];
    char uas_id[24];
    char ua_type[24];
    uint32_t last_decoded_at;
    uint16_t sightings;
    bool quarantined;
    bool used;
} entry_t;

static entry_t entries[IDENTITY_CACHE_SIZE];

void identity_cache_reset(void) {
    memset(entries, 0, sizeof entries);
}

int identity_cache_quarantined(void) {
    int n = 0;
    for (int i = 0; i < IDENTITY_CACHE_SIZE; i++) {
        if (entries[i].used && entries[i].quarantined) n++;
    }
    return n;
}

static entry_t *find(const char *mac) {
    for (int i = 0; i < IDENTITY_CACHE_SIZE; i++) {
        if (entries[i].used && strcmp(entries[i].mac, mac) == 0) return &entries[i];
    }
    return NULL;
}

/* Evicts the least recently decoded entry when full.
 *
 * Quarantined entries are evicted last, not first. Forgetting a quarantine re-enables
 * exactly the mislabelling it was created to prevent, so only unquarantined entries are
 * considered while any exist. */
static entry_t *slot_for(const char *mac) {
    entry_t *found = find(mac);
    if (found) return found;

    entry_t *victim = NULL;
    for (int i = 0; i < IDENTITY_CACHE_SIZE; i++) {
        if (!entries[i].used) { victim = &entries[i]; break; }
        if (entries[i].quarantined) continue;
        if (victim == NULL || entries[i].last_decoded_at < victim->last_decoded_at) {
            victim = &entries[i];
        }
    }
    if (victim == NULL) victim = &entries[0];   /* everything quarantined; reuse the first */

    memset(victim, 0, sizeof *victim);
    snprintf(victim->mac, sizeof victim->mac, "%s", mac);
    victim->used = true;
    return victim;
}

void identity_remember(const char *mac, const char *uas_id, const char *ua_type,
                       uint32_t now_seconds) {
    if (mac == NULL || uas_id == NULL || uas_id[0] == '\0') return;
    /* "N/A" is what the decoder returns for a Basic ID with no id in it, and is not
     * something worth remembering or conflicting against. */
    if (strcmp(uas_id, "N/A") == 0) return;

    entry_t *e = slot_for(mac);
    if (e->quarantined) return;

    if (e->uas_id[0] != '\0' && strcmp(e->uas_id, uas_id) != 0) {
        /* Two identities from one transmitter. Neither can be trusted from here on. */
        e->quarantined = true;
        e->uas_id[0] = '\0';
        e->ua_type[0] = '\0';
        return;
    }

    snprintf(e->uas_id, sizeof e->uas_id, "%s", uas_id);
    if (ua_type != NULL) snprintf(e->ua_type, sizeof e->ua_type, "%s", ua_type);
    e->last_decoded_at = now_seconds;
    if (e->sightings < 0xFFFF) e->sightings++;
}

bool identity_recall(const char *mac, char *uas_id, size_t uas_id_size,
                     char *ua_type, size_t ua_type_size, uint32_t now_seconds) {
    if (mac == NULL) return false;
    entry_t *e = find(mac);
    if (e == NULL || e->quarantined || e->uas_id[0] == '\0') return false;

    uint32_t horizon = (e->sightings >= IDENTITY_STRONG_SIGHTINGS)
                           ? IDENTITY_STRONG_TTL_SECONDS
                           : IDENTITY_WEAK_TTL_SECONDS;
    /* Unsigned subtraction, so a clock that stepped backwards reads as a huge age and
     * the entry is simply not used — which is the safe direction. */
    if (now_seconds < e->last_decoded_at) return false;
    if (now_seconds - e->last_decoded_at > horizon) return false;

    if (uas_id != NULL) snprintf(uas_id, uas_id_size, "%s", e->uas_id);
    if (ua_type != NULL) snprintf(ua_type, ua_type_size, "%s", e->ua_type);
    return true;
}
