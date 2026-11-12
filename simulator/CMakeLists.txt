# UI 模拟器 — PC 上渲染与板上同一份 UI 代码（radial 渐变等打磨项的本地调试环境）
cmake_minimum_required(VERSION 3.16)
project(ui_simulator C CXX)
set(CMAKE_C_STANDARD 11)

set(LVGL_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/p4-hub/managed_components/lvgl__lvgl)
set(MAIN_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/p4-hub/main)
set(SDL2_DIR  CACHE PATH "SDL2 MinGW 解压根目录")

file(GLOB_RECURSE LVGL_SOURCES ${LVGL_DIR}/src/*.c)
file(GLOB_RECURSE THORVG_SOURCES ${LVGL_DIR}/src/libs/thorvg/*.cpp)

add_executable(ui_simulator
    main.c
    stubs/app_proto.c
    stubs/rule_runtime.c
    stubs/ota.c
    stubs/storage.c
    stubs/wifi.c
    ${LVGL_SOURCES}
    ${THORVG_SOURCES}
    ${MAIN_DIR}/ui/ui_pages.c
    ${MAIN_DIR}/ui/fonts/ui_font_cjk_18.c
    ${MAIN_DIR}/ui/fonts/ui_font_cjk_24.c
    ${MAIN_DIR}/ui/fonts/ui_font_cjk_48.c
    ${MAIN_DIR}/ui/ui_device_model.c
    ${MAIN_DIR}/ui/images/img_bg_home.c
    ${MAIN_DIR}/ui/images/img_detail_lamp.c
    ${MAIN_DIR}/ui/images/reicon_assets.c
    ${MAIN_DIR}/ui/images/reicon_nav_assets.c
)
target_compile_definitions(ui_simulator PRIVATE UI_SIMULATOR_FONT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fonts")
target_include_directories(ui_simulator PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${LVGL_DIR}
    ${LVGL_DIR}/src
    ${LVGL_DIR}/src/..
    ${MAIN_DIR}
    ${MAIN_DIR}/ui
    ${MAIN_DIR}/rule
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/p4-hub/components/protocol
    ${SDL2_DIR}/x86_64-w64-mingw32/include)

target_compile_definitions(ui_simulator PRIVATE LV_CONF_INCLUDE_SIMPLE=1 UI_SIMULATOR=1)
target_compile_options(ui_simulator PRIVATE -Wno-unused-parameter -Wno-sign-compare)
# ThorVG uses PRIu32 on MinGW; force the standard format macros into its C++ units.
target_compile_options(ui_simulator PRIVATE
    "$<$<COMPILE_LANGUAGE:CXX>:-D__STDC_FORMAT_MACROS>"
    "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include inttypes.h>")

if(WIN32)
    target_link_libraries(ui_simulator PRIVATE
        ${SDL2_DIR}/x86_64-w64-mingw32/lib/libSDL2.dll.a)
endif()

add_custom_command(TARGET ui_simulator POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${SDL2_DIR}/x86_64-w64-mingw32/bin/SDL2.dll
        $<TARGET_FILE_DIR:ui_simulator>)
