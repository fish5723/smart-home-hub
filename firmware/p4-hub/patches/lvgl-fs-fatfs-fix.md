# LVGL FatFS 编译依赖修复

> **触发条件:** `CONFIG_LV_USE_FS_FATFS=y` 后编译报错 `fatal error: ff.h: No such file or directory`
>
> **根因:** LVGL 组件的 `idf_component_register` 未声明对 `fatfs` 组件的依赖，导致编译 `lv_fs_fatfs.c` 时找不到 FatFS 头文件。
>
> **影响文件:** `managed_components/lvgl__lvgl/` 下 2 个文件（执行 `idf.py fullclean` 后会被覆盖，需重新打补丁）

---

## 补丁 1: `managed_components/lvgl__lvgl/CMakeLists.txt`

在第 22 行 `include(esp.cmake)` 之后插入 fatfs 依赖：

```cmake
if(ESP_PLATFORM)
  include(${CMAKE_CURRENT_LIST_DIR}/env_support/cmake/esp.cmake)
  if(CONFIG_LV_USE_FS_FATFS)                              # ← 新增
    idf_component_get_property(fatfs_lib fatfs COMPONENT_LIB)  # ←
    target_link_libraries(${COMPONENT_LIB} PRIVATE ${fatfs_lib}) # ←
  endif()                                                  # ←
elseif(MICROPY_DIR)
```

---

## 补丁 2: `managed_components/lvgl__lvgl/env_support/cmake/esp.cmake`

把原始的单路 `idf_component_register` 拆成两路，`CONFIG_LV_USE_FS_FATFS=y` 时追加 `fatfs` 到 `PRIV_REQUIRES`：

原始（约第 61-76 行）:
```cmake
  if(CONFIG_LV_USE_PPA)
    set(IDF_COMPONENTS esp_driver_ppa esp_mm esp_timer log)
  else()
    set(IDF_COMPONENTS esp_timer log)
  endif()

  idf_component_register(SRCS ${SOURCES} ${EXAMPLE_SOURCES} ${DEMO_SOURCES}
      INCLUDE_DIRS ${LVGL_ROOT_DIR} ${LVGL_ROOT_DIR}/src ${LVGL_ROOT_DIR}/../
                   ${LVGL_ROOT_DIR}/examples ${LVGL_ROOT_DIR}/demos
      PRIV_REQUIRES ${IDF_COMPONENTS})
```

改为:
```cmake
  if(CONFIG_LV_USE_PPA)
    set(IDF_COMPONENTS esp_driver_ppa esp_mm esp_timer log)
  else()
    set(IDF_COMPONENTS esp_timer log)
  endif()

  if(CONFIG_LV_USE_FS_FATFS)
    idf_component_register(SRCS ${SOURCES} ${EXAMPLE_SOURCES} ${DEMO_SOURCES}
        INCLUDE_DIRS ${LVGL_ROOT_DIR} ${LVGL_ROOT_DIR}/src ${LVGL_ROOT_DIR}/../
                     ${LVGL_ROOT_DIR}/examples ${LVGL_ROOT_DIR}/demos
        PRIV_REQUIRES ${IDF_COMPONENTS} fatfs)
  else()
    idf_component_register(SRCS ${SOURCES} ${EXAMPLE_SOURCES} ${DEMO_SOURCES}
        INCLUDE_DIRS ${LVGL_ROOT_DIR} ${LVGL_ROOT_DIR}/src ${LVGL_ROOT_DIR}/../
                     ${LVGL_ROOT_DIR}/examples ${LVGL_ROOT_DIR}/demos
        PRIV_REQUIRES ${IDF_COMPONENTS})
  endif()
```

---

## 相关 sdkconfig 开关（SD 资产管线）

| 配置项 | 值 | 说明 |
|---|---|---|
| `CONFIG_LV_USE_FS_FATFS` | `y` | LVGL→FatFS→SD 文件访问 |
| `CONFIG_LV_FS_FATFS_LETTER` | `83` | 盘符 `S:` |
| `CONFIG_LV_USE_LODEPNG` | `y` | PNG 解码（lv_init 自动注册） |

> 注（2026-10-18）：本表原先还列了 `CONFIG_SMART_HOME_SDCARD_ENABLED` —— 该自定义开关
> 全工程零引用（SD 卡初始化由 `main/app_main.c` 无条件执行、SDMMC 驱动由
> `esp_driver_sdmmc` 组件提供），已在 2026-10-18 从 `main/Kconfig.projbuild` 删除。