# Host build of the decoder tests. Deliberately not an ESP-IDF project: the parser
# knows nothing about radios, so it can be compiled and proved correct with plain gcc
# long before it runs on a board — and after that, a field disagreement with the Pi is
# a difference in what each receiver heard, not in what each one understood.
#
#     make test

CC      ?= gcc
CFLAGS  ?= -std=c11 -Wall -Wextra -Werror -O2
BUILD   := build

.PHONY: test clean vectors

test: $(BUILD)/test_vectors $(BUILD)/test_ble $(BUILD)/test_pack $(BUILD)/test_wifi_rid $(BUILD)/test_identity_cache $(BUILD)/test_peer_link $(BUILD)/test_contact_json $(BUILD)/test_contact_time $(BUILD)/test_health $(BUILD)/decode_cli $(BUILD)/extract_cli $(BUILD)/pack_cli $(BUILD)/label_cli
	@$(BUILD)/test_vectors
	@echo
	@$(BUILD)/test_ble
	@echo
	@$(BUILD)/test_pack
	@echo
	@$(BUILD)/test_wifi_rid
	@echo
	@$(BUILD)/test_identity_cache
	@echo
	@$(BUILD)/test_peer_link
	@echo
	@$(BUILD)/test_contact_json
	@$(BUILD)/test_contact_time
	@$(BUILD)/test_health

$(BUILD)/test_vectors: odid_decode.c test/test_vectors.c odid_decode.h test/odid_vectors.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ odid_decode.c test/test_vectors.c -lm

$(BUILD)/test_health: esp32/main/health.c test/test_health.c esp32/main/health.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ esp32/main/health.c test/test_health.c

$(BUILD)/test_contact_time: esp32/main/contact_time.c test/test_contact_time.c esp32/main/contact_time.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ esp32/main/contact_time.c test/test_contact_time.c

$(BUILD)/test_peer_link: test/test_peer_link.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ test/test_peer_link.c

$(BUILD)/test_contact_json: esp32/main/contact_json.c test/test_contact_json.c esp32/main/contact_json.h esp32/main/uplink.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ esp32/main/contact_json.c test/test_contact_json.c -lm

$(BUILD)/label_cli: odid_decode.c test/label_cli.c odid_decode.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ odid_decode.c test/label_cli.c -lm

$(BUILD)/test_identity_cache: identity_cache.c test/test_identity_cache.c identity_cache.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ identity_cache.c test/test_identity_cache.c

# The fixture is the source of truth; this header is generated from it.
vectors:
	python3 ../tools/vectors_to_c.py

# Wi-Fi carries Message Packs where BLE carries single messages, so this is the framing
# the Wi-Fi receiver cannot work without.
# Wi-Fi's own framing: finding the payload inside an 802.11 frame, which is pure
# byte-hunting and provable on a host — the half that talks to a radio is not.
$(BUILD)/test_wifi_rid: wifi_rid.c odid_decode.c test/test_wifi_rid.c wifi_rid.h odid_decode.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ wifi_rid.c odid_decode.c test/test_wifi_rid.c -lm

$(BUILD)/test_pack: odid_decode.c test/test_pack.c odid_decode.h test/odid_vectors.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ odid_decode.c test/test_pack.c -lm

$(BUILD)/test_ble: odid_ble.c test/test_ble.c odid_ble.h odid_decode.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ odid_ble.c test/test_ble.c -lm

# Takes a whole advertising payload rather than a message someone already found, so a
# real capture from the air can be run through the same extraction the board uses.
$(BUILD)/extract_cli: odid_ble.c odid_decode.c test/extract_cli.c odid_ble.h odid_decode.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ odid_ble.c odid_decode.c test/extract_cli.c -lm

$(BUILD)/pack_cli: odid_decode.c test/pack_cli.c odid_decode.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ odid_decode.c test/pack_cli.c -lm

$(BUILD)/decode_cli: odid_decode.c test/decode_cli.c odid_decode.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ odid_decode.c test/decode_cli.c -lm

clean:
	rm -rf $(BUILD)
