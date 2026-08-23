/* Asking the server who this board is, instead of being told at compile time.
 *
 * The last per-board secret in the build. With this, secrets.h needs nothing but the
 * server URL and the fleet salt: a board is flashed from the same binary as every other,
 * put on a network from a phone, and then asks for its own identity. What it gets back
 * is one API key, scoped to one device.
 *
 * Identity is derived, not stored, and that is deliberate. Both the device id and the
 * bootstrap secret come from this board's MAC and the fleet salt, so a board that loses
 * its NVS — a partition change, a full erase, the thing that happened to both boards the
 * night this was written — comes back as the same device rather than as a stranger, and
 * can simply be claimed again. Storing a random id would have made every erase orphan a
 * row on the server and every recovery need someone with database access.
 *
 * The API key is the one thing that must be kept, because the server hands it over
 * exactly once and erases its own copy immediately afterwards — so that the database
 * stops being a list of live credentials. A device that loses the key has to be
 * re-claimed. It is therefore written to NVS and read back before the claim is treated
 * as finished.
 */
#ifndef CLAIM_H
#define CLAIM_H

#include <stdbool.h>

/* Starts the claim on its own task and returns at once. Capture never waits for it: a
 * receiver with no credentials still hears aircraft, and the whole design keeps decoding
 * independent of whether anything can be delivered. */
void claim_start(void);

/* True once there is an API key to report with. */
bool claim_ready(void);

/* Valid only once claim_ready(). */
const char *claim_device_id(void);
const char *claim_api_key(void);

#endif /* CLAIM_H */
