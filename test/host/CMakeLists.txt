# Builds the hardware-free dap_core and dap_fs sources with the host compiler
# and runs their Unity tests. Not part of the firmware build.
cmake_minimum_required(VERSION 3.22)
project(dap_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(DEFINED ENV{IDF_PATH})
  set(IDF_PATH $ENV{IDF_PATH})
else()
  set(IDF_PATH /opt/esp-idf)
endif()
set(UNITY_DIR ${IDF_PATH}/components/unity/unity/src)
if(NOT EXISTS ${UNITY_DIR}/unity.c)
  message(FATAL_ERROR "Unity not found at ${UNITY_DIR}; set IDF_PATH")
endif()

set(CORE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../components/dap_core)

# Sanitizers turn out-of-bounds reads and writes into test failures.
add_compile_options(-Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-omit-frame-pointer)
add_link_options(-fsanitize=address,undefined)

add_library(unity STATIC ${UNITY_DIR}/unity.c)
target_include_directories(unity PUBLIC ${UNITY_DIR})
target_compile_options(unity PRIVATE -w)

add_library(dap_core STATIC
  ${CORE_DIR}/translit.c
  ${CORE_DIR}/screen.c
  ${CORE_DIR}/keys.c
  ${CORE_DIR}/keytest.c
  ${CORE_DIR}/flac_header.c
  ${CORE_DIR}/pcm.c
  ${CORE_DIR}/id3.c
  ${CORE_DIR}/mp3_xing.c
  ${CORE_DIR}/marquee.c
  ${CORE_DIR}/tags.c
  ${CORE_DIR}/library.c
  ${CORE_DIR}/menu.c
  ${CORE_DIR}/tracklist.c
  ${CORE_DIR}/browse.c
  ${CORE_DIR}/play_queue.c
)
target_include_directories(dap_core PUBLIC ${CORE_DIR})

# The code that walks a file system: POSIX directory calls, no hardware.
set(FS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../components/dap_fs)
add_library(dap_fs STATIC
  ${FS_DIR}/libscan.c
)
target_include_directories(dap_fs PUBLIC ${FS_DIR})
target_link_libraries(dap_fs PUBLIC dap_core)

enable_testing()

function(dap_test name)
  add_executable(${name} ${name}.c)
  target_link_libraries(${name} PRIVATE dap_core unity)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

dap_test(test_translit)
dap_test(test_screen)
dap_test(test_keys)
dap_test(test_keytest)
dap_test(test_flac_header)
dap_test(test_pcm)
dap_test(test_id3)
dap_test(test_mp3_xing)
dap_test(test_marquee)
dap_test(test_tags)
dap_test(test_library)
dap_test(test_menu)
dap_test(test_tracklist)
dap_test(test_browse)
dap_test(test_play_queue)
# The FLAC files made by tools/make_test_audio.sh.
target_compile_definitions(test_flac_header PRIVATE
  FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
# The tagged files made by tools/make_test_audio.sh; fixtures/tags/README.md
# lists what each one holds.
target_compile_definitions(test_tags PRIVATE
  FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/tags")
# Its tests count the fread and fseek calls that tags_read makes.
target_link_options(test_tags PRIVATE -Wl,--wrap=fread -Wl,--wrap=fseek)
# The same tests against a build of the index whose sort never takes its
# quicksort stage. That stage normally sorts everything and the heapsort
# behind it is reached only on a hostile ordering, so without this the
# fallback would ship untested.
add_executable(test_library_heapsort test_library.c ${CORE_DIR}/library.c)
target_compile_definitions(test_library_heapsort PRIVATE
  LIBRARY_SORT_QUICK_BUDGET=0)
target_link_libraries(test_library_heapsort PRIVATE dap_core unity)
add_test(NAME test_library_heapsort COMMAND test_library_heapsort)
# The scan, against the small library made by tools/make_test_audio.sh;
# fixtures/lib/README.md lists what it holds.
dap_test(test_libscan)
target_link_libraries(test_libscan PRIVATE dap_fs)
target_compile_definitions(test_libscan PRIVATE
  FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/lib")
