#include "uplink.h"
#include "contact_time.h"

#include <sys/time.h>

#include "esp_timer.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#include "claim.h"
#include "provisioning.h"
#include "secrets.h"

#if !defined(CIELOTRACK_SENSOR_ONLY)
#define CIELOTRACK_SENSOR_ONLY 0
#endif

static const char *TAG = "uplink";

/* Sized for a full drain of either role's queue at roughly 200 bytes a contact. */
#define UPLINK_BATCH_BODY_BYTES 8192

/* Credentials come from the claim unless they were compiled in. Defining the key opts
 * out of claiming entirely, the same way defining the SSID opts out of provisioning:
 * two sources of truth for who this board is would be one too many. */
#ifdef CIELOTRACK_DEVICE_KEY
#define UPLINK_HAVE_CREDENTIALS()  true
#define UPLINK_DEVICE_KEY()        CIELOTRACK_DEVICE_KEY
#define UPLINK_DEVICE_ID()         CIELOTRACK_DEVICE_ID
#else
#define UPLINK_HAVE_CREDENTIALS()  claim_ready()
#define UPLINK_DEVICE_KEY()        claim_api_key()
#define UPLINK_DEVICE_ID()         claim_device_id()
#endif

/* Derived from the role, never configured separately. The Wi-Fi board filing its
 * detections as BLE would quietly corrupt the only comparison the second board exists to
 * make, and nothing downstream could catch it. */
#if CIELOTRACK_ROLE_WIFI
#define CIELOTRACK_PROTOCOL "Wi-Fi"
#else
#define CIELOTRACK_PROTOCOL "BLE"
#endif

#if !CIELOTRACK_SENSOR_ONLY
static EventGroupHandle_t events;
#endif
#if !CIELOTRACK_SENSOR_ONLY
static esp_netif_t *sta_netif;
#endif
#define CONNECTED_BIT BIT0
#define CLOCK_BIT     BIT1

#if !CIELOTRACK_SENSOR_ONLY
static uint32_t sent_count, failed_count;
#endif
#if !CIELOTRACK_SENSOR_ONLY
static uint32_t heartbeat_count, heartbeat_failed_count;
#endif

/* See uplink_hold_station(). Read from the Wi-Fi event task and written from the capture
 * task, and only ever a whole-word flag, so volatile is the whole of the synchronisation
 * it needs. */
static volatile bool station_held;

#if !CIELOTRACK_SENSOR_ONLY
/* Reconnect forever rather than giving up after N tries. A receiver on a shelf outlives
 * every router reboot it will ever see, and a firmware that stops trying after five
 * minutes turns a thirty-second outage into a dead unit nobody notices until they check.
 */
static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (!station_held) esp_wifi_connect();
#ifdef CIELOTRACK_STATIC_IP
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        /* Not waiting for IP_EVENT_STA_GOT_IP: with the address assigned rather than
         * leased, association is the moment the board can talk, and depending on an
         * event that exists to announce a lease would be depending on the very step
         * this configuration removes. */
        ESP_LOGI(TAG, "network up: " CIELOTRACK_STATIC_IP " (static)");
        xEventGroupSetBits(events, CONNECTED_BIT);
#endif
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *gone = (wifi_event_sta_disconnected_t *)data;
        xEventGroupClearBits(events, CONNECTED_BIT);
        /* The reason code, because "disconnected; reconnecting" on a loop is a symptom
         * shared by three unrelated causes — a wrong password, a network the radio
         * cannot see, and a radio too busy scanning to associate — and they need
         * completely different fixes. Discarding it turned a two-second answer into
         * guesswork. */
        const char *why = "see esp_wifi_types.h";
        switch (gone->reason) {
        case WIFI_REASON_NO_AP_FOUND:
            why = "no such network in range — check the SSID, and that it is 2.4 GHz: "
                  "this radio cannot see a 5 GHz band at all";
            break;
        case WIFI_REASON_AUTH_FAIL:
            why = "authentication refused — check the password";
            break;
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
            /* Split from AUTH_FAIL, which it was lumped in with. The two need opposite
             * responses and the wrong label sends you to the wrong place: this board hit
             * 204 twice in a row and then associated with the same credentials twelve
             * seconds later, so "check the password" was advice to go and verify
             * something that was already correct. */
            why = "the WPA2 handshake timed out, not a refusal — the credentials are "
                  "fine and the access point did not finish in time. Common on a mesh "
                  "that hands a rejoining client to a different node";
            break;
        case WIFI_REASON_ASSOC_FAIL:
        case WIFI_REASON_CONNECTION_FAIL:
            why = "association failed; if BLE is scanning at full duty this may be "
                  "airtime rather than the network";
            break;
        case WIFI_REASON_BEACON_TIMEOUT:
            why = "lost the access point's beacons — weak signal or it went away";
            break;
        default:
            break;
        }
        /* Quiet when the disconnect was asked for: a board that leaves the network on
         * purpose every minute would otherwise fill its log with a warning about the
         * thing it just did, and bury the disconnects that mean something. */
        if (station_held) return;
        ESP_LOGW(TAG, "Wi-Fi disconnected, reason %d: %s", gone->reason, why);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *got = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "network up: " IPSTR, IP2STR(&got->ip_info.ip));
        xEventGroupSetBits(events, CONNECTED_BIT);
    }
}
#endif

#if !CIELOTRACK_SENSOR_ONLY
static void clock_synced(struct timeval *tv) {
    (void)tv;
    time_t now = time(NULL);
    struct tm utc;
    gmtime_r(&now, &utc);
    char stamp[32];
    strftime(stamp, sizeof stamp, "%Y-%m-%dT%H:%M:%SZ", &utc);
    ESP_LOGI(TAG, "clock set: %s", stamp);
    xEventGroupSetBits(events, CLOCK_BIT);
}
#endif

#if CIELOTRACK_SENSOR_ONLY
void uplink_start(void) {
    /* The radio has to be running for promiscuous capture, and nothing must ever
     * associate. An association owns the channel — esp_wifi_set_channel is refused
     * outright while the station is scanning or connected — which is the whole problem
     * this board exists to avoid. So: driver up, mode set, started, and no netif, no
     * credentials, no connect, ever.
     *
     * The first version of sensor mode left the ordinary uplink_start() in place. It
     * brought up a station, capture then failed to claim channel 6, and ESP_ERROR_CHECK
     * turned that into a boot loop. Compiling out the queue and the upload cycle was not
     * the same thing as compiling out the station. */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "radio up for capture only — no station, no network");
}

bool uplink_ready(void) { return false; }
bool uplink_connected(void) { return false; }
void uplink_resync_clock(void) { }
void uplink_hold_station(bool hold) { (void)hold; }

#else

void uplink_start(void) {
    events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    sta_netif = esp_netif_create_default_wifi_sta();

#ifdef CIELOTRACK_STATIC_IP
    /* A fixed address, to take DHCP out of a cycle that rejoins every minute.
     *
     * This was tried on the network it was written for and reverted the same evening:
     * only the first association after boot could reach anything, because the mesh hands
     * a rejoining client to a different node and without a lease exchange the new node
     * would not route for the address. secrets.h.example carries the full account and
     * the numbers. Off by default, and the default is the tested path.
     *
     * Kept because the mechanism is correct and a network with a single access point,
     * or a proper DHCP reservation, is exactly where it would pay — but read that note
     * before turning it on, and watch the upload failure count afterwards. */
    ESP_ERROR_CHECK(esp_netif_dhcpc_stop(sta_netif));
    esp_netif_ip_info_t ip = { 0 };
    ip.ip.addr = esp_ip4addr_aton(CIELOTRACK_STATIC_IP);
    ip.gw.addr = esp_ip4addr_aton(CIELOTRACK_STATIC_GATEWAY);
    ip.netmask.addr = esp_ip4addr_aton(CIELOTRACK_STATIC_NETMASK);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(sta_netif, &ip));

    /* Set explicitly, because they normally arrive with the lease that is no longer
     * being requested. Without them the board resolves nothing — and would fail at the
     * time server and the uplink both, which looks like a network fault rather than a
     * missing setting. */
    esp_netif_dns_info_t dns = { .ip.type = ESP_IPADDR_TYPE_V4 };
    dns.ip.u_addr.ip4.addr = esp_ip4addr_aton(CIELOTRACK_STATIC_DNS);
    ESP_ERROR_CHECK(esp_netif_set_dns_info(sta_netif, ESP_NETIF_DNS_MAIN, &dns));
    dns.ip.u_addr.ip4.addr = esp_ip4addr_aton(CIELOTRACK_STATIC_DNS_BACKUP);
    ESP_ERROR_CHECK(esp_netif_set_dns_info(sta_netif, ESP_NETIF_DNS_BACKUP, &dns));
    ESP_LOGI(TAG, "static address %s, gateway %s",
             CIELOTRACK_STATIC_IP, CIELOTRACK_STATIC_GATEWAY);
#endif

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &on_wifi_event, NULL, NULL));


    /* Wi-Fi and BLE share one radio. Modem sleep lets the controller interleave them
     * instead of the station starving the scanner — without it, joining a network
     * measurably costs advertisements, which is the one thing this receiver is for. */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));

#ifdef CIELOTRACK_WIFI_SSID
    /* Credentials compiled in. Kept as an escape hatch — a board being developed on, or
     * one whose network has no human nearby with a phone — but it is no longer the way a
     * receiver is meant to be set up. Defining the SSID opts out of provisioning
     * entirely, so the two can never disagree about which network to join. */
    wifi_config_t wifi = { 0 };
    snprintf((char *)wifi.sta.ssid, sizeof wifi.sta.ssid, "%s", CIELOTRACK_WIFI_SSID);
    snprintf((char *)wifi.sta.password, sizeof wifi.sta.password, "%s",
             CIELOTRACK_WIFI_PASSWORD);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi));
    ESP_ERROR_CHECK(esp_wifi_start());
#else
    /* No credentials in the build: they come from NVS, and from a phone the first time.
     * This blocks until the board has a network. */
    provisioning_start_wifi();
#endif

    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    sntp.sync_cb = clock_synced;
    sntp.start = true;
    ESP_ERROR_CHECK(esp_netif_sntp_init(&sntp));
}

bool uplink_connected(void) {
    return events != NULL && (xEventGroupGetBits(events) & CONNECTED_BIT);
}

/* Ask for the time again, now, rather than waiting out SNTP's retry interval.
 *
 * That interval is an hour (CONFIG_LWIP_SNTP_UPDATE_DELAY), which is right for
 * correcting drift and wrong for a first sync that failed. A board whose boot-time
 * attempt missed — because the access point was slow to hand out a lease, or the radio
 * was busy — has a working network on every subsequent cycle and still cannot report
 * anything, because uplink_ready() requires a clock and a detection stamped 1970 is
 * worse than one that never arrived. Measured on the Wi-Fi board: connected at 242s,
 * still refusing to send at 255s, with three upload windows already wasted.
 *
 * Safe to call when the clock is already set, and safe to call repeatedly. */
void uplink_resync_clock(void) {
    if (events == NULL) return;
    if (xEventGroupGetBits(events) & CLOCK_BIT) return;
    if (!uplink_connected()) return;
    ESP_LOGI(TAG, "no clock yet; asking for the time again");
    esp_sntp_restart();
}

void uplink_hold_station(bool hold) {
    station_held = hold;
    if (!hold) esp_wifi_connect();
}

#endif /* CIELOTRACK_SENSOR_ONLY */

#if !CIELOTRACK_SENSOR_ONLY
bool uplink_ready(void) {
    if (events == NULL) return false;
    EventBits_t bits = xEventGroupGetBits(events);
    /* Credentials belong here rather than at each call site: an unclaimed board has an
     * address and a clock and still has nowhere to send anything, and every caller
     * already treats "not ready" as "keep capturing, deliver later". */
    return (bits & CONNECTED_BIT) && (bits & CLOCK_BIT) && UPLINK_HAVE_CREDENTIALS();
}

/* Appends "key": value only when the value was actually decoded. A field omitted says
 * "we do not know"; a field sent as null says "we know it is nothing", and the two are
 * different claims about an aircraft. */
static int append_number(char *out, size_t size, int used, const char *key, double value) {
    if (isnan(value)) return used;
    return used + snprintf(out + used, size - used, ",\"%s\":%.7f", key, value);
}

static int append_string(char *out, size_t size, int used, const char *key,
                         const char *value) {
    if (value == NULL || value[0] == '\0') return used;
    return used + snprintf(out + used, size - used, ",\"%s\":\"%s\"", key, value);
}

/* One POST, shared by detections and heartbeats. Returns true on a 2xx.
 *
 * Synchronous on the caller's task by design — both callers are already off the radio
 * path, and a TLS handshake on a capture callback would drop broadcasts while the
 * socket worked, which is precisely backwards for a receiver. */
static bool post_json_as(const char *url, const char *body, const char *key) {
    char auth[128];
    snprintf(auth, sizeof auth, "Bearer %s", key);

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 8000,
        /* The bundle rather than a pinned certificate: this talks to a host whose
         * certificate rotates every ninety days, and a pin nobody remembers to update
         * is a receiver that goes silent on a Tuesday for no visible reason. */
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err == ESP_OK && status >= 200 && status < 300) return true;
    /* Said once per failure and no more. A receiver that cannot reach the server still
     * has a job to do, and filling the log with it helps nobody. */
    ESP_LOGW(TAG, "POST %s: %s status %d", url, esp_err_to_name(err), status);
    return false;
}

/* Both clocks at one instant, so every contact in a batch is aged against the same
 * reading. Taking them per contact would let the batch's own send time drift into the
 * ages it is meant to remove. */
static void clocks_now(int64_t *wall_us, int64_t *mono_us) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    *wall_us = (int64_t)tv.tv_sec * 1000000 + (int64_t)tv.tv_usec;
    *mono_us = esp_timer_get_time();
}

static bool post_json(const char *url, const char *body) {
    return post_json_as(url, body, UPLINK_DEVICE_KEY());
}

/* One contact as a JSON object, appended at `used`. Returns the new length, which may
 * exceed `size` — the caller checks, exactly as the single-report path always did. */
static int append_contact(char *out, size_t size, int used, const char *stamp,
                          const uplink_contact_t *c) {
    used += snprintf(out + used, size > (size_t)used ? size - used : 0,
                     "{\"detected_at\":\"%s\",\"protocol\":\"%s\"",
                     stamp, CIELOTRACK_PROTOCOL);
    if (used >= (int)size) return used;
    used = append_string(out, size, used, "mac", c->mac);
    used = append_string(out, size, used, "uas_id", c->uas_id);
    used = append_string(out, size, used, "ua_type", c->ua_type);
    used = append_number(out, size, used, "lat", c->lat);
    used = append_number(out, size, used, "lon", c->lon);
    used = append_number(out, size, used, "altitude_m", c->altitude_m);
    used = append_number(out, size, used, "speed_mps", c->speed_mps);
    used += snprintf(out + used, size > (size_t)used ? size - used : 0,
                     ",\"rssi_dbm\":%d,\"message_count\":%d,"
                     "\"identity_source\":\"%s\"}", c->rssi_dbm, c->message_count,
                     c->inferred ? "inferred-from-mac" : "decoded");
    return used;
}

void uplink_report_batch(const uplink_contact_t *contacts, int count) {
    if (count <= 0) return;
    if (!uplink_ready()) {
        failed_count += (uint32_t)count;
        return;
    }

    int64_t wall_us, mono_us;
    clocks_now(&wall_us, &mono_us);

    /* Static, not on the stack: this task has 6 kB and a full batch is several. */
    static char body[UPLINK_BATCH_BODY_BYTES];
    int used = snprintf(body, sizeof body, "{\"detections\":[");
    int packed = 0;
    for (int i = 0; i < count; i++) {
        int before = used;
        if (packed) used += snprintf(body + used, sizeof body - used, ",");
        char stamp[32];
        contact_stamp(wall_us, mono_us, contacts[i].decoded_us, stamp, sizeof stamp);
        used = append_contact(body, sizeof body, used, stamp, &contacts[i]);
        if (used >= (int)sizeof body - 4) {
            /* Does not fit. Send what is packed and leave the rest to the caller's next
             * pass rather than silently truncating the burst. */
            used = before;
            break;
        }
        packed++;
    }
    used += snprintf(body + used, sizeof body - used, "]}");
    if (packed == 0 || used >= (int)sizeof body) {
        ESP_LOGE(TAG, "batch did not fit; dropping %d", count);
        failed_count += (uint32_t)count;
        return;
    }

    if (post_json(CIELOTRACK_SERVER_URL "/v1/detections/batch", body)) {
        sent_count += (uint32_t)packed;
        ESP_LOGI(TAG, "reported %d contact(s) in one request (%" PRIu32 " sent)",
                 packed, sent_count);
    } else {
        failed_count += (uint32_t)packed;
        ESP_LOGW(TAG, "batch of %d failed (%" PRIu32 " failed)", packed, failed_count);
    }
    if (packed < count) {
        /* The remainder was not attempted; count it so the shortfall is never silent. */
        failed_count += (uint32_t)(count - packed);
        ESP_LOGW(TAG, "%d contact(s) did not fit this batch", count - packed);
    }
}

void uplink_report(const char *mac, const char *uas_id, const char *ua_type,
                   double lat, double lon, double altitude_m, double speed_mps,
                   int rssi_dbm, int message_count, bool inferred) {
    if (!uplink_ready()) {
        failed_count++;
        return;
    }

    int64_t wall_us, mono_us;
    clocks_now(&wall_us, &mono_us);
    char stamp[32];
    contact_stamp(wall_us, mono_us, 0, stamp, sizeof stamp);

    char body[512];
    int used = snprintf(body, sizeof body,
                        "{\"detected_at\":\"%s\",\"protocol\":\"%s\"",
                        stamp, CIELOTRACK_PROTOCOL);
    used = append_string(body, sizeof body, used, "mac", mac);
    used = append_string(body, sizeof body, used, "uas_id", uas_id);
    used = append_string(body, sizeof body, used, "ua_type", ua_type);
    used = append_number(body, sizeof body, used, "lat", lat);
    used = append_number(body, sizeof body, used, "lon", lon);
    used = append_number(body, sizeof body, used, "altitude_m", altitude_m);
    used = append_number(body, sizeof body, used, "speed_mps", speed_mps);
    used += snprintf(body + used, sizeof body - used,
                     ",\"rssi_dbm\":%d,\"message_count\":%d,"
                     "\"identity_source\":\"%s\"}", rssi_dbm, message_count,
                     inferred ? "inferred-from-mac" : "decoded");
    if (used >= (int)sizeof body) {
        ESP_LOGE(TAG, "payload did not fit; dropping");
        failed_count++;
        return;
    }

    if (!post_json(CIELOTRACK_SERVER_URL "/v1/detections", body)) {
        failed_count++;
        return;
    }
    sent_count++;
    ESP_LOGI(TAG, "reported %s (%" PRIu32 " sent)", mac ? mac : "?", sent_count);
}

void uplink_heartbeat(const char *status_json) {
    if (!uplink_ready()) {
        /* Connected but clockless is a recoverable state, and this is the periodic
         * call that both roles already make — so it is where the recovery belongs
         * rather than in each caller. */
        uplink_resync_clock();
        return;
    }

    /* The status goes under "status" because the server stores that object verbatim and
     * the fleet page reads its keys by name. A bare heartbeat is still accepted and
     * still proves liveness — the body is the difference between "it is alive" and
     * "here is what it can see", and only the first is required. */
    char body[512];
    int used = snprintf(body, sizeof body, "{\"status\":%s}", status_json);
    if (used >= (int)sizeof body) {
        ESP_LOGW(TAG, "status did not fit; sending a bare heartbeat");
        snprintf(body, sizeof body, "{}");
    }

    char url[160];
    snprintf(url, sizeof url, CIELOTRACK_SERVER_URL "/v1/devices/%s/heartbeat",
             UPLINK_DEVICE_ID());
    if (post_json(url, body)) {
        heartbeat_count++;
    } else {
        heartbeat_failed_count++;
        ESP_LOGW(TAG, "heartbeat failed (%" PRIu32 " failed)", heartbeat_failed_count);
    }
}

uint32_t uplink_sent(void) { return sent_count; }
uint32_t uplink_failed(void) { return failed_count; }
uint32_t uplink_heartbeats(void) { return heartbeat_count; }

/* --------------------------------------------------------------- peer forwarding --- */
#ifdef CIELOTRACK_PEER_DEVICE_ID

/* Contacts from the sensor, accumulated as raw JSON and flushed as one batch. Guarded
 * because the link task fills it and the reporting task drains it. */
static char peer_batch[UPLINK_BATCH_BODY_BYTES];
static int peer_used;
static int peer_count;
static SemaphoreHandle_t peer_lock;

static void peer_lock_init(void) {
    if (peer_lock == NULL) peer_lock = xSemaphoreCreateMutex();
}

void uplink_forward_peer_contact(const char *contact_json) {
    if (contact_json == NULL || contact_json[0] != '{') return;
    peer_lock_init();

    int64_t wall_us, mono_us;
    clocks_now(&wall_us, &mono_us);
    char stamp[32];
    contact_stamp(wall_us, mono_us, 0, stamp, sizeof stamp);

    if (xSemaphoreTake(peer_lock, pdMS_TO_TICKS(200)) != pdTRUE) return;
    /* The sensor's object with a timestamp and a transport spliced onto the front. It
     * has no clock of its own; this stamp is within milliseconds of the decode and is
     * the more trustworthy of the two numbers anyway. */
    int written = snprintf(peer_batch + peer_used, sizeof peer_batch - peer_used,
                           "%s{\"detected_at\":\"%s\",\"protocol\":\"Wi-Fi\",%s",
                           peer_count ? "," : "", stamp, contact_json + 1);
    if (written > 0 && peer_used + written < (int)sizeof peer_batch - 32) {
        peer_used += written;
        peer_count++;
    } else {
        /* Full. Dropped rather than truncated: half a contact is worse than none. */
        failed_count++;
    }
    xSemaphoreGive(peer_lock);
}

void uplink_flush_peer(void) {
    peer_lock_init();
    if (peer_count == 0 || !uplink_ready()) return;

    static char body[UPLINK_BATCH_BODY_BYTES + 64];
    int count;
    if (xSemaphoreTake(peer_lock, pdMS_TO_TICKS(500)) != pdTRUE) return;
    count = peer_count;
    snprintf(body, sizeof body, "{\"detections\":[%s]}", peer_batch);
    peer_used = 0;
    peer_count = 0;
    peer_batch[0] = '\0';
    xSemaphoreGive(peer_lock);

    /* The peer's key, not ours: these are that receiver's detections and must land on
     * that receiver's row. */
    if (post_json_as(CIELOTRACK_SERVER_URL "/v1/detections/batch", body,
                     CIELOTRACK_PEER_DEVICE_KEY)) {
        sent_count += (uint32_t)count;
        ESP_LOGI(TAG, "forwarded %d contact(s) from the sensor", count);
    } else {
        failed_count += (uint32_t)count;
        ESP_LOGW(TAG, "forwarding %d contact(s) failed", count);
    }
}

void uplink_peer_heartbeat(const char *status_json) {
    if (!uplink_ready()) return;
    char body[384];
    if (snprintf(body, sizeof body, "{\"status\":%s}", status_json) >= (int)sizeof body) {
        return;
    }
    char url[160];
    snprintf(url, sizeof url, CIELOTRACK_SERVER_URL "/v1/devices/%s/heartbeat",
             CIELOTRACK_PEER_DEVICE_ID);
    post_json_as(url, body, CIELOTRACK_PEER_DEVICE_KEY);
}

#endif /* CIELOTRACK_PEER_DEVICE_ID */

#endif /* !CIELOTRACK_SENSOR_ONLY — the station half of this file */
