# Drivers for the player's hardware. Depends on ESP-IDF; not built on the host.
idf_component_register(SRCS "storage.c" "audio_out.c" "lcd.c"
                       INCLUDE_DIRS "."
                       REQUIRES dap_core
                       PRIV_REQUIRES fatfs sdmmc esp_driver_sdmmc esp_driver_i2s
                                     esp_driver_i2c)
