#include "receiver_ext.h"

/* Weak no-op defaults for the extension seam — see receiver_ext.h. An out-of-tree overlay
 * provides strong definitions that override these; the open build links these and does
 * nothing, so the payload and behaviour are identical to having no seam at all. */

__attribute__((weak)) void receiver_ext_init(void) {}

__attribute__((weak)) int receiver_ext_append_contact_fields(char *out, size_t size,
                                                             int used,
                                                             const uplink_contact_t *c) {
    (void)out;
    (void)size;
    (void)c;
    return used;
}
