/**
 * @file test_crc_crosslink.c
 * @brief 跨链路 CRC / 帧格式一致性单测（验收 3）
 *
 * 三份实现是各自独立的代码（不同算法、不同设备）：
 *   C3   firmware/c3-node/main/frame.c              —— 逐位法 CRC + frame_build
 *   F103 firmware/f103-node/APP/comm/crc16.c        —— 运行时建表 CRC
 *   P4   firmware/p4-hub/components/protocol/crc16.c—— 常量表 CRC
 *
 * 本测试在 PC 上（mingw gcc）把三份源码一起编进同一个进程，逐字节对拍：
 *   [1] 检查值 CRC("123456789") == 0x29B1
 *   [2] 空缓冲 CRC == 0xFFFF
 *   [3] 随机向量三方一致（长度 0..256）
 *   [4] C3 frame_build 与 F103 proto_frame_build 字节级一致
 *   [5] C3 组帧 → F103 状态机解析，字段与 CRC 正确
 *   [6] 负向：载荷篡改 / LEN 超限 / 版本不符 / 前导垃圾后重同步
 *
 * 全部通过返回 0，任一失败返回 1。
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "proto_frame.h"   /* F103 纯逻辑层：proto_parser_t / proto_frame_build */

/* ── 三份实现的入口（F103 侧在编译时被宏改名，见 run.ps1） ── */
uint16_t frame_crc16(const uint8_t *data, uint16_t len);                    /* C3 */
uint16_t frame_build(uint8_t *out, size_t cap, uint8_t type, uint16_t seq,
                     const uint8_t *payload, uint16_t len);                 /* C3 */
uint16_t f103_crc16_compute(const uint8_t *buf, uint16_t len);              /* F103 */
uint16_t crc16_compute(const uint8_t *data, size_t len);                    /* P4 */

/* ── 极简断言 ── */
static int g_cases;
static int g_fail;

#define CHECK(cond, fmt, ...)                                                  \
    do {                                                                       \
        g_cases++;                                                             \
        if (!(cond)) {                                                         \
            g_fail++;                                                          \
            printf("  FAIL: " fmt "\n", ##__VA_ARGS__);                        \
        }                                                                      \
    } while (0)

/* ── 确定性伪随机（xorshift32），保证结果可复现 ── */
static uint32_t s_rng = 0x12345678u;
static uint8_t rnd8(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return (uint8_t)(s_rng & 0xFFu);
}

/* ════════════════════════════════════════════════════
 *  [1][2] 检查值与边界值
 * ════════════════════════════════════════════════════ */
static void test_kat(void)
{
    static const uint8_t kat[] = "123456789";

    printf("[1] 检查值 CRC(\"123456789\") == 0x29B1\n");
    uint16_t a = frame_crc16(kat, 9);
    uint16_t b = f103_crc16_compute(kat, 9);
    uint16_t c = crc16_compute(kat, (size_t)9);
    CHECK(a == 0x29B1, "C3   = 0x%04X", a);
    CHECK(b == 0x29B1, "F103 = 0x%04X", b);
    CHECK(c == 0x29B1, "P4   = 0x%04X", c);

    printf("[2] 空缓冲 CRC == 0xFFFF（初值，无数据）\n");
    CHECK(frame_crc16(NULL, 0) == 0xFFFF, "C3   = 0x%04X", frame_crc16(NULL, 0));
    CHECK(f103_crc16_compute(NULL, 0) == 0xFFFF, "F103 = 0x%04X",
          f103_crc16_compute(NULL, 0));
    CHECK(crc16_compute(NULL, 0) == 0xFFFF, "P4   = 0x%04X",
          crc16_compute(NULL, 0));
}

/* ════════════════════════════════════════════════════
 *  [3] 随机向量三方一致
 * ════════════════════════════════════════════════════ */
static void test_three_way(void)
{
    static uint8_t buf[PROTO_MAX_PAYLOAD];

    printf("[3] 随机向量三方一致（长度 0..%u）\n", (unsigned)PROTO_MAX_PAYLOAD);
    for (int len = 0; len <= (int)PROTO_MAX_PAYLOAD; len++) {
        for (int i = 0; i < len; i++) {
            buf[i] = rnd8();
        }
        uint16_t a = frame_crc16(buf, (uint16_t)len);
        uint16_t b = f103_crc16_compute(buf, (uint16_t)len);
        uint16_t c = crc16_compute(buf, (size_t)len);
        CHECK(a == b, "len=%d C3=0x%04X F103=0x%04X", len, a, b);
        CHECK(b == c, "len=%d F103=0x%04X P4=0x%04X", len, b, c);
    }
}

/* ════════════════════════════════════════════════════
 *  [4] 组帧字节级一致
 * ════════════════════════════════════════════════════ */
static void test_frame_bytes(void)
{
    static const uint8_t types[] = {0x00, 0x01, 0x10, 0x12, 0x7E, 0xFF};
    static const int lens[] = {0, 1, 2, 8, 33, 255, 256};
    uint8_t payload[PROTO_MAX_PAYLOAD];
    uint8_t fa[PROTO_MAX_FRAME];
    uint8_t fb[PROTO_MAX_FRAME];

    printf("[4] C3 frame_build 与 F103 proto_frame_build 字节级一致\n");
    for (size_t ti = 0; ti < sizeof(types) / sizeof(types[0]); ti++) {
        for (size_t li = 0; li < sizeof(lens) / sizeof(lens[0]); li++) {
            uint8_t  type = types[ti];
            uint16_t seq  = (uint16_t)(0x1234u + ti * 7u + li);
            int      len  = lens[li];

            for (int i = 0; i < len; i++) {
                payload[i] = rnd8();
            }

            uint16_t la = frame_build(fa, sizeof(fa), type, seq,
                                      len ? payload : NULL, (uint16_t)len);
            uint16_t lb = proto_frame_build(type, seq, len ? payload : NULL,
                                            (uint16_t)len, fb);

            CHECK(la == (uint16_t)(PROTO_HEADER_SIZE + len + PROTO_CRC_SIZE),
                  "type=0x%02X len=%d C3 长度=%u", type, len, la);
            CHECK(la == lb, "type=0x%02X len=%d C3=%u F103=%u",
                  type, len, la, lb);
            CHECK(memcmp(fa, fb, la) == 0,
                  "type=0x%02X len=%d 帧字节不一致", type, len);
            CHECK(fa[0] == PROTO_SOF1 && fa[1] == PROTO_SOF2
                      && fa[2] == PROTO_VER,
                  "type=0x%02X len=%d 帧头=%02X %02X %02X",
                  type, len, fa[0], fa[1], fa[2]);
        }
    }
}

/* ════════════════════════════════════════════════════
 *  [5] C3 组帧 → F103 状态机解析
 * ════════════════════════════════════════════════════ */
static proto_frame_t g_rx;
static int           g_rx_count;

static void on_rx(const proto_frame_t *f, void *user)
{
    (void)user;
    g_rx = *f;
    g_rx_count++;
}

static void test_parse_roundtrip(void)
{
    static uint8_t payload[16];
    uint8_t frame[PROTO_MAX_FRAME];
    proto_parser_t p;

    printf("[5] C3 组帧 → F103 状态机解析\n");
    for (size_t i = 0; i < sizeof(payload); i++) {
        payload[i] = rnd8();
    }

    uint16_t n = frame_build(frame, sizeof(frame), PROTO_SENSOR_REPORT,
                             (uint16_t)0x00AB, payload, (uint16_t)sizeof(payload));
    g_rx_count = 0;
    memset(&g_rx, 0, sizeof(g_rx));
    proto_parser_init(&p, on_rx, NULL);
    proto_parser_feed_buf(&p, frame, n);

    CHECK(n == (uint16_t)(PROTO_HEADER_SIZE + sizeof(payload) + PROTO_CRC_SIZE),
          "帧长=%u", n);
    CHECK(p.stats.rx_frames == 1, "rx_frames=%u", (unsigned)p.stats.rx_frames);
    CHECK(p.stats.crc_errors == 0, "crc_errors=%u", (unsigned)p.stats.crc_errors);
    CHECK(g_rx_count == 1, "回调次数=%d", g_rx_count);
    CHECK(g_rx.type == PROTO_SENSOR_REPORT, "type=0x%02X", g_rx.type);
    CHECK(g_rx.seq == 0x00AB, "seq=0x%04X", g_rx.seq);
    CHECK(g_rx.len == sizeof(payload), "len=%u", g_rx.len);
    CHECK(memcmp(g_rx.payload, payload, sizeof(payload)) == 0, "载荷不一致");
    CHECK(g_rx.crc == frame_crc16(frame + 2, (uint16_t)(n - 4)),
          "帧内 CRC=0x%04X 与 C3 复算 0x%04X 不一致",
          g_rx.crc, frame_crc16(frame + 2, (uint16_t)(n - 4)));
}

/* ════════════════════════════════════════════════════
 *  [6] 负向用例
 * ════════════════════════════════════════════════════ */
static void test_negative(void)
{
    static uint8_t payload[8];
    uint8_t frame[PROTO_MAX_FRAME];
    proto_parser_t p;

    printf("[6] 负向用例\n");

    /* 6.1 载荷被篡改一个字节 → CRC 失败，不投递 */
    for (size_t i = 0; i < sizeof(payload); i++) {
        payload[i] = rnd8();
    }
    uint16_t n = frame_build(frame, sizeof(frame), PROTO_SENSOR_REPORT,
                             0x0001, payload, (uint16_t)sizeof(payload));
    frame[PROTO_HEADER_SIZE] ^= 0xFFu;          /* 翻转载荷第一个字节 */
    g_rx_count = 0;
    proto_parser_init(&p, on_rx, NULL);
    proto_parser_feed_buf(&p, frame, n);
    CHECK(p.stats.crc_errors == 1, "篡改载荷 crc_errors=%u",
          (unsigned)p.stats.crc_errors);
    CHECK(p.stats.rx_frames == 0, "篡改载荷不应投递，rx_frames=%u",
          (unsigned)p.stats.rx_frames);
    CHECK(g_rx_count == 0, "篡改载荷不应触发回调");

    /* 6.2 LEN 超限（0x0101 = 257 > 256）→ len_errors，不得写越载荷缓冲 */
    {
        uint8_t bad[] = {PROTO_SOF1, PROTO_SOF2, PROTO_VER, 0x10u,
                         0x00u, 0x00u, 0x01u, 0x01u};
        proto_parser_init(&p, on_rx, NULL);
        proto_parser_feed_buf(&p, bad, (uint16_t)sizeof(bad));
        CHECK(p.stats.len_errors == 1, "超长 LEN len_errors=%u",
              (unsigned)p.stats.len_errors);
        CHECK(p.stats.rx_frames == 0, "超长 LEN 不应投递");
    }

    /* 6.3 版本不符 → len_errors */
    {
        uint8_t bad[] = {PROTO_SOF1, PROTO_SOF2, 0x03u, 0x10u,
                         0x00u, 0x00u, 0x00u, 0x00u};
        proto_parser_init(&p, on_rx, NULL);
        proto_parser_feed_buf(&p, bad, (uint16_t)sizeof(bad));
        CHECK(p.stats.len_errors == 1, "版本不符 len_errors=%u",
              (unsigned)p.stats.len_errors);
    }

    /* 6.4 前导垃圾后重同步：噪声 + 半截 SOF + 正常帧，仍应收到 1 帧 */
    {
        static const uint8_t noise[] = {0x00u, 0x11u, PROTO_SOF1, PROTO_SOF1,
                                        0x00u, 0x55u, 0xAAu};
        uint8_t good[PROTO_MAX_FRAME];
        uint16_t gn = frame_build(good, sizeof(good), PROTO_SENSOR_REPORT,
                                  0x0002, NULL, 0);
        g_rx_count = 0;
        proto_parser_init(&p, on_rx, NULL);
        proto_parser_feed_buf(&p, noise, (uint16_t)sizeof(noise));
        proto_parser_feed_buf(&p, good, gn);
        CHECK(p.stats.rx_frames == 1, "重同步后 rx_frames=%u",
              (unsigned)p.stats.rx_frames);
        CHECK(p.stats.crc_errors == 0, "重同步后 crc_errors=%u",
              (unsigned)p.stats.crc_errors);
        CHECK(g_rx_count == 1, "重同步后回调次数=%d", g_rx_count);
    }
}

int main(void)
{
    printf("跨链路 CRC / 帧格式一致性单测（验收 3）\n");
    printf("实现：C3 frame.c(逐位) / F103 crc16.c(建表) / P4 crc16.c(常量表)\n\n");

    test_kat();
    test_three_way();
    test_frame_bytes();
    test_parse_roundtrip();
    test_negative();

    printf("\n结果：%d 个断言，%d 个失败\n", g_cases, g_fail);
    if (g_fail == 0) {
        printf("ALL PASS\n");
        return 0;
    }
    printf("FAILED\n");
    return 1;
}
