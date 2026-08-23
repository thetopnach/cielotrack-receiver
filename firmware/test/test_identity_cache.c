/* Naming a position from an earlier sighting of the same transmitter.
 *
 * BLE Remote ID rotates one message per advertisement: a Basic ID saying who, then a
 * Location saying where. Reported as they arrive, every positioned row carries no
 * identity and reaches the dashboard as "Unknown Aircraft" while its serial sits in a
 * row that never gets drawn. Measured on the bench board: 19 positions in 24 hours, 0
 * identified, against 13 of 14 on the Pi beside it.
 *
 * The semantics deliberately mirror radio_tracker.py's mac_identity cache, because the
 * point of a second receiver is that its rows are comparable with the first's. The parts
 * worth guarding are the ones that stop it inventing things: a transmitter that has
 * claimed two identities is never used to label anything, and weak evidence expires
 * quickly while a repeatedly confirmed association does not.
 */
#include <stdio.h>
#include <string.h>

#include "../identity_cache.h"

static int passed, failed;

static void check(const char *name, bool ok, const char *detail) {
    printf("  %s  %s", ok ? "PASS" : "FAIL", name);
    if (!ok && detail) printf("  - %s", detail);
    printf("\n");
    if (ok) passed++; else failed++;
}

#define MAC_A "CC:F9:57:9E:74:10"
#define MAC_B "20:BA:36:07:C6:F0"

int main(void) {
    char id[24], type[24];

    printf("identity cache\n");

    /* The case this exists for: identity decoded, then a position with none. */
    identity_cache_reset();
    identity_remember(MAC_A, "178650104A", "Hybrid Lift", 1000);
    bool got = identity_recall(MAC_A, id, sizeof id, type, sizeof type, 1030);
    check("a position is named from the identity just decoded",
          got && strcmp(id, "178650104A") == 0 && strcmp(type, "Hybrid Lift") == 0,
          got ? id : "nothing recalled");

    check("an unheard transmitter is not named",
          !identity_recall(MAC_B, id, sizeof id, type, sizeof type, 1030), NULL);

    /* Weak evidence expires; a confirmed association does not. */
    identity_cache_reset();
    identity_remember(MAC_A, "178650104A", "Hybrid Lift", 1000);
    check("one sighting expires past the weak horizon",
          !identity_recall(MAC_A, id, sizeof id, type, sizeof type,
                           1000 + IDENTITY_WEAK_TTL_SECONDS + 1), NULL);

    identity_cache_reset();
    for (int i = 0; i < IDENTITY_STRONG_SIGHTINGS; i++) {
        identity_remember(MAC_A, "178650104A", "Hybrid Lift", 1000 + i);
    }
    check("a repeatedly confirmed association survives the weak horizon",
          identity_recall(MAC_A, id, sizeof id, type, sizeof type,
                          1000 + IDENTITY_WEAK_TTL_SECONDS + 60), NULL);
    check("but not the strong one",
          !identity_recall(MAC_A, id, sizeof id, type, sizeof type,
                           1000 + IDENTITY_STRONG_TTL_SECONDS + 60), NULL);

    /* The property that stops it mislabelling aircraft. */
    identity_cache_reset();
    identity_remember(MAC_A, "178650104A", "Hybrid Lift", 1000);
    identity_remember(MAC_A, "1786501047", "Hybrid Lift", 1001);
    check("a transmitter claiming two identities is quarantined",
          !identity_recall(MAC_A, id, sizeof id, type, sizeof type, 1002), NULL);
    check("and the quarantine is counted", identity_cache_quarantined() == 1, NULL);

    identity_remember(MAC_A, "178650104A", "Hybrid Lift", 1100);
    check("a quarantined transmitter is not rehabilitated by repetition",
          !identity_recall(MAC_A, id, sizeof id, type, sizeof type, 1101), NULL);

    /* "N/A" is what the decoder yields for a Basic ID carrying no id. Remembering it
     * would quarantine every transmitter that ever sent one. */
    identity_cache_reset();
    identity_remember(MAC_A, "178650104A", "Hybrid Lift", 1000);
    identity_remember(MAC_A, "N/A", "", 1001);
    check("an empty Basic ID neither overwrites nor quarantines",
          identity_recall(MAC_A, id, sizeof id, type, sizeof type, 1002)
              && strcmp(id, "178650104A") == 0, id);

    /* A clock that steps backwards must not produce a huge negative age that passes. */
    identity_cache_reset();
    identity_remember(MAC_A, "178650104A", "Hybrid Lift", 5000);
    check("a backwards clock does not resurrect an entry",
          !identity_recall(MAC_A, id, sizeof id, type, sizeof type, 4000), NULL);

    /* Eviction must not silently forget a quarantine. */
    identity_cache_reset();
    identity_remember(MAC_A, "aaaa", "", 1);
    identity_remember(MAC_A, "bbbb", "", 2);          /* quarantines MAC_A */
    for (int i = 0; i < IDENTITY_CACHE_SIZE * 2; i++) {
        char mac[18];
        snprintf(mac, sizeof mac, "00:00:00:00:%02X:%02X", i / 256, i % 256);
        identity_remember(mac, "filler", "", 10 + (uint32_t)i);
    }
    check("a quarantine survives the cache filling up",
          !identity_recall(MAC_A, id, sizeof id, type, sizeof type, 20), NULL);

    printf("\n%d/%d passed\n", passed, passed + failed);
    return failed ? 1 : 0;
}
