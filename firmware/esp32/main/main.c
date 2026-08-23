/* CieloTrack ESP32 receiver: BLE 5 extended scanning for Remote ID.
 *
 * Scan parameters and the callback shape follow ESP-IDF's ble_50/periodic_sync example,
 * which is the reference for extended advertising on this chip. What is ours is the
 * part after "a report arrived": handing the payload to odid_ble_extract and then to
 * the same decoder the Pi runs and the host tests exercise.
 *
 * Deliberately does nothing else yet. No Wi-Fi, no uploading, no claim flow — this
 * stage answers one question, which is whether the radio hears Remote ID at all, and
 * mixing an unproven network path into that would make a silence ambiguous. That
 * ambiguity is what cost seventeen hours on the Pi.
 */
#include <inttypes.h>
#include <math.h>
#include <time.h>
#include <stdio.h>
#include <string.h>

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_log.h"
#include "claim.h"
#include "identity_cache.h"

#include "peer_link.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "odid_ble.h"
#include "odid_decode.h"
#include "secrets.h"
#if !defined(CIELOTRACK_SENSOR_ONLY)
#define CIELOTRACK_SENSOR_ONLY 0
#endif

#include "uplink.h"
#include "health.h"
#include "esp_timer.h"
#include "wifi_capture.h"

static const char *TAG = "cielotrack";

/* Comfortably inside the server's three-missed-intervals window (180s), so one dropped
 * heartbeat does not read as a dead receiver. Matching the server's own interval exactly
 * would make every transient network blip look like a fault. */
#define HEARTBEAT_EVERY_SECONDS 45

#if !CIELOTRACK_ROLE_WIFI
/* Contacts waiting to be reported. The BLE callback runs on the controller's task and a
 * TLS handshake takes hundreds of milliseconds, so posting from there would drop
 * advertisements while the socket worked — the radio would be blocked by the network,
 * which is precisely backwards for a receiver.
 *
 * Small on purpose. If the network is down, dropping the newest contact is the right
 * failure: a queue that grows until the heap is gone takes the capture down with it, and
 * losing detections is what happens either way. A real outbox that survives this is the
 * next slice; counting what was dropped is what stops it being invisible in the
 * meantime. */
/* Was 12, which held about fifty seconds of a busy pass while each contact took a
 * whole TLS handshake to deliver. Half of everything the board heard outdoors was
 * dropped. The depth matters far less than the batching that now drains it, but a
 * burst still has to land somewhere while the radio keeps up.
 *
 * Then 48, which still overflowed — rarely, and only in bursts. Measured from the
 * stored heartbeat history on 2026-08-21: three check-ins out of two hundred reported
 * drops, losing 1, 2 and 5 contacts within about two minutes of each other, 33 in the
 * board's whole uptime against 2031 messages decoded. Small, but each one is a contact
 * heard and then thrown away, which no other counter distinguishes from never-heard.
 *
 * A contact is 128 bytes, so this costs 6 KB more of heap and buys twice the room for a
 * burst to land in. Raising the batch size instead would drain faster but hold contacts
 * longer while a burst is still arriving, which is the trade the batch comment refuses.
 */
#define REPORT_QUEUE_DEPTH 96
/* How many go in one request. The server accepts 500; this is bounded by the body
 * buffer and by not sitting on contacts while a burst is still arriving.
 *
 * 16 drained too slowly for a multi-aircraft pass. On 2026-08-21 three aircraft were
 * overhead at once, peaking at thirteen contacts in a single second: the board recorded
 * 97 and dropped 39, losing about a third of what it heard. Depth alone does not fix
 * that — a deeper queue only buys time if the drain keeps up.
 *
 * 24 rather than 32, and the reason is measured rather than assumed. The comment above
 * once said a contact is "roughly 200 bytes"; the real ones are 272, sampled from forty
 * of this board's own rows. At 32 a full batch needs 8736 bytes against an 8192 buffer,
 * so it would pack short and waste the loop. Raising UPLINK_BATCH_BODY_BYTES would cost
 * twice what it looks like, because peer_batch is declared with the same constant.
 */
#define REPORT_BATCH_MAX 24


static QueueHandle_t reports;
static uint32_t reports_dropped;
static uint32_t reports_seen;
static uint32_t messages_decoded;

/* What this board can see, in the shape the fleet page reads.
 *
 * stream_alive is measured, not asserted: it is true only if the controller has handed
 * over at least one advertising report since the last heartbeat. 2.4 GHz advertising is
 * constant everywhere, so a scanner that has gone quiet has stopped working — that is
 * the one BLE fault a receiver cannot otherwise notice about itself, because a dead
 * radio and an empty sky produce identical detection counts. */
static void send_heartbeat(void) {
    static uint32_t reports_at_last_heartbeat;
    static uint32_t drops_at_last_heartbeat;
    bool stream_alive = health_counter_moved(&reports_at_last_heartbeat, reports_seen);
    /* Drops since the last check-in, not since boot.
     *
     * reports_dropped never resets, so flagging on its total meant one busy pass marked
     * the receiver degraded for the rest of its uptime: a board that dropped nineteen
     * contacts during a dense pass still reported the fault hours later with an empty
     * queue and a healthy radio. A badge that latches on is a badge you stop reading.
     *
     * The lifetime total stays in the payload. Losing it was never the point — a
     * dropped contact is heard-and-lost, which no other counter distinguishes from
     * never-heard, and that remains just as true. What changes is only which of the two
     * drives the fault flag: how it is now, in the same tense as stream_alive above. */
    bool dropping = health_counter_moved(&drops_at_last_heartbeat, reports_dropped);

    char status[320];
    snprintf(status, sizeof status,
             "{\"version\":\"%s\",\"ble\":{\"mode\":\"extended\",\"stream_alive\":%s},"
             "\"reports_seen\":%" PRIu32 ",\"messages_decoded\":%" PRIu32 ","
             "\"outbox_pending\":%u,\"detections_dropped\":%" PRIu32 ",\"problems\":[%s]}",
             CIELOTRACK_FIRMWARE_VERSION, stream_alive ? "true" : "false",
             reports_seen, messages_decoded,
             (unsigned)uxQueueMessagesWaiting(reports), reports_dropped,
             (!stream_alive ? "\"ble_stream_down\""
                            : (dropping ? "\"detections_not_queued\"" : "")));
    uplink_heartbeat(status);
}

#ifdef CIELOTRACK_PEER_DEVICE_ID
/* The sensor's health, reported on its behalf, because it has no network of its own.
 *
 * link_alive is the honest part: it is false until the sensor has actually been heard
 * from, so an unplugged wire shows as a problem rather than as a receiver that happens
 * to be having a quiet day. link_errors counts lines this end threw away — a wire
 * corrupting one line in ten would otherwise look exactly like an empty sky. */
static void send_peer_heartbeat(void) {
    uint32_t frames = 0, decoded = 0, dropped = 0, errors = 0, channel = 0;
    peer_link_peer_status(&frames, &decoded, &dropped, &errors, &channel);
    bool alive = peer_link_sensor_alive();

    char status[320];
    snprintf(status, sizeof status,
             "{\"version\":\"%s\",\"wifi\":{\"mode\":\"promiscuous\",\"channel\":%" PRIu32 "},"
             "\"frames_seen\":%" PRIu32 ",\"messages_decoded\":%" PRIu32 ","
             "\"detections_dropped\":%" PRIu32 ",\"link_errors\":%" PRIu32 ","
             "\"outbox_pending\":0,\"problems\":[%s]}",
             CIELOTRACK_FIRMWARE_VERSION, channel,
             frames, decoded, dropped, errors,
             alive ? "" : "\"peer_link_down\"");
    uplink_peer_heartbeat(status);
}
#endif

static void reporting_task(void *arg) {
    (void)arg;
    static uplink_contact_t batch[REPORT_BATCH_MAX];
    /* Waits on the queue with a timeout rather than forever, so the heartbeat happens
     * on a schedule even when nothing is heard — which is the case it exists for. The
     * server treats a receiver as offline after three missed intervals, so the interval
     * must be comfortably shorter than that window, not equal to it. */
    TickType_t last = xTaskGetTickCount() - pdMS_TO_TICKS(HEARTBEAT_EVERY_SECONDS * 1000);
    for (;;) {
        int count = 0;
        /* Block for the first, then take whatever else is already waiting. During a
         * pass that is the rest of the burst; when the sky is empty it is just the one,
         * and nothing waits on a batch that will never fill. */
        if (xQueueReceive(reports, &batch[0], pdMS_TO_TICKS(1000)) == pdTRUE) {
            count = 1;
            while (count < REPORT_BATCH_MAX &&
                   xQueueReceive(reports, &batch[count], 0) == pdTRUE) {
                count++;
            }
            uplink_report_batch(batch, count);
        }
#ifdef CIELOTRACK_PEER_DEVICE_ID
        /* Anything the sensor pushed down the wire since the last pass. Sent from here
         * rather than from the link task so a TLS handshake can never hold up the
         * reader and back the wire up. */
        uplink_flush_peer();
#endif
        if (xTaskGetTickCount() - last >= pdMS_TO_TICKS(HEARTBEAT_EVERY_SECONDS * 1000)) {
            last = xTaskGetTickCount();
            send_heartbeat();
#ifdef CIELOTRACK_PEER_DEVICE_ID
            send_peer_heartbeat();
#endif
        }
    }
}

static void enqueue_report(const char *mac, const char *uas_id, const char *ua_type,
                           double lat, double lon, double altitude_m, double speed_mps,
                           int rssi, bool inferred) {
    if (reports == NULL) return;
    uplink_contact_t item = { .lat = lat, .lon = lon, .altitude_m = altitude_m,
                              .speed_mps = speed_mps, .rssi_dbm = rssi,
                              .inferred = inferred,
                              /* Here, not at upload: this is the moment it was heard,
                               * and the queue between the two can be seconds deep. */
                              .decoded_us = esp_timer_get_time() };
    snprintf(item.mac, sizeof item.mac, "%s", mac ? mac : "");
    snprintf(item.uas_id, sizeof item.uas_id, "%s", uas_id ? uas_id : "");
    snprintf(item.ua_type, sizeof item.ua_type, "%s", ua_type ? ua_type : "");
    if (xQueueSend(reports, &item, 0) != pdTRUE) {
        reports_dropped++;
    }
}
#endif /* !CIELOTRACK_ROLE_WIFI */

/* Counted the way the Pi counts them, and for the same reason: frames seen must be
 * separate from messages decoded, or a dead radio and an empty sky read identically.
 * Remote ID is rare and ordinary 2.4 GHz advertising is not, so a live radio shows
 * reports climbing whether or not anything is flying. */
#if !CIELOTRACK_ROLE_WIFI

/* Coded PHY scanning — Bluetooth 5 Long Range. Off, and measured rather than assumed.
 *
 * There is one radio. Asking it for both PHYs at full duty makes the controller
 * alternate between them, so half the listening time goes to a PHY that nothing here
 * transmits on. We know nothing local does, because a receiver that fell back to legacy
 * scanning — 1M only — still heard aircraft.
 *
 * The board was catching one message from passes where a Pi beside it caught twelve,
 * and it looked like an antenna problem. It was not. Both configurations, same bench,
 * same sky:
 *
 *     both PHYs   65 min ·   358,500 advertisements ·  4 decodes  ~ 3.7/hour
 *     1M only    102 min · 1,127,500 advertisements · 31 decodes  ~  18/hour
 *
 * Five times the decode rate, and three times the advertisements seen per minute,
 * which is the dwell time coming back.
 *
 * Turn it on where Coded PHY traffic actually exists: the cost of scanning for it is
 * everything above, and the cost of not scanning is missing long-range broadcasts
 * entirely. Neither is a default that suits everywhere, so the log line says which one
 * is in force rather than leaving it to be inferred. Passive throughout — this
 * receives, it never transmits. */
#define CIELOTRACK_SCAN_CODED_PHY 0

static esp_ble_ext_scan_params_t ext_scan_params = {
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
    .scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE,
#if CIELOTRACK_SCAN_CODED_PHY
    .cfg_mask = ESP_BLE_GAP_EXT_SCAN_CFG_UNCODE_MASK | ESP_BLE_GAP_EXT_SCAN_CFG_CODE_MASK,
    .uncoded_cfg = { BLE_SCAN_TYPE_PASSIVE, 80, 80 },
    .coded_cfg   = { BLE_SCAN_TYPE_PASSIVE, 80, 80 },
#else
    .cfg_mask = ESP_BLE_GAP_EXT_SCAN_CFG_UNCODE_MASK,
    .uncoded_cfg = { BLE_SCAN_TYPE_PASSIVE, 80, 80 },
#endif
};

static void report_detection(const uint8_t *addr, int rssi, const uint8_t *message) {
    char mac[18];
    snprintf(mac, sizeof mac, "%02X:%02X:%02X:%02X:%02X:%02X",
             addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);

    switch (odid_message_type(message)) {
    case 0x0: {
        odid_basic_id_t id = odid_decode_basic_id(message);
        ESP_LOGI(TAG, "%s  BasicID  uas_id=%s  ua_type=%u  rssi=%d",
                 mac, id.valid ? id.uas_id : "N/A", id.ua_type, rssi);
        if (id.valid) {
            char type[24];
            /* The label, not the number — every other receiver in the fleet reports
             * a class name, and one row reading "4" beside another reading "Hybrid
             * Lift" for the same aircraft is the fleet disagreeing with itself. */
            snprintf(type, sizeof type, "%s", odid_ua_type_label(id.ua_type));
            identity_remember(mac, id.uas_id, type, (uint32_t)time(NULL));
            enqueue_report(mac, id.uas_id, type, NAN, NAN, NAN, NAN, rssi, false);
        }
        break;
    }
    case 0x1: {
        odid_location_t loc = odid_decode_location(message);
        if (!loc.valid) {
            /* 0,0 is the spec's "no fix" sentinel. Saying so beats plotting the
             * Atlantic. */
            ESP_LOGI(TAG, "%s  Location no fix  rssi=%d", mac, rssi);
            break;
        }
        char altitude[32] = "n/a";
        if (loc.has_altitude) {
            snprintf(altitude, sizeof altitude, "%.1fm %s", loc.altitude_m,
                     loc.altitude_ref == ODID_ALT_AGL ? "agl" : "abs");
        }
        char height[32] = "n/a";
        if (loc.has_height) {
            snprintf(height, sizeof height, "%.1fm %s", loc.height_m,
                     loc.height_ref == ODID_HEIGHT_GROUND ? "ground" : "takeoff");
        }
        ESP_LOGI(TAG, "%s  Location %.7f,%.7f  alt=%s  height=%s  %.1fm/s  %ddeg  rssi=%d",
                 mac, loc.lat, loc.lon, altitude, height,
                 loc.speed_mps, loc.direction_deg, rssi);
        /* A Location message says where but never who — BLE rotates one message per
         * advertisement. Without the transmitter's last known identity attached, every
         * positioned row reaches the map as "Unknown Aircraft" while its serial sits in
         * a different row that never gets drawn. */
        char recalled_id[24] = "", recalled_type[24] = "";
        bool inferred = identity_recall(mac, recalled_id, sizeof recalled_id,
                                        recalled_type, sizeof recalled_type,
                                        (uint32_t)time(NULL));
        enqueue_report(mac, inferred ? recalled_id : NULL,
                       inferred ? recalled_type : NULL, loc.lat, loc.lon,
                       loc.has_altitude ? loc.altitude_m : NAN, loc.speed_mps, rssi,
                       inferred);
        break;
    }
    case 0x4: {
        odid_system_t sys = odid_decode_system(message);
        if (!sys.valid) break;
        ESP_LOGI(TAG, "%s  System operator %.7f,%.7f  type=%d",
                 mac, sys.operator_lat, sys.operator_lon,
                 (int)sys.operator_location_type);
        break;
    }
    default:
        ESP_LOGD(TAG, "%s  message type 0x%X not decoded", mac, odid_message_type(message));
    }
}

static void gap_callback(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
    switch (event) {
    case ESP_GAP_BLE_SET_EXT_SCAN_PARAMS_COMPLETE_EVT:
        if (param->set_ext_scan_params.status != ESP_BT_STATUS_SUCCESS) {
            /* This is the failure that has to be loud. Extended scan parameters being
             * refused is how a receiver ends up quietly listening on legacy only,
             * hearing almost nothing, and looking fine. */
            ESP_LOGE(TAG, "extended scan parameters REFUSED (status %d) — this receiver "
                          "would miss most aircraft", param->set_ext_scan_params.status);
            break;
        }
        /* Derived from the configuration rather than written out beside it, because a
         * log line that describes what the code used to do is worse than none: it is
         * the thing someone reads when they are trying to work out why a receiver is
         * quiet, and it will tell them the radio is covering a PHY it is not. */
        ESP_LOGI(TAG, "extended scan parameters accepted (%s)",
                 (ext_scan_params.cfg_mask & ESP_BLE_GAP_EXT_SCAN_CFG_CODE_MASK)
                     ? ((ext_scan_params.cfg_mask & ESP_BLE_GAP_EXT_SCAN_CFG_UNCODE_MASK)
                            ? "1M + Coded PHY" : "Coded PHY only")
                     : "1M PHY only, no Long Range coverage");
        esp_ble_gap_start_ext_scan(0, 0);      /* 0,0 = scan until stopped */
        break;

    case ESP_GAP_BLE_EXT_SCAN_START_COMPLETE_EVT:
        if (param->ext_scan_start.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(TAG, "extended scan failed to start (status %d)",
                     param->ext_scan_start.status);
            break;
        }
        ESP_LOGI(TAG, "scanning for Remote ID");
        break;

    case ESP_GAP_BLE_EXT_ADV_REPORT_EVT: {
        const esp_ble_gap_ext_adv_report_t *r = &param->ext_adv_report.params;
        reports_seen++;

        uint8_t message[ODID_MESSAGE_SIZE];
        /* One report's own payload, never a batch: pairing a payload with the wrong
         * advertiser attributes one aircraft's telemetry to another's address. */
        if (odid_ble_extract(r->adv_data, r->adv_data_len, message)) {
            messages_decoded++;
            report_detection(r->addr, r->rssi, message);
        }

        /* Progress on a quiet sky, so silence can be told from a stopped radio. */
        if (reports_seen % 500 == 0) {
            ESP_LOGI(TAG, "%" PRIu32 " advertisements seen, %" PRIu32 " carried Remote ID, "
                     "%" PRIu32 " reported, %" PRIu32 " failed, %" PRIu32 " dropped",
                     reports_seen, messages_decoded, uplink_sent(), uplink_failed(),
                     reports_dropped);
        }
        break;
    }

    default:
        break;
    }
}

#endif /* !CIELOTRACK_ROLE_WIFI */

#if !CIELOTRACK_ROLE_WIFI
/* Brings the controller up for scanning, or adopts it if provisioning already did.
 *
 * esp_bt_controller_init on an initialised controller is an error, not a no-op, so the
 * state has to be asked for rather than assumed — a board that has just provisioned
 * arrives here with Bluetooth already running, and a board with credentials in NVS
 * arrives here with it untouched. Both must end up scanning. */
static void bring_up_bluetooth_for_scanning(void) {
    if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE) {
        ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));
        esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    }
    if (esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_ENABLED) {
        ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));
    }
    if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_UNINITIALIZED) {
        ESP_ERROR_CHECK(esp_bluedroid_init());
    }
    if (esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_ENABLED) {
        ESP_ERROR_CHECK(esp_bluedroid_enable());
    }
    ESP_ERROR_CHECK(esp_ble_gap_register_callback(gap_callback));
}
#endif

void app_main(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);


#if !CIELOTRACK_ROLE_WIFI
    reports = xQueueCreate(REPORT_QUEUE_DEPTH, sizeof(uplink_contact_t));
    /* Its own task at a low priority: reporting is the least urgent thing here and must
     * never preempt the radio. 6 kB because TLS needs the room. */
    xTaskCreate(reporting_task, "cielotrack_report", 6144, NULL, 3, NULL);
#endif
    /* Before the scanner's Bluetooth, not after.
     *
     * Provisioning talks to the phone over BLE, so it needs the controller — and on this
     * board the controller was already up for scanning, which meant protocomm would have
     * been initialising a stack that was in use. Worse, the scheme handler frees the
     * controller's memory when provisioning ends, and esp_bt_mem_release cannot be
     * undone: the board would have finished provisioning and then had no radio to do its
     * actual job with. That path was written and tested for a Wi-Fi-only board and would
     * have failed the first time anyone provisioned the BLE one.
     *
     * So uplink_start() runs first, provisioning with it, and the scanner takes over
     * whatever Bluetooth state is left afterwards. */
    uplink_start();

#if !CIELOTRACK_ROLE_WIFI
    bring_up_bluetooth_for_scanning();
#endif
#if !defined(CIELOTRACK_DEVICE_KEY) && !CIELOTRACK_SENSOR_ONLY
    /* Asks the server for this board's identity. On its own task, because a board
     * waiting to be claimed by a human may wait a long time and must keep capturing.
     * Never on the sensor: it has no network, and its rows reach the server under the
     * master's roof using the identity the master already holds. */
    claim_start();
#endif

    ESP_LOGI(TAG, "CieloTrack receiver starting");

#if CIELOTRACK_ROLE_WIFI
    /* No BLE at all on this board. The Bluetooth controller is never initialised, so it
     * is not merely idle — it cannot contend for the radio, which is the entire reason
     * for splitting the two transports across two boards. */
    ESP_LOGI(TAG, "role: Wi-Fi Remote ID");
    /* Held only now, after uplink_start has a network — during provisioning the station
     * must be free to associate, and at boot the first association is what sets the
     * clock. wifi_capture releases it for each upload window and takes it back. */
    uplink_hold_station(true);
    wifi_capture_start();
#else
    ESP_LOGI(TAG, "role: BLE Remote ID");
#ifdef CIELOTRACK_PEER_DEVICE_ID
    peer_link_master_start();
#endif
    ESP_ERROR_CHECK(esp_ble_gap_set_ext_scan_params(&ext_scan_params));
#endif
}
