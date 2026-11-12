cmake_minimum_required(VERSION 3.22)

# 阶段 8：F103 IAP bootloader — 独立最小裸机工程
# 不复用 f103-node 的 HAL/CMake 配置，避免把 HAL 和外设初始化带进 8KB 预算。
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_C_COMPILER arm-none-eabi-gcc)
set(CMAKE_ASM_COMPILER arm-none-eabi-gcc)
set(CMAKE_OBJCOPY arm-none-eabi-objcopy)
set(CMAKE_SIZE arm-none-eabi-size)
set(CMAKE_EXECUTABLE_SUFFIX_C ".elf")
set(CMAKE_EXECUTABLE_SUFFIX_ASM ".elf")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

project(f103-boot C ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(F103_NODE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../f103-node)

add_executable(${PROJECT_NAME}
    main.c
    boot_util.c
    boot_board.c
    boot_w25q.c
    boot_flash.c
    boot_meta.c
    startup_stm32f103xb_boot.s
    ${F103_NODE_DIR}/APP/comm/crc16.c
    ${F103_NODE_DIR}/APP/comm/crc32.c
)

option(BOOT_TEST_BAD_STAGING "Inject staging CRC corruption for IAP verification test" OFF)

target_compile_definitions(${PROJECT_NAME} PRIVATE STM32F103xB
    $<$<BOOL:${BOOT_TEST_BAD_STAGING}>:BOOT_TEST_BAD_STAGING>)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${F103_NODE_DIR}/Core/Inc
    ${F103_NODE_DIR}/Drivers/CMSIS/Device/ST/STM32F1xx/Include
    ${F103_NODE_DIR}/Drivers/CMSIS/Include
    ${F103_NODE_DIR}/APP/comm
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_options(${PROJECT_NAME} PRIVATE
    -mcpu=cortex-m3 -mthumb
    -Os -g0 -Wall -fdata-sections -ffunction-sections
)

target_link_options(${PROJECT_NAME} PRIVATE
    -mcpu=cortex-m3 -mthumb
    -T ${CMAKE_CURRENT_SOURCE_DIR}/f103_boot.ld
    -Wl,-Map=${PROJECT_NAME}.map
    -Wl,--gc-sections
    -Wl,--print-memory-usage
    -nostdlib
    -nostartfiles
)

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary "$<TARGET_FILE:${PROJECT_NAME}>"
            "${CMAKE_BINARY_DIR}/${PROJECT_NAME}.bin"
    COMMAND ${CMAKE_SIZE} "$<TARGET_FILE:${PROJECT_NAME}>"
    COMMENT "Generate bootloader .bin"
)
