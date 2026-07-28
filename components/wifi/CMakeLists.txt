file(GLOB SRC_FILES *.c)

# esp_wifi is explicit: espnow_probe.c uses esp_now.h/esp_wifi.h directly, and
# relying on it arriving transitively is how a working build breaks later.
idf_component_register(SRCS ${SRC_FILES}
        INCLUDE_DIRS "./include"
        REQUIRES nvs_flash mdns util esp_wifi)
