/* Setting a receiver up from a phone instead of from a rebuild.
 *
 * Until this existed, putting a board on a network meant editing secrets.h and
 * recompiling — which is fine for the person who wrote the firmware and a non-starter
 * for anyone else. It also produced the single most expensive bug of this project so
 * far: an SSID typed as ATTF4U46ul instead of ATTF4U46uI, a lowercase l for an uppercase
 * I, indistinguishable in most fonts, which cost a flash cycle to find and came back
 * after being fixed once. A network picked from a scanned list cannot be mistyped.
 *
 * Espressif's "ESP BLE Provisioning" app is the client. The board advertises, the phone
 * finds it, the user picks a network and enters its password, and the credentials land
 * in NVS. From then on the board connects on its own and this code does nothing.
 *
 * What this costs the Wi-Fi board is worth being precise about. That board exists so
 * that no Bluetooth controller competes with Wi-Fi capture, and provisioning is BLE. The
 * controller is only ever started while provisioning is actually running — a board with
 * credentials in NVS never initialises it at all, and the scheme's own handler frees its
 * memory when provisioning ends. So the guarantee narrows from "never initialised" to
 * "never initialised once set up", and never overlaps with capture.
 */
#ifndef PROVISIONING_H
#define PROVISIONING_H

#include <stdbool.h>

/* Brings up Wi-Fi, provisioning over BLE first if this board has no credentials yet.
 * Blocks until the board is provisioned — a receiver with no network has nothing useful
 * to do, and returning early would only let capture start against a radio that is about
 * to be reconfigured.
 *
 * Returns true if it had to provision, false if credentials were already stored. */
bool provisioning_start_wifi(void);

/* Erases stored credentials and reboots into provisioning. For the case where a receiver
 * moves to a different network and nobody has a cable. */
void provisioning_forget(void);

#endif /* PROVISIONING_H */
