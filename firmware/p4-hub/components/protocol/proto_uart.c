/**
 * @file proto_uart.c
 * @brief UART 传输层 — 帧封装/解帧（ESP-IDF 驱动）
 *
 * 与 F103 侧 proto_uart.c 接口兼容，底层使用 ESP-IDF uart 驱动。
 * 字节级状态机：SOF1→SOF2→HEADER→PAYLOAD→CRC→完整帧回调。
 */
#include "proto_uart.h"
#include "crc16.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include <string.h>

static const char *TAG = "proto_uart";

/* ════════════════════════════════════════════════════
 *  UART 配置
 *  ════════════════════════════════════════════════════ */

#define PROTO_UART_NUM   CONFIG_SMART_HOME_PROTO_UART_NUM
#define PROTO_UART_TX    CONFIG_SMART_HOME_PROTO_UART_TX
#define PROTO_UART_RX    CONFIG_SMART_HOME_PROTO_UART_RX
#define PROTO_UART_BAUD  CONFIG_SMART_HOME_PROTO_UART_BAUD

#define RX_BUF_SIZE   1024
#define TX_BUF_SIZE   1024

/* ════════════════════════════════════════════════════
 *  解帧状态机
 *  ════════════════════════════════════════════════════ */

typedef enum {
    STATE_SOF1,
    STATE_SOF2,
    STATE_HEADER,
    STATE_PAYLOAD,
    STATE_CRC_LO,
    STATE_CRC_HI,
} rx_state_t;

static rx_state_t       s_rx_state = STATE_SOF1;
static uint8_t          s_rx_header[6];   /* VER TYPE SEQ(2) LEN(2) */
static uint8_t          s_rx_header_pos;
static proto_frame_t    s_rx_frame;
static uint16_t         s_rx_payload_pos;
static uint16_t         s_rx_crc;
static uint16_t         s_rx_computed_crc;

static proto_rx_cb_t    s_rx_cb;
static proto_stats_t    s_stats;

/* ── 声明 ── */
static uint16_t crc16_compute_partial(const uint8_t *data, size_t len,
                                       uint16_t initial_crc);

/* ════════════════════════════════════════════════════
 *  初始化
 *  ════════════════════════════════════════════════════ */

void ProtoUart_Init(void)
{
    const uart_config_t uart_config = {
        .baud_rate  = PROTO_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    /* 顺序很重要：param_config → set_pin → driver_install */
    ESP_ERROR_CHECK(uart_param_config(PROTO_UART_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(PROTO_UART_NUM,
                                 PROTO_UART_TX, PROTO_UART_RX,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(PROTO_UART_NUM,
                                        RX_BUF_SIZE, TX_BUF_SIZE,
                                        0, NULL, 0));

    ESP_LOGI(TAG, "Initialized UART%d TX=GPIO%d RX=GPIO%d baud=%d",
             PROTO_UART_NUM, PROTO_UART_TX, PROTO_UART_RX, PROTO_UART_BAUD);

    memset(&s_stats, 0, sizeof(s_stats));

    /* 环回诊断 */
    uart_flush_input(PROTO_UART_NUM);

    uint8_t test_tx[5] = { 0xA5, 0x5A, 0x01, 0x02, 0x03 };
    uart_write_bytes(PROTO_UART_NUM, test_tx, sizeof(test_tx));
    esp_err_t tx_err = uart_wait_tx_done(PROTO_UART_NUM, pdMS_TO_TICKS(200));

    vTaskDelay(pdMS_TO_TICKS(50));

    uint8_t test_rx[5] = {0};
    int rx_len = uart_read_bytes(PROTO_UART_NUM, test_rx, sizeof(test_rx),
                                 pdMS_TO_TICKS(200));
    ESP_LOGI(TAG, "LOOPBACK  tx_done=%d rx=%d [%02X %02X %02X %02X %02X]",
             (int)tx_err, rx_len,
             (unsigned)test_rx[0], (unsigned)test_rx[1],
             (unsigned)test_rx[2], (unsigned)test_rx[3],
             (unsigned)test_rx[4]);

    /* 内部环回 */
    uart_set_loop_back(PROTO_UART_NUM, true);
    uart_flush_input(PROTO_UART_NUM);

    uint8_t int_test[3] = { 0xDE, 0xAD, 0xBE };
    uart_write_bytes(PROTO_UART_NUM, int_test, 3);
    uart_wait_tx_done(PROTO_UART_NUM, pdMS_TO_TICKS(200));

    uint8_t int_rx[3] = {0};
    int int_rx_len = uart_read_bytes(PROTO_UART_NUM, int_rx, 3,
                                     pdMS_TO_TICKS(200));
    ESP_LOGI(TAG, "INT-LOOPBACK  rx=%d [%02X %02X %02X]",
             int_rx_len,
             (unsigned)int_rx[0], (unsigned)int_rx[1], (unsigned)int_rx[2]);

    uart_set_loop_back(PROTO_UART_NUM, false);
    uart_flush_input(PROTO_UART_NUM);

    if (int_rx_len != 3 || int_rx[0] != 0xDE) {
        ESP_LOGE(TAG, "INT-LOOPBACK FAILED! UART%d broken", PROTO_UART_NUM);
    }
}

void ProtoUart_RegisterRxCb(proto_rx_cb_t cb)
{
    s_rx_cb = cb;
}

/* ════════════════════════════════════════════════════
 *  发送
 *  ════════════════════════════════════════════════════ */

void ProtoUart_SendFrame(uint8_t type, uint16_t seq,
                         const uint8_t *payload, uint16_t len)
{
    uint8_t buf[PROTO_MAX_FRAME];
    uint16_t off = 0;

    buf[off++] = PROTO_SOF1;
    buf[off++] = PROTO_SOF2;
    buf[off++] = PROTO_VER;
    buf[off++] = type;
    buf[off++] = (uint8_t)(seq & 0xFF);
    buf[off++] = (uint8_t)((seq >> 8) & 0xFF);
    buf[off++] = (uint8_t)(len & 0xFF);
    buf[off++] = (uint8_t)((len >> 8) & 0xFF);

    if (len > 0 && payload != NULL) {
        memcpy(&buf[off], payload, len);
    }

    uint16_t header_payload_len = off + len;
    uint16_t crc = crc16_compute(buf + 2, off + len - 2);
    buf[header_payload_len]     = (uint8_t)(crc & 0xFF);
    buf[header_payload_len + 1] = (uint8_t)((crc >> 8) & 0xFF);

    int total = header_payload_len + 2;
    int sent = uart_write_bytes(PROTO_UART_NUM, buf, total);
    if (type == PROTO_ACTUATOR_CMD) {
        ESP_LOGI(TAG, "TX ACTUATOR_CMD seq=%u bytes=%d", (unsigned)seq, sent);
    }
    if (sent > 0) {
        s_stats.tx_frames++;
        s_stats.tx_bytes += (uint32_t)sent;
    }
}

/* ════════════════════════════════════════════════════
 *  状态机：喂入一个字节
 *  ════════════════════════════════════════════════════ */

static void rx_feed_byte(uint8_t byte)
{
    s_stats.rx_bytes++;

    switch (s_rx_state) {

    case STATE_SOF1:
        if (byte == PROTO_SOF1) {
            s_rx_state = STATE_SOF2;
        }
        break;

    case STATE_SOF2:
        if (byte == PROTO_SOF2) {
            s_rx_state       = STATE_HEADER;
            s_rx_header_pos  = 0;
            memset(s_rx_header, 0, sizeof(s_rx_header));
        } else if (byte != PROTO_SOF1) {
            s_rx_state = STATE_SOF1;
        }
        break;

    case STATE_HEADER:
        s_rx_header[s_rx_header_pos++] = byte;
        if (s_rx_header_pos >= 6) {
            s_rx_frame.ver  = s_rx_header[0];
            s_rx_frame.type = s_rx_header[1];
            s_rx_frame.seq  = (uint16_t)s_rx_header[2]
                            | ((uint16_t)s_rx_header[3] << 8);
            s_rx_frame.len  = (uint16_t)s_rx_header[4]
                            | ((uint16_t)s_rx_header[5] << 8);

            if (s_rx_frame.ver != PROTO_VER) {
                s_stats.rx_ver_mismatch++;
                s_rx_state = STATE_SOF1;
                break;
            }

            if (s_rx_frame.len > PROTO_MAX_PAYLOAD) {
                s_stats.rx_too_long++;
                s_rx_state = STATE_SOF1;
                break;
            }

            if (s_rx_frame.len == 0) {
                s_rx_computed_crc = crc16_compute(s_rx_header, 6);
                s_rx_state = STATE_CRC_LO;
            } else {
                s_rx_payload_pos = 0;
                memset(s_rx_frame.payload, 0, PROTO_MAX_PAYLOAD);
                s_rx_state = STATE_PAYLOAD;
            }
        }
        break;

    case STATE_PAYLOAD:
        s_rx_frame.payload[s_rx_payload_pos++] = byte;
        if (s_rx_payload_pos >= s_rx_frame.len) {
            s_rx_computed_crc = crc16_compute(s_rx_header, 6);
            s_rx_computed_crc = crc16_compute_partial(
                s_rx_frame.payload, s_rx_frame.len, s_rx_computed_crc);
            s_rx_state = STATE_CRC_LO;
        }
        break;

    case STATE_CRC_LO:
        s_rx_crc = byte;
        s_rx_state = STATE_CRC_HI;
        break;

    case STATE_CRC_HI: {
        s_rx_crc |= (uint16_t)byte << 8;
        s_rx_frame.crc = s_rx_crc;

        if (s_rx_crc == s_rx_computed_crc) {
            s_stats.rx_frames++;
            if (s_rx_cb) {
                s_rx_cb(&s_rx_frame);
            }
        } else {
            s_stats.rx_crc_err++;
        }
        s_rx_state = STATE_SOF1;
        break;
    }
    }
}

/* ════════════════════════════════════════════════════
 *  辅助：CRC 分步计算
 *  ════════════════════════════════════════════════════ */

static uint16_t crc16_compute_partial(const uint8_t *data, size_t len,
                                       uint16_t initial_crc)
{
    static const uint16_t table[256] = {
        0x0000,0x1021,0x2042,0x3063,0x4084,0x50A5,0x60C6,0x70E7,
        0x8108,0x9129,0xA14A,0xB16B,0xC18C,0xD1AD,0xE1CE,0xF1EF,
        0x1231,0x0210,0x3273,0x2252,0x52B5,0x4294,0x72F7,0x62D6,
        0x9339,0x8318,0xB37B,0xA35A,0xD3BD,0xC39C,0xF3FF,0xE3DE,
        0x2462,0x3443,0x0420,0x1401,0x64E6,0x74C7,0x44A4,0x5485,
        0xA56A,0xB54B,0x8528,0x9509,0xE5EE,0xF5CF,0xC5AC,0xD58D,
        0x3653,0x2672,0x1611,0x0630,0x76D7,0x66F6,0x5695,0x46B4,
        0xB75B,0xA77A,0x9719,0x8738,0xF7DF,0xE7FE,0xD79D,0xC7BC,
        0x48C4,0x58E5,0x6886,0x78A7,0x0840,0x1861,0x2802,0x3823,
        0xC9CC,0xD9ED,0xE98E,0xF9AF,0x8948,0x9969,0xA90A,0xB92B,
        0x5AF5,0x4AD4,0x7AB7,0x6A96,0x1A71,0x0A50,0x3A33,0x2A12,
        0xDBFD,0xCBDC,0xFBBF,0xEB9E,0x9B79,0x8B58,0xBB3B,0xAB1A,
        0x6CA6,0x7C87,0x4CE4,0x5CC5,0x2C22,0x3C03,0x0C60,0x1C41,
        0xEDAE,0xFD8F,0xCDEC,0xDDCD,0xAD2A,0xBD0B,0x8D68,0x9D49,
        0x7E97,0x6EB6,0x5ED5,0x4EF4,0x3E13,0x2E32,0x1E51,0x0E70,
        0xFF9F,0xEFBE,0xDFDD,0xCFFC,0xBF1B,0xAF3A,0x9F59,0x8F78,
        0x9188,0x81A9,0xB1CA,0xA1EB,0xD10C,0xC12D,0xF14E,0xE16F,
        0x1080,0x00A1,0x30C2,0x20E3,0x5004,0x4025,0x7046,0x6067,
        0x83B9,0x9398,0xA3FB,0xB3DA,0xC33D,0xD31C,0xE37F,0xF35E,
        0x02B1,0x1290,0x22F3,0x32D2,0x4235,0x5214,0x6277,0x7256,
        0xB5EA,0xA5CB,0x95A8,0x8589,0xF56E,0xE54F,0xD52C,0xC50D,
        0x34E2,0x24C3,0x14A0,0x0481,0x7466,0x6447,0x5424,0x4405,
        0xA7DB,0xB7FA,0x8799,0x97B8,0xE75F,0xF77E,0xC71D,0xD73C,
        0x26D3,0x36F2,0x0691,0x16B0,0x6657,0x7676,0x4615,0x5634,
        0xD94C,0xC96D,0xF90E,0xE92F,0x99C8,0x89E9,0xB98A,0xA9AB,
        0x5844,0x4865,0x7806,0x6827,0x18C0,0x08E1,0x3882,0x28A3,
        0xCB7D,0xDB5C,0xEB3F,0xFB1E,0x8BF9,0x9BD8,0xABBB,0xBB9A,
        0x4A75,0x5A54,0x6A37,0x7A16,0x0AF1,0x1AD0,0x2AB3,0x3A92,
        0xFD2E,0xED0F,0xDD6C,0xCD4D,0xBDAA,0xAD8B,0x9DE8,0x8DC9,
        0x7C26,0x6C07,0x5C64,0x4C45,0x3CA2,0x2C83,0x1CE0,0x0CC1,
        0xEF1F,0xFF3E,0xCF5D,0xDF7C,0xAF9B,0xBFBA,0x8FD9,0x9FF8,
        0x6E17,0x7E36,0x4E55,0x5E74,0x2E93,0x3EB2,0x0ED1,0x1EF0,
    };
    uint16_t crc = initial_crc;
    for (size_t i = 0; i < len; i++) {
        uint8_t idx = (uint8_t)((crc >> 8) ^ data[i]);
        crc = (uint16_t)(crc << 8) ^ table[idx];
    }
    return crc;
}

/* ════════════════════════════════════════════════════
 *  收包轮询（由 RX 任务或主任务周期调用）
 *  ════════════════════════════════════════════════════ */

void ProtoUart_Poll(void)
{
    size_t avail = 0;
    uart_get_buffered_data_len(PROTO_UART_NUM, &avail);

    if (avail == 0) return;

    if (avail > RX_BUF_SIZE) avail = RX_BUF_SIZE;
    uint8_t buf[256];
    size_t to_read = (avail > sizeof(buf)) ? sizeof(buf) : avail;

    int len = uart_read_bytes(PROTO_UART_NUM, buf, to_read, 0);
    for (int i = 0; i < len; i++) {
        rx_feed_byte(buf[i]);
    }
}

/* ════════════════════════════════════════════════════
 *  统计
 *  ════════════════════════════════════════════════════ */

void ProtoUart_GetStats(proto_stats_t *out)
{
    if (out) memcpy(out, &s_stats, sizeof(s_stats));
}
