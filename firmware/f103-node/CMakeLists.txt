cmake_minimum_required(VERSION 3.22)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#
# 得到所有子文件夹目录的函数：
add_definitions(-DLV_LVGL_H_INCLUDE_SIMPLE)
function(collect_subdirectories OUTPUT_VAR ROOT_DIR)
    set(SUBDIRS "${ROOT_DIR}")  # 优先把根目录放进去
    function(_recurse dir)
        file(GLOB CHILDREN RELATIVE ${dir} ${dir}/*)
        foreach(CHILD ${CHILDREN})
            set(CHILD_PATH "${dir}/${CHILD}")
            if(IS_DIRECTORY ${CHILD_PATH})
                list(APPEND SUBDIRS ${CHILD_PATH})
                _recurse(${CHILD_PATH})
            endif()
        endforeach()
        set(SUBDIRS ${SUBDIRS} PARENT_SCOPE)
    endfunction()
    _recurse(${ROOT_DIR})
    list(REMOVE_DUPLICATES SUBDIRS)
    set(${OUTPUT_VAR} ${SUBDIRS} PARENT_SCOPE)
endfunction()

function(collect_all_c_files OUTPUT_VAR ROOT_DIR)
    file(GLOB_RECURSE C_FILES "${ROOT_DIR}/*.c")
    list(REMOVE_DUPLICATES C_FILES)
    list(FILTER C_FILES EXCLUDE REGEX "bsp_ssd1306_tests\\.c$")
    set(${OUTPUT_VAR} ${C_FILES} PARENT_SCOPE)
endfunction()

# 整备BSP文件夹
set(BSP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/BSP)         # 设定根目录地址
collect_subdirectories(BSP_SUBDIRS ${BSP_DIR})      # 得到所有子文件夹
collect_all_c_files(BSP_SOURCE_FILES ${BSP_DIR})     # 得到所有文件名
# 整备SYS文件夹
set(SYS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/Sys)
collect_subdirectories(SYS_SUBDIRS ${SYS_DIR})
collect_all_c_files(SYS_SOURCE_FILES ${SYS_DIR})
# 整备APP文件夹
set(APP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/APP)
collect_subdirectories(APP_SUBDIRS ${APP_DIR})
collect_all_c_files(APP_SOURCE_FILES ${APP_DIR})
# 整备COMMON文件夹
set(COMMON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/common)

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)


# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Set the project name
set(CMAKE_PROJECT_NAME f103-node)

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME} ${BSP_SOURCE_FILES} ${SYS_SOURCE_FILES} ${APP_SOURCE_FILES}


)

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user sources here
)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    ${BSP_DIR}
    ${BSP_SUBDIRS}
    ${SYS_DIR}
    ${SYS_SUBDIRS}
    ${APP_DIR}
    ${APP_SUBDIRS}
    ${COMMON_DIR}
)

# 阶段 8 回滚验收：生成"合法但启动后不确认"的坏镜像（默认 OFF）。
# 用法：cmake -DAPP_TEST_BAD_IMAGE=ON 到独立 build 目录，产出物不要用于正式固件。
option(APP_TEST_BAD_IMAGE "Build a bad image for bootloader rollback test" OFF)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    $<$<BOOL:${APP_TEST_BAD_IMAGE}>:APP_TEST_BAD_IMAGE>
    # MQ 标定时间参数（阶段 1 验收整改 §2.3）：
    #   Debug   = 快速模式：预热 60s + 老化 60s（开发联调用）
    #   Release = 生产模式：预热 3 分钟 + 老化 24 小时（出货/长时间测试用）
    # 详见 APP/sensor/app_mq_calib.h；启动日志会打印当前生效的时长。
    $<$<CONFIG:Debug>:MQ_CALIB_FAST_AGING>
)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx
    # Add user defined libraries
)

# 浮点 printf 支持已移除（阶段 1 验收整改）：
#   newlib 的浮点格式化（_dtoa_r + _printf_float + _printf_i）实测约 4.7KB，
#   在 64KB Flash 的 F103C8 上占 7%。工程里只有约 15 处用到 %f，
#   已全部改用 common/fmt_fixed.h 的定点格式化（见该文件头部说明）。
#   浮点【运算】不受影响，软浮点仍在（__adddf3 等）。
#   如将来确实需要 %f，把下面两行取消注释即可。
#target_link_options(${CMAKE_PROJECT_NAME} PRIVATE
#        -u _printf_float
#)

# ===== Post-build: generate .bin and show size =====
find_program(ARM_SIZE arm-none-eabi-size)
find_program(ARM_OBJCOPY arm-none-eabi-objcopy)

if(ARM_SIZE AND ARM_OBJCOPY)
    add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
        COMMAND ${ARM_OBJCOPY} -O binary "$<TARGET_FILE:${CMAKE_PROJECT_NAME}>"
                "${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.bin"
        COMMAND ${ARM_SIZE} "$<TARGET_FILE:${CMAKE_PROJECT_NAME}>"
        COMMENT "Generate .bin and show size:"
    )
endif()