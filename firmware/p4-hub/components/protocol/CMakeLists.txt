## Smart Home Hub — Protocol Component (P4 side)
## 阶段 2B: P4↔F103 二进制协议栈，独立实现

idf_component_register(
    SRCS
        "crc16.c"
        "proto_uart.c"
        "proto_tx.c"
        "app_proto.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        freertos
        esp_system
        driver
        esp_driver_uart
        esp_driver_gpio
        esp_timer
)
