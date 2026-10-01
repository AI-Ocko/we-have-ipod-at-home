# Drivers for the player's hardware. Depends on ESP-IDF; not built on the host.
idf_component_register(SRCS "storage.c" "audio_out.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES fatfs sdmmc esp_driver_sdmmc esp_driver_i2s)
