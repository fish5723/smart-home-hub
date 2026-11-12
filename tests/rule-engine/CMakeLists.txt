cmake_minimum_required(VERSION 3.16)
project(rule_engine_test C)
set(CMAKE_C_STANDARD 11)
add_executable(rule_engine_test test_rule_engine.c
    ../../firmware/p4-hub/main/rule/rule_engine.c)
target_include_directories(rule_engine_test PRIVATE
    ../../firmware/p4-hub/main/rule)
enable_testing()
add_test(NAME rule_engine_test COMMAND rule_engine_test)
