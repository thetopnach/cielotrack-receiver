/* Extension seam for out-of-tree builds.
 *
 * The open firmware is one source of truth. A private build layered on top of it — a
 * partner or commercial fleet — should NOT fork this tree. It adds an ESP-IDF component
 * that provides STRONG definitions of the weak hooks declared here; the open build links
 * the weak no-op defaults (in receiver_ext.c) instead. The defaults do nothing and change
 * no payload, so the public firmware behaves identically whether or not an overlay is
 * present, and there is no partner-specific symbol named anywhere in the open tree.
 *
 * The defaults deliberately live in their own translation unit. A weak definition sitting
 * in the same file that calls it can be bound at compile time under optimisation, which
 * would stop an overlay's strong definition from ever taking effect; keeping them apart
 * from every call site keeps the override reliable.
 */
#ifndef RECEIVER_EXT_H
#define RECEIVER_EXT_H

#include <stddef.h>

#include "uplink.h"

/* Called once from app_main, early, before the radios come up. An overlay uses it for its
 * own setup — and referencing it from the core is also what pulls the overlay component
 * into the link, so its other strong symbols can override the weak ones. Open default:
 * does nothing. */
void receiver_ext_init(void);

/* Called at the end of contact_append_fields(), so an overlay can add its own fields to
 * every serialised contact — a fleet tag, a partner-specific measurement — using the same
 * contact_append_* helpers, and return the new length. Open default: appends nothing and
 * returns `used` unchanged. */
int receiver_ext_append_contact_fields(char *out, size_t size, int used,
                                       const uplink_contact_t *c);

#endif /* RECEIVER_EXT_H */
