# The following lines of boilerplate have to be in your project's
# CMakeLists.txt file
cmake_minimum_required(VERSION 3.16)

# esp_wifi_remote ships Kconfig variants by major.minor (for example idf_v6.1),
# while ESP-IDF may export a full patch version (for example 6.1.x).
if(DEFINED ENV{ESP_IDF_VERSION})
    string(REGEX MATCH "^[0-9]+\\.[0-9]+" SMART_HOME_IDF_KCONFIG_VERSION "$ENV{ESP_IDF_VERSION}")
    if(SMART_HOME_IDF_KCONFIG_VERSION)
        set(ENV{ESP_IDF_VERSION} "${SMART_HOME_IDF_KCONFIG_VERSION}")
    endif()
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
## 固件版本：写入 esp_app_desc_t.version，OTA 日志/版本管理读取。
## 网络 OTA 的防降级按前 3 段数字比较，默认版本必须不低于最近发布的设备版本；
## 联调双镜像时可用 -DPROJECT_VER=0.8.2-xxx 临时覆盖。
set(PROJECT_VER "0.8.1-netota" CACHE STRING "Firmware version")

project(p4-hub)
