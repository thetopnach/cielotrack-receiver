/* Getting a decoded contact off the board and into the central server.
 *
 * Until this existed the receiver could hear drones and tell nobody: every decode went
 * to a serial console, so a measurement lasted exactly as long as somebody had a cable
 * attached and a terminal open. An overnight run was lost that way. Reporting is
 * therefore not a feature that comes after the experiments — it is what makes the
 * experiments keep their results.
 *
 * This is the first slice, and deliberately the smallest one that proves the path: a
 * fixed device key, no claim flow, no queue. A contact that cannot be sent is dropped
 * and counted, not stored. Both of those are next.
 */
#ifndef UPLINK_H
#define UPLINK_H

#include <stdbool.h>
#include <stdint.h>

/* Brings up Wi-Fi and starts a clock. Returns immediately; the network arrives later
 * and uplink_ready() says when. Capture must never wait for this — a receiver with no
 * network still hears aircraft, and the whole design keeps decoding independent of
 * whether anyone is listening. */
void uplink_start(void);

/* True once there is an address and the clock has been set. Reporting before the clock
 * is set would stamp detections with 1970, which the server rejects — and rightly, since
 * a detection whose time is wrong is worse than one that never arrived. */
bool uplink_ready(void);

/* One decoded contact. Fields that were not decoded are passed as NAN or NULL and are
 * left out of the payload entirely rather than sent as nulls: the server drops unknown
 * fields but a null height is not the same claim as no height at all.
 *
 * Sends synchronously on the caller's task, so it is called from the reporting path
 * rather than the BLE callback — a blocked socket must not stall the radio. */
/* One decoded contact, in the shape both roles queue them.
 *
 * Shared rather than declared twice: the BLE role and the Wi-Fi role had their own
 * near-identical structs, which is two places for a field to be added and one place for
 * it to be forgotten. */
typedef struct {
    char mac[18];
    char uas_id[24];
    char ua_type[24];
    double lat, lon, altitude_m, speed_mps;
    int rssi_dbm;
    int message_count;
    bool inferred;
    /* esp_timer_get_time() at the moment this was decoded — monotonic microseconds
     * since boot, not a wall-clock time. The stamp that reaches the server is worked
     * out from this at upload time; see contact_time.h for why the age rather than the
     * time. Zero means the producer did not record one. */
    int64_t decoded_us;
} uplink_contact_t;

/* Sends up to `count` contacts in a single request.
 *
 * One POST per detection was costing about half of everything the BLE board heard once
 * it went outside and started hearing more: a TLS handshake takes a second or more, a
 * busy pass yields a contact every four seconds, and a 12-deep queue fills long before
 * it drains. Batching amortises the handshake across the whole burst, which is the
 * actual bottleneck — a deeper queue alone would only have delayed the loss.
 *
 * Partial success is not reported as failure: the server accepts what validates and
 * says how many it rejected. */
void uplink_report_batch(const uplink_contact_t *contacts, int count);

/* `inferred` marks an identity recalled from an earlier sighting of this transmitter
 * rather than decoded from this frame. It reaches the server as identity_source, the
 * same distinction the Pi has always made — a receiver that reports a serial it did not
 * hear, without saying so, makes its own data untrustworthy. */
void uplink_report(const char *mac, const char *uas_id, const char *ua_type,
                   double lat, double lon, double altitude_m, double speed_mps,
                   int rssi_dbm, int message_count, bool inferred);

/* Counters, for the same reason the Pi keeps them: a receiver that silently stops
 * uploading looks identical to a quiet sky. */
uint32_t uplink_sent(void);
uint32_t uplink_failed(void);
uint32_t uplink_heartbeats(void);

/* Says "still here", with what this receiver can currently see.
 *
 * Without it a board is indistinguishable from a dead one whenever the sky is empty,
 * which is most of the time: detections alone cannot tell a receiver that hears nothing
 * from a receiver that hears nothing *because it stopped working*. Both ESP32 boards read
 * as permanently offline on the fleet page until this exists, which is worse than no
 * status at all — a column that is always red teaches you to ignore it.
 *
 * status_json is an object literal stored verbatim by the server and read key-by-key by
 * the fleet page. Passing "{}" is legitimate and still proves liveness. */
void uplink_heartbeat(const char *status_json);

/* True once there is an address, whether or not the clock is set. */
bool uplink_connected(void);

/* Kick SNTP now instead of waiting out its hour-long retry. See uplink.c. */
void uplink_resync_clock(void);

/* Contacts arriving over the wire from the sensor board, forwarded under the peer's own
 * device identity so each transport still reports as its own receiver — which is the
 * whole reason for having two boards.
 *
 * The JSON is taken as given and never parsed: the master adds a timestamp and the
 * transport and sends it on. Parsing it here would mean a second implementation of the
 * same decode, on the board that did not do the decoding.
 *
 * Buffered and sent in batches for the same reason the local queue is — a TLS handshake
 * per contact is what cost this project half a board's data this morning. */
void uplink_forward_peer_contact(const char *contact_json);

/* Sends anything buffered from the peer. Called from the reporting task, so contacts do
 * not sit waiting for the buffer to fill during a quiet spell. */
void uplink_flush_peer(void);

/* A heartbeat on the peer's behalf. The sensor has no network of its own, so without
 * this its device row goes stale and the dashboard calls a working receiver dead. */
void uplink_peer_heartbeat(const char *status_json);

/* Stops the station chasing the network, for a board that needs its radio elsewhere.
 *
 * Reconnecting forever is right for a receiver whose radio has nothing else to do, and
 * wrong for one sniffing a channel the access point is not on: association drives the
 * channel, so a station quietly retrying in the background parks the radio on the
 * router's channel and the capture listens to the wrong place while every counter still
 * looks healthy. Held, the station stays off the air until released; releasing connects
 * immediately. */
void uplink_hold_station(bool hold);

#endif /* UPLINK_H */
