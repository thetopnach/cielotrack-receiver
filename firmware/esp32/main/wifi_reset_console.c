#include "wifi_reset_console.h"
#include "provisioning.h"

#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "resetwifi";

/* Read from the USB-serial-JTAG port directly rather than through esp_console: on this
 * board that port is the console's *secondary* (log) output, and standing up a console
 * REPL on it fights the logger for the peripheral. The driver's receive path is separate
 * from the ROM path the logs use, so installing it for input leaves logging untouched.
 *
 * One command, matched on a whole line, so stray bytes or a terminal's keep-alive never
 * trip it. */
#define CMD "resetwifi"

static void reset_task(void *arg) {
    (void)arg;
    uint8_t buf[64];
    char line[32];
    size_t len = 0;
    for (;;) {
        int n = usb_serial_jtag_read_bytes(buf, sizeof buf, pdMS_TO_TICKS(1000));
        for (int i = 0; i < n; i++) {
            char c = (char)buf[i];
            if (c == '\n' || c == '\r') {
                line[len] = '\0';
                if (len > 0) {
                    if (strcmp(line, CMD) == 0) {
                        ESP_LOGW(TAG, "'%s' received — clearing Wi-Fi and rebooting into "
                                      "provisioning", CMD);
                        provisioning_forget();   /* clears only Wi-Fi, restarts; never returns */
                    } else {
                        ESP_LOGI(TAG, "unknown command '%s' — the only command is '%s'",
                                 line, CMD);
                    }
                }
                len = 0;
            } else if (len < sizeof line - 1) {
                line[len++] = c;
            } else {
                /* Line longer than any command we take: drop it rather than match a
                 * prefix of something longer. */
                len = 0;
            }
        }
    }
}

void wifi_reset_console_start(void) {
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    /* Never fatal: a receiver with no way to reset its Wi-Fi from the cable is a smaller
     * problem than one that will not boot. INVALID_STATE just means it is already
     * installed, which is fine. */
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "USB-serial-JTAG driver install failed (%s); '%s' unavailable this "
                      "boot", esp_err_to_name(err), CMD);
        return;
    }
    if (xTaskCreate(reset_task, "resetwifi", 4096, NULL, 2, NULL) != pdPASS) {
        ESP_LOGW(TAG, "could not start the '%s' listener", CMD);
        return;
    }
    ESP_LOGI(TAG, "serial ready — type '%s' to move this receiver to a new network", CMD);
}
