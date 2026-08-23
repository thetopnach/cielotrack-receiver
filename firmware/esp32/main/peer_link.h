/* A wire between the two boards, so only one of them talks to the network.
 *
 * The Wi-Fi board has to sit on the Remote ID channel, and the access point is on a
 * different one. Everything painful about that board came from trying to be in both
 * places: leaving channel 6 every cycle, re-associating a hundred times an hour until
 * the mesh stopped completing the handshake, twenty-minute stretches unable to report,
 * a clock that could not set itself, a per-device online window widened on the server
 * to stop it reading as dead. All of it downstream of one radio doing two jobs.
 *
 * The DroneSight unit in the same garden — two ESP32-S3s, one per transport — wires its
 * boards together over their pins. That is not a coincidence: with a link between them,
 * only one board needs a network, and the other never leaves its channel.
 *
 * So: the BLE board keeps the uplink and becomes the master. The Wi-Fi board becomes a
 * sensor with no station, no SNTP, no association, no channel alternation and no
 * credentials at all — it decodes and pushes contacts down a wire.
 *
 * The master holds both device identities. That keeps each transport reporting as its
 * own receiver, which is the entire reason for having two boards, without the sensor
 * needing a key it would otherwise have to be told.
 *
 * Wiring on the Seeed XIAO ESP32S3. Two wires carry the link, because it is one-way:
 * the sensor only writes and the master only reads. All three pins are on the bottom
 * header, GND at one end and D9/D10 three along.
 *
 *     sensor D9  (GPIO8, TX)  ->  master D10 (GPIO9, RX)    contacts and status
 *     sensor GND              <-> master GND                common reference
 *
 *     master D9  (GPIO8, TX)  ->  sensor D10 (GPIO9, RX)    optional; unused today
 *
 * The ground matters more than it looks. Two boards on separate supplies share no
 * reference, and a UART without one reads noise rather than nothing — which arrives as
 * CRC failures rather than as silence, so watch link_errors in the peer's heartbeat.
 * Powering both boards from the same source makes that wire redundant, and fitting it
 * anyway costs nothing.
 *
 * The silkscreen says D9 and D10; the code says GPIO8 and GPIO9. Those are the same two
 * pins — Seeed's D-numbers are not GPIO numbers, and confusing the two is how the first
 * version of this ended up specifying pins the board does not have.
 */
#ifndef PEER_LINK_H
#define PEER_LINK_H

#include <stdbool.h>
#include <stdint.h>

#include "uplink.h"

/* Line protocol. Text, one record per line, so a human with a serial adapter can watch
 * it — a binary framing here would have to be decoded before it could be diagnosed.
 *
 *   CT1D <crc32> {"mac":"..","uas_id":"..",...}\n     one contact
 *   CT1S <crc32> {"frames_seen":N,"messages_decoded":N,"dropped":N}\n   sensor health
 *
 * The CRC covers the JSON only. A UART between two boards on a pole is not a quiet
 * environment, and a corrupted line that still parses would put a wrong serial or a
 * wrong position into the record — worse than losing the line outright.
 *
 * Contacts carry no timestamp. The sensor has no clock and no way to set one; the
 * master stamps on arrival, which is within milliseconds of the decode and is the more
 * trustworthy of the two numbers anyway.
 */
#define PEER_LINK_UART_NUM     1
/* GPIO8 and GPIO9 — D9 and D10 on the Seeed XIAO ESP32S3 these boards actually are.
 *
 * The first version of this asked for GPIO17 and GPIO18, which the XIAO does not break
 * out at all: it exposes eleven pins labelled D0-D10 and neither of those is among them.
 * Of the eleven, D2 is a strapping pin sampled at boot, and D6/D7 are GPIO43/44 — the
 * UART0 console, which is emitting the log stream. Wiring the link to those would feed
 * every log line into the peer's parser. D9 and D10 are free and adjacent, and GND is on
 * the same header.
 *
 * That header reads D7, D8, D9, D10, 3V3, GND, UUSB, so GND is two pins beyond D10 rather
 * than next to it — this comment claimed the link was "three pins in a row", which it is
 * not, and a wiring document repeated it before anyone checked against the board. */
#define PEER_LINK_TX_GPIO      8    /* XIAO D9  */
#define PEER_LINK_RX_GPIO      9    /* XIAO D10 */
#define PEER_LINK_BAUD         115200
#define PEER_LINK_MAX_LINE     512
/* How often the sensor says how it is doing. Short enough that the master notices a
 * dead sensor within a few minutes, long enough to be invisible next to the traffic. */
#define PEER_STATUS_EVERY_SECONDS 60

/* Sensor side: opens the link and sends. Never blocks the capture path. */
void peer_link_sensor_start(void);
void peer_link_send_contact(const uplink_contact_t *contact);
void peer_link_send_status(uint32_t frames_seen, uint32_t messages_decoded,
                           uint32_t dropped);

/* Master side: reads the link, stamps and forwards under the peer's identity. */
void peer_link_master_start(void);

/* True once the sensor has been heard from recently. A master that reports its peer as
 * healthy because it has simply never heard from it would repeat the mistake the
 * heartbeat was built to fix. */
bool peer_link_sensor_alive(void);

/* The sensor's own counters, as last reported, plus how many lines this end threw away.
 * The master puts these in the peer's heartbeat: a sensor with no network of its own is
 * otherwise invisible, and a link corrupting traffic would look exactly like a quiet
 * sky. */
void peer_link_peer_status(uint32_t *frames, uint32_t *decoded, uint32_t *dropped,
                           uint32_t *crc_errors, uint32_t *channel);

#endif /* PEER_LINK_H */
