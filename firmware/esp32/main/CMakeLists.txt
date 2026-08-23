# The decoder lives one level up and is compiled straight into the firmware rather than
# copied here. There is one implementation, and it is the one the host tests and the
# conformance vectors run against — a second copy on the device would be a second thing
# to be wrong, and the whole point is that the ESP32 and the Pi agree.
idf_component_register(
    SRCS "main.c"
         "uplink.c"
         "provisioning.c"
         "claim.c"
         "peer_link.c"
         "contact_time.c"
         "health.c"
         "wifi_capture.c"
         "../../wifi_rid.c"
         "../../odid_decode.c"
         "../../identity_cache.c"
         "../../odid_ble.c"
    INCLUDE_DIRS "." "../.."
    REQUIRES bt nvs_flash esp_timer esp_wifi esp_netif esp_event esp_driver_uart
             espressif__network_provisioning espressif__cjson
             esp_http_client esp-tls mbedtls lwip
)

# The build actually on the board, so a rollout can be checked rather than assumed —
# the same reason the Pi reports its version. Falls back to "unknown" outside a git
# checkout rather than failing the build, because a firmware that will not compile off
# a clone is worse than one whose version column reads "unknown".
execute_process(
    COMMAND git describe --always --dirty --tags
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    OUTPUT_VARIABLE CIELOTRACK_GIT_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT CIELOTRACK_GIT_VERSION)
    set(CIELOTRACK_GIT_VERSION "unknown")
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    CIELOTRACK_FIRMWARE_VERSION="${CIELOTRACK_GIT_VERSION}")
