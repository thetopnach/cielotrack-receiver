#include "provisioning.h"

#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "psa/crypto.h"
#include "network_provisioning/manager.h"
#include "network_provisioning/scheme_ble.h"
#include "nvs_flash.h"

#include "secrets.h"

static const char *TAG = "provision";

/* Security 1, not 0. Security 0 is plain text, and a receiver that anyone within BLE
 * range can silently point at their own access point is worse than one that is hard to
 * set up: it would keep capturing and keep reporting, to somebody else's network.
 *
 * The proof of possession is derived from this board's MAC and a salt shared across the
 * fleet, so one build still serves every board — deriving it from the MAC alone would be
 * no security at all, since the MAC is in the advertisement the phone is reading. The
 * salt is the secret; the MAC only makes each board's code different.
 *
 * It is printed on the serial console at boot because that is the one channel a person
 * setting up a board in their hand definitely has. A production run would put it on a
 * label or a QR code instead. */
static char pop[13];
static char service_name[16];

static void derive_identity(void) {
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));

    /* Named for what it is when it appears in a list of nearby devices. The last three
     * MAC bytes distinguish boards from each other at a glance, and match what is
     * printed on the console. */
    snprintf(service_name, sizeof service_name, "CIELO_%02X%02X%02X",
             mac[3], mac[4], mac[5]);

    /* PSA rather than mbedtls_sha256_*: mbedtls 4 moved that header under private/ and
     * the supported public interface is PSA Crypto. */
    unsigned char input[sizeof(CIELOTRACK_PROV_SALT) + sizeof mac];
    size_t salt_len = strlen(CIELOTRACK_PROV_SALT);
    memcpy(input, CIELOTRACK_PROV_SALT, salt_len);
    memcpy(input + salt_len, mac, sizeof mac);

    unsigned char digest[32];
    size_t digest_len = 0;
    ESP_ERROR_CHECK(psa_crypto_init() == PSA_SUCCESS ? ESP_OK : ESP_FAIL);
    psa_status_t status = psa_hash_compute(PSA_ALG_SHA_256, input, salt_len + sizeof mac,
                                           digest, sizeof digest, &digest_len);
    ESP_ERROR_CHECK(status == PSA_SUCCESS ? ESP_OK : ESP_FAIL);

    for (int i = 0; i < 6; i++) {
        snprintf(pop + i * 2, 3, "%02x", digest[i]);
    }
}

static void on_prov_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)base;
    switch (id) {
    case NETWORK_PROV_START:
        ESP_LOGI(TAG, "waiting for a phone: open ESP BLE Provisioning and choose \"%s\"",
                 service_name);
        ESP_LOGI(TAG, "proof of possession: %s", pop);
        break;
    case NETWORK_PROV_WIFI_CRED_RECV: {
        wifi_sta_config_t *got = (wifi_sta_config_t *)data;
        /* The SSID as the phone read it from a scan, not as anyone typed it. Logged
         * because this is the exact value that used to be wrong. */
        ESP_LOGI(TAG, "credentials received for \"%s\"", (const char *)got->ssid);
        break;
    }
    case NETWORK_PROV_WIFI_CRED_FAIL: {
        network_prov_wifi_sta_fail_reason_t *why =
            (network_prov_wifi_sta_fail_reason_t *)data;
        /* Said in the terms the person holding the phone can act on, and distinguished,
         * because "provisioning failed" sends them to check the wrong thing. */
        ESP_LOGE(TAG, "could not join: %s",
                 *why == NETWORK_PROV_WIFI_STA_AUTH_ERROR
                     ? "wrong password"
                     : "network not found — note this radio is 2.4 GHz only, so a "
                       "5 GHz-only network will not appear or connect");
        network_prov_mgr_reset_wifi_sm_state_on_failure();
        break;
    }
    case NETWORK_PROV_WIFI_CRED_SUCCESS:
        ESP_LOGI(TAG, "joined; credentials stored");
        break;
    case NETWORK_PROV_END:
        ESP_LOGI(TAG, "provisioning finished; Bluetooth is being released");
        break;
    default:
        break;
    }
}

bool provisioning_start_wifi(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_event_handler_register(NETWORK_PROV_EVENT, ESP_EVENT_ANY_ID,
                                               &on_prov_event, NULL));

    /* What happens to Bluetooth when provisioning ends depends on whether this board
     * still needs it.
     *
     * The Wi-Fi board does not: freeing the controller's memory is the difference
     * between a radio it merely stopped using and one it no longer has, which is the
     * point of splitting the transports.
     *
     * The BLE board very much does. esp_bt_mem_release cannot be undone, so freeing here
     * would leave a board that provisioned successfully and then had no radio to scan
     * with — it would have failed the first time anyone used the feature on the board
     * that most needs it. */
    network_prov_mgr_config_t config = {
        .scheme = network_prov_scheme_ble,
#if CIELOTRACK_ROLE_WIFI
        .scheme_event_handler = NETWORK_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM,
#else
        .scheme_event_handler = NETWORK_PROV_EVENT_HANDLER_NONE,
#endif
    };
    ESP_ERROR_CHECK(network_prov_mgr_init(config));

    bool provisioned = false;
    ESP_ERROR_CHECK(network_prov_mgr_is_wifi_provisioned(&provisioned));

    if (provisioned) {
        /* Nothing to do, and importantly the Bluetooth controller was never started —
         * network_prov_mgr_init does not touch it, only start_provisioning does. A board
         * that is already set up carries this code and never runs the radio. */
        network_prov_mgr_deinit();
        ESP_LOGI(TAG, "already provisioned; Bluetooth never started");
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());
        return false;
    }

    derive_identity();
    ESP_LOGW(TAG, "no network configured — starting BLE provisioning");
    ESP_ERROR_CHECK(network_prov_mgr_start_provisioning(
        NETWORK_PROV_SECURITY_1, (const void *)pop, service_name, NULL));

    /* Blocks until the phone has finished. A receiver with no network has nothing useful
     * to do, and letting capture start here would only run it against a radio that is
     * about to be reconfigured. */
    network_prov_mgr_wait();
    network_prov_mgr_deinit();

    /* Already connected as a side effect of provisioning, so no start() here. */
    return true;
}

void provisioning_forget(void) {
    ESP_LOGW(TAG, "erasing stored credentials; the board will restart into provisioning");
    ESP_ERROR_CHECK(network_prov_mgr_reset_wifi_provisioning());
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}
