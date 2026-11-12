# 阶段 2C-1：F103 协议纯逻辑层 PC 单元测试
# 直接编译 firmware/f103-node/APP/comm/ 下的纯逻辑源文件（无副本、无桩），
# 保证"测的代码就是板上跑的代码"。
cmake_minimum_required(VERSION 3.16)
project(f103_proto_tests C)

set(CMAKE_C_STANDARD 11)
set(COMM_DIR ${CMAKE_CURRENT_LIST_DIR}/../../firmware/f103-node/APP/comm)

add_library(f103_proto STATIC
    ${COMM_DIR}/crc16.c
    ${COMM_DIR}/proto_frame.c)
target_include_directories(f103_proto PUBLIC ${COMM_DIR})
target_compile_options(f103_proto PRIVATE -Wall -Wextra)

add_executable(test_proto test_proto.c)
target_link_libraries(test_proto PRIVATE f103_proto)
target_compile_options(test_proto PRIVATE -Wall -Wextra)

enable_testing()
add_test(NAME protocol_spec_s8 COMMAND test_proto)
