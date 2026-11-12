# 固件目录说明

三个独立固件工程，**不要共用代码**（协议要两侧各自实现，这才是"协议"而非"库"的证明）。

```
firmware/
├── p4-hub/     ESP32-P4   HMI / AI 主控        工具链: ESP-IDF v6.1
├── f103-node/  STM32F103  实时控制节点          工具链: arm-none-eabi-gcc + CMake
└── c3-node/    ESP32-C3   低功耗无线节点        工具链: ESP-IDF v6.1（阶段 6 验证固件已可运行）
```

---

## 各固件的实际目录结构

### `p4-hub/`（阶段二～四已实现）

```
p4-hub/
├── CMakeLists.txt
├── sdkconfig.defaults        ← 关键配置固化在这里（PSRAM、分区表、日志级别）
├── partitions.csv            ← 现状 nvs 24K / phy 4K / factory 6M / assets(fat) 9900K（OTA 双分区为阶段 8 TODO）
├── main/
│   ├── app_main.c            ← 启动顺序编排 + 主循环（协议轮询/喂狗）
│   ├── Kconfig.projbuild     ← 硬件 / 待机层 / 天气 / 壁纸配置项
│   ├── bsp/
│   │   ├── lcd/ touch/ lvgl/        ← 点屏(JD9165BA)、GT911、LVGL 9.4 桥接
│   │   ├── storage/                 ← NVS 封装 + SD 卡 + crash_log
│   │   └── wifi/ camera/ time/      ← esp_hosted SDIO / OV5647 / SNTP
│   ├── standby/              ← 待机时钟层 + 动态天气壁纸（jwp_reader / wallpaper_player / standby_ui / weather_service）
│   ├── rule/                 ← 规则引擎 + 运行时（阶段 4）
│   ├── ui/                   ← 六页 UI（ui_pages）+ 设计 token + 内置字体/图片资源
│   ├── vision/               ← 视觉平台
│   ├── manager/              ← 摄像头管理器
│   └── common/               ← JSON 工具、错误码、看门狗、视觉类型
└── components/
    ├── protocol/             ← 协议主机侧：帧解析、CRC、重传、去重、ALARM_EVENT
    └── esp_lcd_jd9165/       ← JD9165BA 屏驱动
```

（`managed_components/` 为 IDF 依赖缓存，不入库；`patches/` 为依赖补丁）

### `f103-node/`（阶段一起填充，阶段 1～2 已完成）

```
f103-node/
├── CMakeLists.txt / CMakePresets.json / smart_home.ioc
├── startup_stm32f103xb.s / STM32F103XX_FLASH.ld
├── Core/                     ← CubeMX 生成的 HAL 初始化（Src/main.c、stm32f1xx_it.c、adc/i2c/spi/tim/usart…）
├── Drivers/                  ← STM32 HAL + CMSIS（入库，保证脱离 CubeMX 可独立构建）
├── BSP/                      ← 板级驱动层
│   ├── bsp_adc.[ch]          ← 土壤/雨滴/MQ135/MQ4 采集 + 分压换算
│   ├── bsp_dht11.[ch] / bsp_bmp280.[ch] / bsp_human.[ch](PIR)
│   ├── bsp_eeprom.[ch](AT24Cxx) / bsp_w25q64.[ch](SPI Flash，阶段 8 占位)
│   ├── bsp_ssd1306[_fonts/_tests].[ch]  ← 0.96" OLED
│   ├── bsp_beep.[ch](蜂鸣器) / bsp_tim.[ch]
│   ├── bsp_iwdg.[ch]         ← 独立看门狗（2s）
│   └── bsp_pins.h            ← 全部引脚单点维护
├── APP/
│   ├── comm/                 ← 协议从机侧（app_proto / proto_frame / proto_tx / proto_uart / crc16）
│   ├── control/              ← act_manager（GPIO 单一所有权）+ app_beep + app_irrigation
│   ├── sensor/               ← app_{soil,rain,mq4,air,dht11,bmp280,human} + app_mq_calib
│   └── display/              ← app_monitor（OLED 降级显示）
├── Sys/                      ← delay / retarget（printf 重定向）
├── common/                   ← mq_math、fmt_fixed、error_code、debug_config
└── cmake/                    ← 工具链与 CubeMX 集成（arm-none-eabi / starm-clang）
```

（**不存在** `bsp_relay` / `bsp_titan_lock`：继电器与 PB8 由 `APP/control/act_manager` 直接驱动；也没有 `Protocol/` 目录，协议实现在 `APP/comm/`）

### `c3-node/`（阶段 6 验证固件，已可运行）

```
main/
├── app_main.c     主循环：1Hz Notify + MQTT 上行 + 10s 统计
├── ble_svc.c/.h   NimBLE：FFF0 遥测/控制 + FFF3 配网（文本协议，需加密链路）
└── net_link.c/.h  Wi-Fi STA（NVS 凭据优先）+ MQTT（复用阶段 5 主题）
```

配网：手机连 `SHH-C3` → `0xFFF4` 写 `ssid=...` / `pass=...` → `0xFFF5` 写 `1`。
详见 `docs/18-阶段六-C3低功耗节点规范.md`。板载天线存在阻抗失配，TX 限 8.5dBm（ADR-025）。

---

## 通信协议基线（P4 ↔ F103，两侧 `protocol.h` 同源）

- 物理层：**UART2，P4 TX=GPIO46 / RX=GPIO47 @115200**，接 F103 USART1（PA9/PA10）。
- 帧：SOF 0xAA55 · VER 0x02 · 8B 帧头 · CRC16-CCITT-FALSE · ACK 超时 **100ms** · 最大重传 3 · 去重窗口 8。
- 传感器上报：**SENSOR_REPORT @1Hz，条目 6B**（dev_id 1B + quality 1B + float32 4B）。
- 告警上报：**ALARM_EVENT 载荷 10B**（alarm_id 1B + level 1B + value float32 + threshold float32；2026-10-18 补齐生产者/消费者）。
- 链路状态：3s 无帧→DEGRADED、6s→DOWN；LINK_DOWN 持续 >10s 由 F103 强制关断灯/泵/蜂鸣。

## 锁（PB8）现状

**不做锁**（XG07E 模块买错，决策 2026-10-18）：F103 侧 `app_lock` / `bsp_lock` 已删除，调度器已去掉 LOCK 任务。PB8 仍由 `APP/control/act_manager` 独占写，默认输出锁定电平。

---

## 编码约定（三人以下也别省，这是工程素养）

| 项目 | 约定 |
|---|---|
| 命名 | 模块前缀函数：`proto_send_frame()`、`act_manager_set()`；静态函数加 `s_` 前缀 |
| 错误处理 | 每个可能失败的操作都要返回值；**禁止静默忽略错误** |
| 日志 | 分级（`LOGE/LOGW/LOGI/LOGD`），生产配置只留 INFO 以上 |
| 副作用的函数 | 命名要体现（`_init` / `_deinit` / `_set` / `_get` 区分清楚） |
| 魔法数字 | 一律宏定义或 `const`，禁止裸数字（尤其时间、阈值、引脚） |
| 中断 | ISR 只投递事件，不做业务、不做动态分配、不打印日志 |
| 提交信息 | `feat|fix|docs|refactor|test(模块): 做了什么、为什么` |

---

## 构建命令速查

```bash
# ESP32-P4（推荐用封装脚本，已在脚本里固化 IDF v6.1 环境；唯一构建目录 build/）
tools\p4build.bat                       # Windows cmd；或先 . .\tools\activate_idf.ps1 再 idf.py build
idf.py -p COM3 flash monitor

# ESP32-C3（环境同上：必须用 python 调用 idf.py，且 MSYSTEM 置空）
# cd firmware/c3-node && python "%IDF_PATH%\tools\idf.py" -B build -p COM9 flash

# STM32F103
cd firmware/f103-node && cmake -B build -G Ninja && cmake --build build
# 烧录（OpenOCD 示例）
openocd -f interface/stlink.cfg -f target/stm32f1x.cfg -c "program build/xxx.elf verify reset exit"

# 跨链路 CRC / 帧格式一致性单测（PC 上跑，不需要板子；退出码 0 = 全过）
powershell -ExecutionPolicy Bypass -File firmware\tests\crc_crosslink\run.ps1

# MQTT TLS 证书（只需在本机生成一次；默认复用已有 CA，换热点 IP 时重跑一次即可）
# 产物：%TEMP%\mqtt-broker\certs（含私钥，不入库）+ 两个工程的 certs\ca.pem（公开证书，入库）
python tools\gen_mqtt_certs.py --ip <当前热点 IP>
```

> **构建前前提**：`firmware/p4-hub/certs/ca.pem` 与 `firmware/c3-node/certs/ca.pem` 必须存在，
> 它们通过 `CONFIG_MBEDTLS_CUSTOM_CERTIFICATE_BUNDLE_PATH="certs"` 被打进 crt bundle；
> 缺失会直接编译失败。ca.pem 已入库，正常 clone 即有。
