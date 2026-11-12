cmake_minimum_required(VERSION 3.16)
project(ota_manifest_test C)
set(CMAKE_C_STANDARD 11)
# Fixed cJSON 1.7.19~2 test fixture; no ESP-IDF download or network needed.
set(CJSON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendor)
add_executable(test_ota_manifest test_ota_manifest.c
    ../../firmware/p4-hub/main/ota/ota_manifest.c ${CJSON_DIR}/cJSON.c)
target_include_directories(test_ota_manifest PRIVATE
    ../../firmware/p4-hub/main/ota ${CJSON_DIR})
enable_testing()
add_test(NAME ota_manifest COMMAND test_ota_manifest)
