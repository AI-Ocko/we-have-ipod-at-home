# Hardware-free logic. Pure C: no ESP-IDF or FreeRTOS includes, so the same
# sources also build with the host compiler under test/host.
idf_component_register(SRCS "translit.c" "screen.c" "keys.c" "keytest.c"
                            "flac_header.c" "pcm.c" "id3.c"
                            "mp3_xing.c" "marquee.c" "tags.c"
                            "library.c" "menu.c" "tracklist.c"
                            "browse.c"
                       INCLUDE_DIRS ".")
