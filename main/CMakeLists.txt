idf_component_register(SRCS "app_main.c" "player.c" "debug_console.c"
                            "library_task.c" "ui.c" "ui_options.c" "ui_play.c"
                            "ui_screens.c" "state_task.c"
                       INCLUDE_DIRS "."
                       REQUIRES dap_hw dap_core dap_fs esp_timer esp_driver_uart esp_app_format nvs_flash)
