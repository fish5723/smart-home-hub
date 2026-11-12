# jwp_reader 宿主机单元测试（不依赖 ESP-IDF / 不接硬件）
#
# 用法:
#   cmake -S tests/jwp-reader -B tests/jwp-reader/build -G Ninja
#   cmake --build tests/jwp-reader/build
#   ctest --test-dir tests/jwp-reader/build --output-on-failure
#
# 说明: jwp_reader.c 只依赖 esp_err.h / esp_log.h，本目录用 stubs/ 下的
#       最小实现替代，从而可以在 PC 上直接跑格式解析的回归测试。
cmake_minimum_required(VERSION 3.16)
project(jwp_reader_test C)

set(CMAKE_C_STANDARD 11)

set(MAIN_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../firmware/p4-hub/main)

add_executable(test_jwp_reader
    test_jwp_reader.c
    ${MAIN_DIR}/standby/jwp_reader.c
)

target_include_directories(test_jwp_reader PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${MAIN_DIR}/standby
)

if(MSVC)
    target_compile_options(test_jwp_reader PRIVATE /W3)
else()
    target_compile_options(test_jwp_reader PRIVATE -Wall -Wextra)
endif()

enable_testing()
add_test(NAME jwp_reader COMMAND test_jwp_reader)