#include "wifi_capture.h"

#include <inttypes.h>
#include <math.h>
#include <time.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "identity_cache.h"
#include "odid_decode.h"
#include "secrets.h"

#if !defined(CIELOTRACK_SENSOR_ONLY)
#define CIELOTRACK_SENSOR_ONLY 0
#endif
#include "peer_link.h"
#include "uplink.h"
#include "health.h"
#include "esp_timer.h"
#include "wifi_rid.h"

static const char *TAG = "wifi_rid";

/* Where Remote ID actually is. Parking beats rotating when you know where to park:
 * rotating would spend two thirds of the duty cycle on channels nothing here has ever
 * used, and it would cost it worst on the brief transits that are hardest to catch. */
#ifndef CIELOTRACK_RID_CHANNEL
#define CIELOTRACK_RID_CHANNEL 6
#endif

/* How long to listen between uploads.
 *
 * Was 60s, which turned out to be the access point's problem rather than ours. Every
 * cycle is a full disassociate and rejoin, and at 60s that is 45 associations an hour;
 * after about 45 minutes this mesh stopped completing the WPA2 handshake and six
 * consecutive upload windows failed while the board captured perfectly and read offline
 * on the dashboard. Nothing was wrong with the board, the network, or the credentials —
 * only with how often it was asking.
 *
 * At 300s that is 12 an hour, and the blind window drops from about 11% of the cycle to
 * under 3% as a side effect. What it costs is latency: a detection can now wait up to
 * five minutes before anyone sees it, which is the honest price of sharing one radio
 * between listening and talking. The queue holds 48 contacts, so nothing is lost unless
 * more than that arrives inside one cycle.
 *
 * The server must agree about this, or a board reporting every five minutes reads as
 * offline against a three-minute window — see heartbeat_interval_seconds. */
#ifndef CIELOTRACK_UPLOAD_EVERY_SECONDS
#define CIELOTRACK_UPLOAD_EVERY_SECONDS 300
#endif

#define CAPTURE_QUEUE_DEPTH 48
#define CAPTURE_BATCH_MAX 16

/* 15s was too tight and it was a guess. Measured on this network: a lease took 13.1s on
 * one cycle and never arrived on two others, so the budget was failing cycles that were
 * about to succeed. With a static address the join finishes in well under a second and
 * this bound stops mattering — it is here for the DHCP case, which is still the default
 * for anyone who has not set an address. */
#define JOIN_TIMEOUT_MS 25000


#if !CIELOTRACK_SENSOR_ONLY
static uplink_contact_t queue[CAPTURE_QUEUE_DEPTH];
static SemaphoreHandle_t queue_lock;
#endif
static int queued;   /* still reported: a sensor's queue is simply always empty */
static uint32_t frames_seen, messages_decoded, dropped;

#if !CIELOTRACK_SENSOR_ONLY
static void remember(const uplink_contact_t *contact) {
    if (xSemaphoreTake(queue_lock, 0) != pdTRUE) { dropped++; return; }
    if (queued < CAPTURE_QUEUE_DEPTH) {
        queue[queued++] = *contact;
    } else {
        /* The newest goes, as on the BLE board. A queue that grows until the heap is
         * gone takes capture down with it, and losing detections is what happens either
         * way — this way it is counted. */
        dropped++;
    }
    xSemaphoreGive(queue_lock);
}
#endif

/* Called from the Wi-Fi driver's task for every frame on the channel. Does as little as
 * possible: find the payload, split the pack, decode, queue. No network, no logging per
 * frame — this runs thousands of times a minute and anything expensive here is
 * backpressure on the radio. */
static void on_frame(void *buffer, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA) return;
    const wifi_promiscuous_pkt_t *packet = (wifi_promiscuous_pkt_t *)buffer;
    const uint8_t *frame = packet->payload;
    size_t length = packet->rx_ctrl.sig_len;
    frames_seen++;

    const uint8_t *payload = NULL;
    size_t payload_length = 0;
    if (!wifi_rid_payload(frame, length, &payload, &payload_length)) return;

    const uint8_t *messages[ODID_PACK_MAX_MESSAGES];
    int count = odid_pack_split(payload, payload_length, messages, ODID_PACK_MAX_MESSAGES);
    /* A single message rather than a pack is legal on Wi-Fi too, even if rare. */
    if (count == 0 && payload_length >= ODID_MESSAGE_SIZE) {
        messages[0] = payload;
        count = 1;
    }
    if (count == 0) return;

    uint8_t mac[6];
    if (!wifi_rid_transmitter(frame, length, mac)) return;

    uplink_contact_t contact = { .lat = NAN, .lon = NAN, .altitude_m = NAN,
                          .height_m = NAN, .speed_mps = NAN,
                          .rssi_dbm = packet->rx_ctrl.rssi, .message_count = count,
                          .decoded_us = esp_timer_get_time() };
    snprintf(contact.mac, sizeof contact.mac, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    /* One frame, one contact: a pack carries Basic ID and Location together, and they
     * describe the same aircraft at the same instant. Merging them here is what makes a
     * Wi-Fi row richer than a BLE one, where the types arrive seconds apart. */
    bool anything = false;
    for (int i = 0; i < count; i++) {
        switch (odid_message_type(messages[i])) {
        case 0x0: {
            odid_basic_id_t id = odid_decode_basic_id(messages[i]);
            if (id.valid) {
                snprintf(contact.uas_id, sizeof contact.uas_id, "%s", id.uas_id);
                snprintf(contact.ua_type, sizeof contact.ua_type, "%s",
                         odid_ua_type_label(id.ua_type));
                anything = true;
            }
            break;
        }
        case 0x1: {
            odid_location_t loc = odid_decode_location(messages[i]);
            if (loc.valid) {
                contact.lat = loc.lat;
                contact.lon = loc.lon;
                if (loc.has_altitude) {
                    contact.altitude_m = loc.altitude_m;
                    /* The reference goes with it — a height above takeoff read as absolute
                     * lands below the terrain and renders at zero. "absolute" is the
                     * server's word, not the "abs" used elsewhere for the console. */
                    snprintf(contact.altitude_ref, sizeof contact.altitude_ref, "%s",
                             loc.altitude_ref == ODID_ALT_AGL ? "agl" : "absolute");
                }
                if (loc.has_height) {
                    contact.height_m = loc.height_m;
                    snprintf(contact.height_ref, sizeof contact.height_ref, "%s",
                             loc.height_ref == ODID_HEIGHT_GROUND ? "ground" : "takeoff");
                }
                contact.speed_mps = loc.speed_mps;
                anything = true;
            }
            break;
        }
        default:
            break;
        }
    }
    if (!anything) return;

    /* Same reasoning as the BLE path. A pack normally carries identity and position
     * together, so this rarely fires — but a lone Location frame is legal, and when one
     * arrives it should reach the map named rather than as "Unknown Aircraft". */
    if (contact.uas_id[0] != '\0') {
        identity_remember(contact.mac, contact.uas_id, contact.ua_type,
                          (uint32_t)time(NULL));
    } else if (identity_recall(contact.mac, contact.uas_id, sizeof contact.uas_id,
                               contact.ua_type, sizeof contact.ua_type,
                               (uint32_t)time(NULL))) {
        contact.inferred = true;
    }

    messages_decoded++;
#if CIELOTRACK_SENSOR_ONLY
    /* Straight down the wire. No queue, because there is nothing to wait for: the
     * master is always on the network, so a contact is delivered in milliseconds
     * rather than held for an upload window that may be five minutes away. */
    peer_link_send_contact(&contact);
#else
    remember(&contact);
#endif
}

static void listen_on_rid_channel(void) {
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(false));
    ESP_ERROR_CHECK(esp_wifi_set_channel(CIELOTRACK_RID_CHANNEL, WIFI_SECOND_CHAN_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));
}

#if !CIELOTRACK_SENSOR_ONLY
static void drain_queue(void) {
    /* Static, not on the stack: a full queue is ~4.5 kB of contacts and this task has
     * 6 kB in total. Only capture_task ever calls this, so one copy is enough. */
    static uplink_contact_t batch[CAPTURE_QUEUE_DEPTH];
    int count = 0;
    if (xSemaphoreTake(queue_lock, pdMS_TO_TICKS(1000)) == pdTRUE) {
        count = queued;
        memcpy(batch, queue, sizeof(uplink_contact_t) * (size_t)count);
        queued = 0;
        xSemaphoreGive(queue_lock);
    }
    if (count == 0) return;

    /* Taken off the queue before the upload, not after: a send that fails loses these
     * contacts rather than blocking the next cycle's capture behind them. That is the
     * honest trade for a queue with no storage behind it, and the reason the next slice
     * is an outbox. */
    /* In batches rather than one request each. The upload window is bounded at 25s and
     * a TLS handshake per contact would have blown straight through it on a full queue
     * — the same failure the BLE board was actually suffering. */
    for (int sent = 0; sent < count; sent += CAPTURE_BATCH_MAX) {
        int n = count - sent;
        if (n > CAPTURE_BATCH_MAX) n = CAPTURE_BATCH_MAX;
        uplink_report_batch(&batch[sent], n);
    }
}
#endif

#if !CIELOTRACK_SENSOR_ONLY
/* Leaves the Remote ID channel and joins the network. True once there is an address and
 * a clock — both, because a detection stamped 1970 is worse than one that never arrived.
 * The caller is blind to Remote ID for however long this takes, which is why every
 * caller passes a bounded wait. */
static bool join_network(int timeout_ms) {
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(false));
    uplink_hold_station(false);
    int waited = 0;
    bool asked_for_time = false;
    while (!uplink_ready() && waited < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(250));
        waited += 250;
        /* An address without a clock is the one failure that does not fix itself:
         * SNTP's own retry is an hour away, so a board whose boot sync missed would
         * hold a working connection every cycle and still refuse to send. Ask again
         * the moment there is a network to ask over. */
        if (!asked_for_time && uplink_connected()) {
            asked_for_time = true;
            uplink_resync_clock();
        }
    }
    return uplink_ready();
}
#endif

#if !CIELOTRACK_SENSOR_ONLY
static void leave_network(void) {
    /* Held before the disconnect, not after: releasing the network and then asking to
     * leave it races the reconnect handler, and losing that race means the radio follows
     * the access point instead of coming back to the Remote ID channel. */
    uplink_hold_station(true);
    esp_wifi_disconnect();
    listen_on_rid_channel();
}
#endif

#if !CIELOTRACK_SENSOR_ONLY
/* What this board can see, in the shape the fleet page reads.
 *
 * outbox_pending is the capture queue: it is the same claim the Pi's makes — contacts
 * heard but not yet delivered — and a number that climbs is the first sign the upload
 * leg is failing while capture is fine.
 *
 * detections_not_queued reports drops since the last check-in rather than since boot.
 * The counter it reads never resets, so flagging on the total left a board that
 * overflowed once during a dense pass reporting the fault for the rest of its uptime.
 * The lifetime total is still in the payload, which is what made a dropped contact
 * distinguishable from a contact never heard; only the fault flag now speaks in the
 * present tense. */
static void send_heartbeat(void) {
    static uint32_t drops_at_last_heartbeat;
    bool dropping = health_counter_moved(&drops_at_last_heartbeat, dropped);

    char status[320];
    snprintf(status, sizeof status,
             "{\"version\":\"%s\",\"wifi\":{\"mode\":\"promiscuous\",\"channel\":%d},"
             "\"frames_seen\":%" PRIu32 ",\"messages_decoded\":%" PRIu32 ","
             "\"outbox_pending\":%d,\"detections_dropped\":%" PRIu32 ",\"problems\":[%s]}",
             CIELOTRACK_FIRMWARE_VERSION, CIELOTRACK_RID_CHANNEL,
             frames_seen, messages_decoded, queued, dropped,
             dropping ? "\"detections_not_queued\"" : "");
    uplink_heartbeat(status);
}
#endif

static void capture_task(void *arg) {
    (void)arg;
#if CIELOTRACK_SENSOR_ONLY
    peer_link_sensor_start();
    listen_on_rid_channel();
    ESP_LOGI(TAG, "sensor: listening on channel %d, reporting over the wire",
             CIELOTRACK_RID_CHANNEL);
#else

    /* One join before any capture, for the clock. SNTP starts at boot and retries only
     * once an hour, so with the station held from the start it would never get a network
     * to ask — and uplink_ready() would still be false the first time this board had
     * something to say, leaving real detections stuck behind a clock that had no chance
     * to set itself. Doing it here also means the whole hop-and-return path is exercised
     * and logged at every boot, rather than first running unattended at the moment it
     * matters. */
    if (join_network(30000)) {
        ESP_LOGI(TAG, "clock set and uplink reachable; capture can report");
    } else {
        ESP_LOGW(TAG, "no clock after 30s — will keep trying at each upload window; "
                      "capture starts regardless");
    }
    leave_network();

    ESP_LOGI(TAG, "listening on channel %d; uploading every %ds",
             CIELOTRACK_RID_CHANNEL, CIELOTRACK_UPLOAD_EVERY_SECONDS);
#endif

#if CIELOTRACK_SENSOR_ONLY
    /* Never leaves the channel. Everything the alternating design existed to work
     * around — the rejoins, the handshake failures, the clock that could not set
     * itself, the widened online window on the server — is gone with the association
     * it was working around. */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(PEER_STATUS_EVERY_SECONDS * 1000));
        ESP_LOGI(TAG, "%lu frames, %lu decoded, %lu dropped (sensor; link to master)",
                 (unsigned long)frames_seen, (unsigned long)messages_decoded,
                 (unsigned long)dropped);
        peer_link_send_status(frames_seen, messages_decoded, dropped);
    }
#else
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(CIELOTRACK_UPLOAD_EVERY_SECONDS * 1000));

        /* Every cycle, whether or not anything was heard. A silent log and a dead radio
         * are the same thing to anyone reading it, so frames climbing with decodes at
         * zero has to be visible as a distinct state: that is a working receiver over an
         * empty sky, and it is the common case. */
        ESP_LOGI(TAG, "%lu frames, %lu decoded, %d queued, %lu dropped",
                 (unsigned long)frames_seen, (unsigned long)messages_decoded,
                 queued, (unsigned long)dropped);
        /* Every cycle now, not only when there is something to send. A board that
         * joins the network only when it hears a drone cannot prove it is alive during
         * the hours it hears nothing — which is most of them — and that is the exact
         * gap the heartbeat exists to close. It also means the cost of this design is
         * paid at a constant, measurable rate rather than in bursts that correlate with
         * the very traffic being measured. */
        ESP_LOGI(TAG, "leaving channel %d; %d contact(s) to send",
                 CIELOTRACK_RID_CHANNEL, queued);
        if (join_network(JOIN_TIMEOUT_MS)) {
            /* Heartbeat first. If the queue drain is slow or fails, liveness has
             * already been recorded, and a receiver that looks dead while it is busy
             * uploading is the wrong way round. */
            send_heartbeat();
            drain_queue();
        } else {
            ESP_LOGW(TAG, "no uplink in %ds; keeping the queue for next time",
                     JOIN_TIMEOUT_MS / 1000);
        }
        leave_network();
    }
#endif
}

void wifi_capture_start(void) {
#if !CIELOTRACK_SENSOR_ONLY
    queue_lock = xSemaphoreCreateMutex();
#endif
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(&on_frame));
    xTaskCreate(capture_task, "cielotrack_wifi", 6144, NULL, 4, NULL);
}

uint32_t wifi_capture_frames(void) { return frames_seen; }
uint32_t wifi_capture_decoded(void) { return messages_decoded; }
uint32_t wifi_capture_queued(void) { return (uint32_t)queued; }
uint32_t wifi_capture_dropped(void) { return dropped; }
