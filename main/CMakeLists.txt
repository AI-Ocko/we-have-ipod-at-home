idf_component_register(SRCS "app_main.c" "player.c" "debug_console.c"
                       INCLUDE_DIRS "."
                       REQUIRES dap_hw dap_core esp_timer esp_driver_uart)
