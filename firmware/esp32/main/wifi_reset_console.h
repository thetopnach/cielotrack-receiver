/* A one-command serial console: `resetwifi`.
 *
 * Until this existed, a receiver that had moved to a new network had no way back onto
 * one without a reflash — provisioning_forget() was written for exactly this and had no
 * caller. This wires it to the one channel a cabled board always has: the USB serial
 * port it already logs to. Open a terminal on the "USB JTAG/serial debug unit" port and
 * type `resetwifi`; the board clears only its stored Wi-Fi credentials and reboots into
 * BLE provisioning, where a phone sets the new network. The identity it was claimed
 * under lives in a different NVS namespace and is kept, so it rejoins as the same
 * receiver rather than needing to be claimed again.
 */
#ifndef WIFI_RESET_CONSOLE_H
#define WIFI_RESET_CONSOLE_H

/* Starts the console REPL on the USB-serial-JTAG port. Safe to call once at boot; it
 * runs alongside the normal log output on the same port. */
void wifi_reset_console_start(void);

#endif /* WIFI_RESET_CONSOLE_H */
