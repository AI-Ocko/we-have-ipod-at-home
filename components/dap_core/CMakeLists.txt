# Hardware-free logic. Pure C: no ESP-IDF or FreeRTOS includes, so the same
# sources also build with the host compiler under test/host.
idf_component_register(SRCS "translit.c" "screen.c" "keys.c" "keytest.c"
                            "flac_header.c" "pcm.c"
                       INCLUDE_DIRS ".")
