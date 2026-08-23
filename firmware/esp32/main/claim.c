#include "claim.h"

#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "psa/crypto.h"

#include "secrets.h"
#include "uplink.h"

static const char *TAG = "claim";

#define NVS_NAMESPACE "cielotrack"
#define NVS_KEY_API   "api_key"

static char device_id[40];
static char bootstrap[33];
static char api_key[80];
static bool ready;

bool claim_ready(void) { return ready; }
const char *claim_device_id(void) { return device_id; }
const char *claim_api_key(void) { return api_key; }

/* Both identifiers come from the same hash of the fleet salt and this board's MAC, with
 * different prefixes so they cannot collide. Deterministic on purpose: see claim.h. */
static void derive(const char *purpose, unsigned char *out) {
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));

    unsigned char input[96];
    size_t used = snprintf((char *)input, sizeof input, "%s|%s|", purpose,
                           CIELOTRACK_PROV_SALT);
    memcpy(input + used, mac, sizeof mac);

    size_t len = 0;
    psa_status_t status = psa_hash_compute(PSA_ALG_SHA_256, input, used + sizeof mac,
                                           out, 32, &len);
    ESP_ERROR_CHECK(status == PSA_SUCCESS ? ESP_OK : ESP_FAIL);
}

static void derive_identity(void) {
    unsigned char digest[32];

    /* Shaped as a UUID because every other device row is one, and a fleet listing where
     * one id looks unlike the rest invites the question of whether it is a real device. */
    derive("device-id", digest);
    snprintf(device_id, sizeof device_id,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             digest[0], digest[1], digest[2], digest[3], digest[4], digest[5],
             digest[6], digest[7], digest[8], digest[9], digest[10], digest[11],
             digest[12], digest[13], digest[14], digest[15]);

    derive("bootstrap", digest);
    for (int i = 0; i < 16; i++) {
        snprintf(bootstrap + i * 2, 3, "%02x", digest[i]);
    }
}

/* One request, returning the parsed body or NULL. The caller owns the result. */
static cJSON *request(const char *url, esp_http_client_method_t method,
                      const char *header_name, const char *header_value,
                      const char *body, int *status_out) {
    char response[512];
    esp_http_client_config_t config = {
        .url = url,
        .method = method,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (header_name) esp_http_client_set_header(client, header_name, header_value);
    if (body) {
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, body, strlen(body));
    }

    cJSON *parsed = NULL;
    esp_err_t err = esp_http_client_open(client, body ? (int)strlen(body) : 0);
    if (err == ESP_OK) {
        if (body) esp_http_client_write(client, body, strlen(body));
        esp_http_client_fetch_headers(client);
        int read = esp_http_client_read_response(client, response, sizeof response - 1);
        if (read >= 0) {
            response[read] = '\0';
            parsed = cJSON_Parse(response);
        }
        *status_out = esp_http_client_get_status_code(client);
    } else {
        *status_out = 0;
        ESP_LOGW(TAG, "%s: %s", url, esp_err_to_name(err));
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return parsed;
}

static bool store_api_key(const char *key) {
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return false;
    esp_err_t err = nvs_set_str(nvs, NVS_KEY_API, key);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err != ESP_OK) return false;

    /* Read back before calling it done. The server erased its own copy the moment it
     * sent this, so a write that silently failed would cost a re-claim, and the board
     * would look claimed while being unable to prove it. */
    char check[sizeof api_key];
    size_t len = sizeof check;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return false;
    err = nvs_get_str(nvs, NVS_KEY_API, check, &len);
    nvs_close(nvs);
    return err == ESP_OK && strcmp(check, key) == 0;
}

static bool load_api_key(void) {
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return false;
    size_t len = sizeof api_key;
    esp_err_t err = nvs_get_str(nvs, NVS_KEY_API, api_key, &len);
    nvs_close(nvs);
    return err == ESP_OK && api_key[0] != '\0';
}

static void claim_task(void *arg) {
    (void)arg;
    derive_identity();

    if (load_api_key()) {
        ESP_LOGI(TAG, "already claimed as %s", device_id);
        ready = true;
        vTaskDelete(NULL);
        return;
    }

    char url[192];
    char body[192];
    char code[16] = "";

    for (;;) {
        /* Nothing can be claimed without a network, and there is no hurry: this runs
         * once in a board's life. */
        while (!uplink_connected()) vTaskDelay(pdMS_TO_TICKS(2000));

        if (code[0] == '\0') {
            snprintf(url, sizeof url, CIELOTRACK_SERVER_URL "/v1/devices/claim");
            snprintf(body, sizeof body,
                     "{\"device_id\":\"%s\",\"bootstrap_secret\":\"%s\"}",
                     device_id, bootstrap);
            int status = 0;
            cJSON *json = request(url, HTTP_METHOD_POST, NULL, NULL, body, &status);
            if (json) {
                const cJSON *claim_code = cJSON_GetObjectItem(json, "claim_code");
                if (cJSON_IsString(claim_code)) {
                    snprintf(code, sizeof code, "%s", claim_code->valuestring);
                }
                cJSON_Delete(json);
            }
            if (status == 403) {
                /* The server knows this device under a different secret. Since both are
                 * derived from the fleet salt, the usual cause is that the salt changed
                 * — which is a fleet-wide decision, not a fault of this board. */
                ESP_LOGE(TAG, "the server has %s registered with a different secret; "
                              "CIELOTRACK_PROV_SALT has probably changed since it was "
                              "first registered. Delete the device and re-claim it.",
                         device_id);
                vTaskDelay(pdMS_TO_TICKS(60000));
                continue;
            }
        }

        if (code[0] != '\0') {
            /* Said plainly and repeated, because this is the one thing a person has to
             * act on and they may not be watching when it first appears. */
            ESP_LOGW(TAG, "NOT YET CLAIMED. Enter code %s at " CIELOTRACK_SERVER_URL
                          " to add this receiver to your account.", code);
        }

        snprintf(url, sizeof url, CIELOTRACK_SERVER_URL "/v1/devices/%s/status",
                 device_id);
        int status = 0;
        cJSON *json = request(url, HTTP_METHOD_GET, "X-Bootstrap-Secret", bootstrap,
                              NULL, &status);
        if (json) {
            const cJSON *key = cJSON_GetObjectItem(json, "api_key");
            const cJSON *detail = cJSON_GetObjectItem(json, "detail");
            if (cJSON_IsString(key)) {
                snprintf(api_key, sizeof api_key, "%s", key->valuestring);
                if (store_api_key(api_key)) {
                    ESP_LOGI(TAG, "claimed; key stored. Reporting as %s", device_id);
                    ready = true;
                    cJSON_Delete(json);
                    vTaskDelete(NULL);
                    return;
                }
                /* Worth shouting about: the key is gone from the server too. */
                ESP_LOGE(TAG, "could not persist the API key, and the server has already "
                              "erased its copy. This device must be re-claimed.");
                api_key[0] = '\0';
            } else if (cJSON_IsString(detail)) {
                ESP_LOGE(TAG, "%s", detail->valuestring);
            }
            cJSON_Delete(json);
        }

        vTaskDelay(pdMS_TO_TICKS(15000));
    }
}

void claim_start(void) {
    xTaskCreate(claim_task, "cielotrack_claim", 6144, NULL, 3, NULL);
}
