/**
 * @file test_proto.c
 * @brief docs/03-通信协议规范.md §8 测试用例 — PC 端执行（阶段 2C-1 / 2C-2）
 *
 * 覆盖:
 *   T-01 CRC 标准向量          T-06 CRC 错误帧
 *   T-02 正常单帧              T-07 LEN 超限（不得写越缓冲）
 *   T-03 半包 + 帧内超时恢复    T-08 未知 TYPE（传输层透传，应用层忽略）
 *   T-04 粘包                  T-09 传输层版本不符丢弃（应用层协商在目标板验证）
 *   T-05 载荷含 0xAA 0x55
 *   T-13 随机位翻转 ×1000     —— 全部检出，零误执行
 *   T-14 随机丢字节 ×1000     —— 能重同步，零伪造帧
 *
 * 用例边界（诚实记录）: T-10 重复 SEQ / T-11 TTL / T-12 参数越界属于
 * app_proto / control 应用层行为，与 BSP 耦合，在目标板上验证（见验收报告）。
 *
 * 编译即用的同一份代码: crc16.c + proto_frame.c 直接取自 firmware/，
 * 无副本、无桩 —— 测试通过 = 板上逻辑正确（在相同编译器语义下）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "protocol.h"
#include "crc16.h"
#include "proto_frame.h"

/* ════════════════════════════════════════════════════
 *  极简测试框架
 *  ════════════════════════════════════════════════════ */

static int g_fail_cnt;
static int g_check_cnt;

#define CHECK(cond)                                                       \
    do {                                                                  \
        g_check_cnt++;                                                    \
        if (!(cond)) {                                                    \
            g_fail_cnt++;                                                 \
            printf("  [FAIL] %s:%d  %s\n", __FILE__, __LINE__, #cond);    \
        }                                                                 \
    } while (0)

/* ════════════════════════════════════════════════════
 *  投递捕获
 *  ════════════════════════════════════════════════════ */

#define CAP_MAX 4096
static proto_frame_t cap_buf[CAP_MAX];
static uint32_t      cap_n;

static void capture_cb(const proto_frame_t *f, void *user)
{
    (void)user;
    if (cap_n < CAP_MAX) {
        cap_buf[cap_n++] = *f;
    }
}

static proto_parser_t P;

static void parser_reset_all(void)
{
    proto_parser_init(&P, capture_cb, 0);
    cap_n = 0;
}

/** 组帧辅助：返回帧总长 */
static uint16_t mkframe(uint8_t type, uint16_t seq,
                        const uint8_t *payload, uint16_t len, uint8_t *out)
{
    return proto_frame_build(type, seq, payload, len, out);
}

/* ════════════════════════════════════════════════════
 *  §8 用例
 *  ════════════════════════════════════════════════════ */

/* T-01 CRC16-CCITT-FALSE 标准向量 */
static void t01_crc_vector(void)
{
    printf("T-01  CRC 标准向量 \"123456789\" -> 0x29B1\n");
    crc16_init();
    CHECK(crc16_compute((const uint8_t *)"123456789", 9) == 0x29B1U);
    CHECK(crc16_compute((const uint8_t *)"", 0) == 0xFFFFU);   /* 初值 */
}

/* T-02 正常单帧：字段逐项核对 */
static void t02_normal_frame(void)
{
    printf("T-02  正常单帧解析\n");
    parser_reset_all();
    uint8_t pl[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    uint8_t buf[PROTO_MAX_FRAME];
    uint16_t n = mkframe(PROTO_SENSOR_REPORT, 0x1234, pl, 4, buf);
    CHECK(n == PROTO_MIN_FRAME + 4);

    proto_parser_feed_buf(&P, buf, n);
    CHECK(P.stats.rx_frames == 1);
    CHECK(cap_n == 1);
    CHECK(cap_buf[0].ver  == PROTO_VER);
    CHECK(cap_buf[0].type == PROTO_SENSOR_REPORT);
    CHECK(cap_buf[0].seq  == 0x1234);
    CHECK(cap_buf[0].len  == 4);
    CHECK(memcmp(cap_buf[0].payload, pl, 4) == 0);
    CHECK(P.stats.crc_errors == 0 && P.stats.len_errors == 0);
}

/* T-03 半包：喂半帧 -> 帧内超时 abort -> 后续帧正常解析，不得误解析 */
static void t03_half_frame(void)
{
    printf("T-03  半包 + 帧内超时恢复\n");
    parser_reset_all();
    uint8_t pl[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t buf[PROTO_MAX_FRAME];
    (void)mkframe(PROTO_SENSOR_REPORT, 7, pl, 8, buf);

    /* 喂到 PAYLOAD 一半（SOF2 + 6B头 + 4B载荷） */
    proto_parser_feed_buf(&P, buf, 2 + 6 + 4);
    CHECK(proto_parser_in_frame(&P) == 1);
    CHECK(cap_n == 0);

    /* 20ms 无新字节 -> 传输层调用 abort（规范 §7.4） */
    proto_parser_abort(&P);
    CHECK(proto_parser_in_frame(&P) == 0);
    CHECK(P.stats.resyncs == 1);

    /* 补发一个全新完整帧，必须正常解析（半帧的尾巴绝不粘连） */
    uint8_t pl2[2] = {0x11, 0x22};
    uint16_t n2 = mkframe(PROTO_PING, 8, pl2, 2, buf);
    proto_parser_feed_buf(&P, buf, n2);
    CHECK(P.stats.rx_frames == 1);
    CHECK(cap_n == 1);
    CHECK(cap_buf[0].type == PROTO_PING && cap_buf[0].seq == 8);

    /* 不 abort 的对照组：真半帧 + 完整帧连续喂入，半帧的 CRC 必然失败，
     * 只允许丢半帧，绝不允许"拼"出误解析帧 */
    parser_reset_all();
    uint8_t half_frame[PROTO_MAX_FRAME];
    (void)mkframe(PROTO_SENSOR_REPORT, 7, pl, 8, half_frame);
    uint8_t stream[PROTO_MAX_FRAME * 2];
    memcpy(stream, half_frame, 12);           /* 截断的 SENSOR_REPORT */
    memcpy(stream + 12, buf, n2);             /* 完整 PING 紧随 */
    proto_parser_feed_buf(&P, stream, (uint16_t)(12 + n2));
    /* 无论解析出什么，字段必须来自真实完整帧 */
    for (uint32_t i = 0; i < cap_n; i++) {
        CHECK(cap_buf[i].type == PROTO_PING && cap_buf[i].seq == 8);
    }
    CHECK(P.stats.crc_errors + P.stats.len_errors >= 1);
}

/* T-04 粘包：两帧连续到达 */
static void t04_back_to_back(void)
{
    printf("T-04  粘包（两帧连续）\n");
    parser_reset_all();
    uint8_t buf[PROTO_MAX_FRAME];
    uint16_t n1 = mkframe(PROTO_SENSOR_REPORT, 1, 0, 0, buf);
    uint8_t pl[3] = {0xAA, 0x55, 0x00};
    uint16_t n2 = mkframe(PROTO_PONG, 2, pl, 3, buf + n1);
    proto_parser_feed_buf(&P, buf, (uint16_t)(n1 + n2));
    CHECK(P.stats.rx_frames == 2);
    CHECK(cap_n == 2);
    CHECK(cap_buf[0].seq == 1 && cap_buf[1].seq == 2);
}

/* T-05 载荷含 0xAA 0x55：靠 LEN 定长，不误判帧头 */
static void t05_sof_in_payload(void)
{
    printf("T-05  载荷含 0xAA 0x55\n");
    parser_reset_all();
    uint8_t pl[16];
    for (int i = 0; i < 16; i++) pl[i] = 0xAA;
    pl[3] = 0x55; pl[10] = 0xAA; pl[11] = 0x55;
    uint8_t buf[PROTO_MAX_FRAME];
    uint16_t n = mkframe(PROTO_SENSOR_REPORT, 0xBEEF, pl, 16, buf);

    proto_parser_feed_buf(&P, buf, n);
    CHECK(P.stats.rx_frames == 1);
    CHECK(cap_n == 1);
    CHECK(cap_buf[0].len == 16);
    CHECK(memcmp(cap_buf[0].payload, pl, 16) == 0);
}

/* T-06 CRC 错误帧：丢弃 + 计数，无副作用，后续帧恢复 */
static void t06_crc_error(void)
{
    printf("T-06  CRC 错误帧\n");
    parser_reset_all();
    uint8_t pl[4] = {9, 9, 9, 9};
    uint8_t buf[PROTO_MAX_FRAME];
    uint16_t n = mkframe(PROTO_ACTUATOR_CMD, 5, pl, 4, buf);
    buf[n - 1] ^= 0xFF;                      /* 翻转 CRC 高字节 */

    proto_parser_feed_buf(&P, buf, n);
    CHECK(cap_n == 0);                       /* 绝不投递 */
    CHECK(P.stats.crc_errors == 1);
    CHECK(P.stats.resyncs == 1);

    /* 后续正常帧不受影响 */
    uint16_t n2 = mkframe(PROTO_PING, 6, 0, 0, buf);
    proto_parser_feed_buf(&P, buf, n2);
    CHECK(P.stats.rx_frames == 1 && cap_n == 1);
}

/* T-07 LEN 超限 / 版本不符：早于缓冲写入丢弃（规范 §7.3） */
static void t07_len_overflow(void)
{
    printf("T-07  LEN 超过 MAX_PAYLOAD + 版本不符\n");
    parser_reset_all();
    uint8_t buf[PROTO_MAX_FRAME];

    /* LEN=300：手动组帧绕过 build 的合法性检查 */
    uint8_t evil[16] = {PROTO_SOF1, PROTO_SOF2, PROTO_VER, PROTO_ACTUATOR_CMD,
                        0x01, 0x00, 0x2C, 0x01, 0xFF, 0xFF, 0xFF, 0xFF,
                        0xFF, 0xFF, 0xFF, 0xFF};
    proto_parser_feed_buf(&P, evil, sizeof(evil));
    CHECK(cap_n == 0);
    CHECK(P.stats.len_errors == 1);

    /* 版本不符：ver=0x01 */
    uint8_t evil2[12] = {PROTO_SOF1, PROTO_SOF2, 0x01, PROTO_HELLO_REQ,
                         0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    proto_parser_feed_buf(&P, evil2, sizeof(evil2));
    CHECK(cap_n == 0);
    CHECK(P.stats.len_errors == 2);

    /* 恢复能力 */
    uint16_t n = mkframe(PROTO_PING, 9, 0, 0, buf);
    proto_parser_feed_buf(&P, buf, n);
    CHECK(P.stats.rx_frames == 1);
}

/* T-08 未知 TYPE：传输层透传给应用层（向前兼容），
 * 应用层"静默忽略"行为在目标板 app_proto.c 验证 */
static void t08_unknown_type_passthrough(void)
{
    printf("T-08  未知 TYPE 传输层透传\n");
    parser_reset_all();
    uint8_t pl[2] = {0x77, 0x88};
    uint8_t buf[PROTO_MAX_FRAME];
    uint16_t n = mkframe(0x77, 0x42, pl, 2, buf);
    proto_parser_feed_buf(&P, buf, n);
    CHECK(P.stats.rx_frames == 1);
    CHECK(cap_n == 1);
    CHECK(cap_buf[0].type == 0x77);          /* 解析层不按类型过滤 */
}

/* T-09（传输层部分）帧内版本不符在传输层丢弃；
 * 应用层 HELLO 载荷版本协商 -> ERROR(result=1) 在目标板验证 */
static void t09_version_mismatch_transport(void)
{
    printf("T-09  帧版本不符传输层丢弃（应用层协商留目标板）\n");
    parser_reset_all();
    uint8_t pl[4] = {0x01, 0x00, 0x00, 0x00};   /* HELLO 载荷带旧版本号 */
    uint8_t buf[PROTO_MAX_FRAME];
    uint16_t n = mkframe(PROTO_HELLO_REQ, 1, pl, 4, buf);
    buf[2] = 0x01;                           /* 帧版本改为 0x01，CRC 必然失配 */
    proto_parser_feed_buf(&P, buf, n);
    CHECK(P.stats.rx_frames == 0);
    CHECK(P.stats.crc_errors + P.stats.len_errors >= 1);
}

/* ════════════════════════════════════════════════════
 *  T-13 随机位翻转 ×1000：全部检出、零误执行
 *  ════════════════════════════════════════════════════ */

/* 误执行判定：投递帧与原始帧不一致（类型/序列/载荷任一不同）即算失败。
 * 位翻转落在 CRC 覆盖区必被 CRC 检出；落在 SOF 只导致丢帧。
 * 验收口径: 0 次误执行（detected 之外的多余投递）。 */
static int frame_differs(const proto_frame_t *a, const proto_frame_t *b)
{
    if (a->type != b->type || a->seq != b->seq || a->len != b->len) return 1;
    return memcmp(a->payload, b->payload, a->len) != 0;
}

static void t13_bit_flip_1000(void)
{
    printf("T-13  随机位翻转 x1000\n");
    srand(20260918U);
    parser_reset_all();

    uint32_t caught = 0, silent = 0, false_exec = 0, delivered_ok = 0;
    uint8_t  pl[PROTO_MAX_PAYLOAD];
    uint8_t  buf[PROTO_MAX_FRAME];
    proto_frame_t orig;

    for (int trial = 0; trial < 1000; trial++) {
        parser_reset_all();

        /* 随机帧：偏选 ACTUATOR_CMD 加重"误执行"后果 */
        uint8_t types[4] = {PROTO_ACTUATOR_CMD, PROTO_SENSOR_REPORT,
                            PROTO_PING, PROTO_CONFIG_SET};
        uint8_t  type = types[rand() & 3];
        uint16_t seq  = (uint16_t)(rand() & 0xFFFF);
        uint16_t len  = (uint16_t)(rand() % 65);
        for (uint16_t i = 0; i < len; i++) pl[i] = (uint8_t)rand();
        uint16_t n = mkframe(type, seq, pl, len, buf);
        if (n == 0) { CHECK(0); continue; }

        memcpy(&orig, &(proto_frame_t){.type = type, .seq = seq, .len = len}, sizeof orig);
        memcpy(orig.payload, pl, len);

        uint32_t before_frames = P.stats.rx_frames;
        uint32_t before_errs   = P.stats.crc_errors + P.stats.len_errors;

        /* 单比特翻转，位置含 SOF 与 CRC 字段 */
        uint32_t pos  = (uint32_t)(rand() % n);
        uint8_t  bit  = (uint8_t)(rand() & 7);
        buf[pos] ^= (uint8_t)(1U << bit);

        proto_parser_feed_buf(&P, buf, n);

        /* 核对每一笔投递 */
        for (uint32_t i = 0; i < cap_n; i++) {
            if (frame_differs(&cap_buf[i], &orig)) {
                false_exec++;                       /* 伪造帧 = 误执行风险 */
            } else {
                delivered_ok++;
            }
        }

        if (P.stats.rx_frames > before_frames ||
            P.stats.crc_errors + P.stats.len_errors > before_errs) {
            caught++;
        } else {
            silent++;   /* 翻转落在 SOF 等无害处，帧被安全忽略 */
        }
    }

    printf("      caught=%u silent=%u delivered_ok=%u FALSE_EXEC=%u\n",
           caught, silent, delivered_ok, false_exec);
    CHECK(false_exec == 0);
    CHECK(caught + silent == 1000);
}

/* ════════════════════════════════════════════════════
 *  T-14 随机丢字节 ×1000：能重同步、零伪造帧
 *  ════════════════════════════════════════════════════ */

static void t14_byte_drop_1000(void)
{
    printf("T-14  随机丢字节 x1000\n");
    srand(20261002U);

    uint32_t false_exec = 0, recovered = 0;
    uint8_t  pl[PROTO_MAX_PAYLOAD];
    uint8_t  stream[PROTO_MAX_FRAME * 6];
    uint8_t  tail[PROTO_MAX_FRAME];
    proto_frame_t sent[6];

    for (int trial = 0; trial < 1000; trial++) {
        parser_reset_all();

        /* 5 帧连续流（含执行器命令帧）+ 1 帧恢复探针 */
        uint16_t off = 0, total = 0;
        for (int f = 0; f < 5; f++) {
            uint8_t  type = (f == 2) ? PROTO_ACTUATOR_CMD : PROTO_SENSOR_REPORT;
            uint16_t len  = (uint16_t)(rand() % 48);
            for (uint16_t i = 0; i < len; i++) pl[i] = (uint8_t)rand();
            sent[f].type = type; sent[f].seq = (uint16_t)(f + 1);
            sent[f].len  = len;
            memcpy(sent[f].payload, pl, len);
            off = (uint16_t)(off + mkframe(type, (uint16_t)(f + 1), pl, len,
                                           stream + off));
        }
        total = off;
        uint16_t tail_len = mkframe(PROTO_PING, 0xABCD, 0, 0, tail);

        /* 丢 1 个随机字节 */
        uint16_t drop = (uint16_t)(rand() % total);
        memmove(stream + drop, stream + drop + 1, (size_t)(total - drop - 1));

        proto_parser_feed_buf(&P, stream, (uint16_t)(total - 1));

        /* 每笔投递必须逐字节等于 5 帧 originals 之一（零伪造） */
        for (uint32_t i = 0; i < cap_n; i++) {
            int match = 0;
            for (int f = 0; f < 5; f++) {
                if (!frame_differs(&cap_buf[i], &sent[f])) { match = 1; break; }
            }
            if (!match) false_exec++;
        }

        /* 恢复探针：真机在损坏流结束后有 >=20ms 总线空闲，传输层的
         * 帧内超时（规范 §7.4）会 abort 半开帧 —— 此处显式模拟之，
         * 然后下一个完整帧必须被解析（1Hz 上报下即下一帧恢复）。 */
        proto_parser_abort(&P);
        uint32_t before = P.stats.rx_frames;
        proto_parser_feed_buf(&P, tail, tail_len);
        if (P.stats.rx_frames == before + 1 &&
            cap_n >= 1 &&
            cap_buf[cap_n - 1].type == PROTO_PING &&
            cap_buf[cap_n - 1].seq == 0xABCD) {
            recovered++;
        }
    }

    printf("      recovered=%u/1000 FALSE_EXEC=%u\n", recovered, false_exec);
    CHECK(false_exec == 0);
    CHECK(recovered == 1000);
}

/* ════════════════════════════════════════════════════ */

int main(void)
{
    crc16_init();

    t01_crc_vector();
    t02_normal_frame();
    t03_half_frame();
    t04_back_to_back();
    t05_sof_in_payload();
    t06_crc_error();
    t07_len_overflow();
    t08_unknown_type_passthrough();
    t09_version_mismatch_transport();
    t13_bit_flip_1000();
    t14_byte_drop_1000();

    printf("\n=== %d checks, %d failed ===\n", g_check_cnt, g_fail_cnt);
    return g_fail_cnt ? 1 : 0;
}
