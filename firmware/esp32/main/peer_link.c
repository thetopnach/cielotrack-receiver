#include "peer_link.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "secrets.h"

#ifndef CIELOTRACK_RID_CHANNEL
#define CIELOTRACK_RID_CHANNEL 6
#endif

static const char *TAG = "peer";

/* Plain CRC-32, the same polynomial zlib uses, computed without a table: this runs once
 * per contact, not per byte of a stream, and 256 words of table is not worth the flash
 * for the handful of lines a minute this link carries. */
static uint32_t crc32(const char *data, size_t length) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < length; i++) {
        crc ^= (uint8_t)data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
        }
    }
    return ~crc;
}

static void link_open(void) {
    const uart_config_t config = {
        .baud_rate = PEER_LINK_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(PEER_LINK_UART_NUM, PEER_LINK_MAX_LINE * 4,
                                        PEER_LINK_MAX_LINE * 4, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(PEER_LINK_UART_NUM, &config));
    ESP_ERROR_CHECK(uart_set_pin(PEER_LINK_UART_NUM, PEER_LINK_TX_GPIO, PEER_LINK_RX_GPIO,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
}

/* ---------------------------------------------------------------- sensor side --- */

static void send_line(const char *kind, const char *json) {
    char line[PEER_LINK_MAX_LINE];
    int used = snprintf(line, sizeof line, "%s %08" PRIx32 " %s\n",
                        kind, crc32(json, strlen(json)), json);
    if (used >= (int)sizeof line) {
        ESP_LOGW(TAG, "line too long; dropping");
        return;
    }
    /* Bounded wait, not forever: the capture callback must never be held up by a wire.
     * A master that is wedged or unplugged costs contacts, and that is preferable to a
     * sensor that stops listening because nobody is reading. */
    uart_write_bytes(PEER_LINK_UART_NUM, line, (size_t)used);
}

/* Only the fields that were actually decoded, exactly as the uplink does it: a field
 * left out says "we do not know", a field sent as null says "we know it is nothing",
 * and the two are different claims about an aircraft. */
static int append_number(char *out, size_t size, int used, const char *key, double value) {
    if (isnan(value)) return used;
    return used + snprintf(out + used, size - used, ",\"%s\":%.7f", key, value);
}

static int append_string(char *out, size_t size, int used, const char *key,
                         const char *value) {
    if (value == NULL || value[0] == '\0') return used;
    return used + snprintf(out + used, size - used, ",\"%s\":\"%s\"", key, value);
}

void peer_link_send_contact(const uplink_contact_t *c) {
    char json[PEER_LINK_MAX_LINE - 16];
    int used = snprintf(json, sizeof json, "{\"rssi_dbm\":%d", c->rssi_dbm);
    used = append_string(json, sizeof json, used, "mac", c->mac);
    used = append_string(json, sizeof json, used, "uas_id", c->uas_id);
    used = append_string(json, sizeof json, used, "ua_type", c->ua_type);
    used = append_number(json, sizeof json, used, "lat", c->lat);
    used = append_number(json, sizeof json, used, "lon", c->lon);
    used = append_number(json, sizeof json, used, "altitude_m", c->altitude_m);
    used = append_number(json, sizeof json, used, "speed_mps", c->speed_mps);
    used += snprintf(json + used, sizeof json - used,
                     ",\"message_count\":%d,\"inferred\":%s}",
                     c->message_count, c->inferred ? "true" : "false");
    if (used >= (int)sizeof json) {
        ESP_LOGW(TAG, "contact did not fit; dropping");
        return;
    }
    send_line("CT1D", json);
}

void peer_link_send_status(uint32_t frames_seen, uint32_t messages_decoded,
                           uint32_t dropped) {
    char json[192];
    /* The channel comes from the sensor because only the sensor knows it. The master
     * asserting one it cannot see would be a fact invented on the reporting side. */
    snprintf(json, sizeof json,
             "{\"frames_seen\":%" PRIu32 ",\"messages_decoded\":%" PRIu32
             ",\"dropped\":%" PRIu32 ",\"channel\":%d}",
             frames_seen, messages_decoded, dropped, CIELOTRACK_RID_CHANNEL);
    send_line("CT1S", json);
}

void peer_link_sensor_start(void) {
    link_open();
    ESP_LOGI(TAG, "sensor link up on GPIO%d/TX GPIO%d/RX at %d baud",
             PEER_LINK_TX_GPIO, PEER_LINK_RX_GPIO, PEER_LINK_BAUD);
}

/* ---------------------------------------------------------------- master side --- */

static volatile int64_t last_heard_us;
static uint32_t peer_frames, peer_decoded, peer_dropped, peer_channel;
static uint32_t bad_crc, bad_frame;

bool peer_link_sensor_alive(void) {
    /* Three missed status reports, the same rule the server applies to a receiver.
     * Never true before the first line: a master that assumed its peer was fine until
     * told otherwise would repeat exactly the failure the heartbeat exists to catch. */
    if (last_heard_us == 0) return false;
    return (esp_timer_get_time() - last_heard_us) < (int64_t)PEER_STATUS_EVERY_SECONDS * 3 * 1000000;
}

/* Pulls one integer field out of a flat JSON object. Enough for the status line, and
 * deliberately not a parser: the contact lines are forwarded verbatim without being
 * understood, which is what keeps this end free of a JSON library. */
static uint32_t json_uint(const char *json, const char *key, uint32_t fallback) {
    char needle[32];
    snprintf(needle, sizeof needle, "\"%s\":", key);
    const char *at = strstr(json, needle);
    if (at == NULL) return fallback;
    return (uint32_t)strtoul(at + strlen(needle), NULL, 10);
}

static void handle_line(char *line) {
    char *kind = line;
    char *space = strchr(line, ' ');
    if (space == NULL) { bad_frame++; return; }
    *space = '\0';
    char *crc_text = space + 1;
    space = strchr(crc_text, ' ');
    if (space == NULL) { bad_frame++; return; }
    *space = '\0';
    char *json = space + 1;

    uint32_t claimed = (uint32_t)strtoul(crc_text, NULL, 16);
    if (crc32(json, strlen(json)) != claimed) {
        /* Counted, not merely dropped. A link quietly corrupting one line in ten would
         * otherwise look identical to a quiet sky. */
        bad_crc++;
        return;
    }
    last_heard_us = esp_timer_get_time();

    if (strcmp(kind, "CT1S") == 0) {
        peer_frames = json_uint(json, "frames_seen", peer_frames);
        peer_decoded = json_uint(json, "messages_decoded", peer_decoded);
        peer_dropped = json_uint(json, "dropped", peer_dropped);
        peer_channel = json_uint(json, "channel", peer_channel);
        return;
    }
    if (strcmp(kind, "CT1D") == 0) {
        uplink_forward_peer_contact(json);
        return;
    }
    bad_frame++;
}

static void master_task(void *arg) {
    (void)arg;
    static char line[PEER_LINK_MAX_LINE];
    int used = 0;
    for (;;) {
        uint8_t byte;
        int got = uart_read_bytes(PEER_LINK_UART_NUM, &byte, 1, pdMS_TO_TICKS(1000));
        if (got != 1) continue;
        if (byte == '\n') {
            line[used] = '\0';
            if (used > 0) handle_line(line);
            used = 0;
        } else if (used < (int)sizeof line - 1) {
            line[used++] = (char)byte;
        } else {
            /* Overlong line: discard to the next newline rather than splitting it into
             * two records, either of which would fail its CRC anyway. */
            used = 0;
            bad_frame++;
        }
    }
}

void peer_link_master_start(void) {
    link_open();
    xTaskCreate(master_task, "cielotrack_peer", 4096, NULL, 4, NULL);
    ESP_LOGI(TAG, "master link up; waiting for the sensor");
}

void peer_link_peer_status(uint32_t *frames, uint32_t *decoded, uint32_t *dropped,
                           uint32_t *crc_errors, uint32_t *channel) {
    if (channel) *channel = peer_channel;
    if (frames) *frames = peer_frames;
    if (decoded) *decoded = peer_decoded;
    if (dropped) *dropped = peer_dropped;
    if (crc_errors) *crc_errors = bad_crc + bad_frame;
}
