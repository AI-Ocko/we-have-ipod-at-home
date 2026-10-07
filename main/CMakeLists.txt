idf_component_register(SRCS "app_main.c" "player.c" "debug_console.c"
                            "library_task.c" "ui.c" "ui_options.c" "ui_play.c"
                            "ui_screens.c" "ui_playlists.c" "ui_text.c" "state_task.c"
                            "playlist_task.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_ringbuf dap_hw dap_core dap_fs esp_timer esp_driver_uart esp_app_format nvs_flash)
