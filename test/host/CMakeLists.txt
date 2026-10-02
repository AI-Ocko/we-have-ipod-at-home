# Builds the hardware-free dap_core sources with the host compiler and runs
# their Unity tests. Not part of the firmware build.
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
)
target_include_directories(dap_core PUBLIC ${CORE_DIR})

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
# The FLAC files made by tools/make_test_audio.sh.
target_compile_definitions(test_flac_header PRIVATE
  FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
# The tagged files made by tools/make_test_audio.sh; fixtures/tags/README.md
# lists what each one holds.
target_compile_definitions(test_tags PRIVATE
  FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/tags")
# Its tests count the fread and fseek calls that tags_read makes.
target_link_options(test_tags PRIVATE -Wl,--wrap=fread -Wl,--wrap=fseek)
