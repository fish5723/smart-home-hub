/*
 * SPDX-FileCopyrightText: 2025 Smart Home Hub
 * SPDX-License-Identifier: Apache-2.0
 *
 * jwp_reader 宿主机单元测试
 *
 * 目的：在不接任何硬件的情况下验证 JWP 容器解析器——
 *   * 32 字节头解析与字段校验（magic/version/header_size/pixel_format/尺寸）
 *   * 顺序读帧、帧长度校验、缓冲区不足的拒绝
 *   * frame_count 之后自动回跳
 *   * transition_offset（片头只播一次）回跳位置
 *   * seek_frame 随机定位
 *   * CRC32 与 zlib 一致（转换脚本用 zlib.crc32 写入 payload_crc32）
 *
 * 构建/运行: 见同目录 README 或 CMakeLists.txt（ctest）
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "jwp_reader.h"

/* 测试用的临时文件统一加这个前缀，跑完即删，避免污染仓库
 * （此前直接在仓库根运行会留下 t_*.jwp）。必须在首次使用之前定义。 */
#define TMP_PREFIX "jwp_test_tmp_"

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond, msg) do {                                    \
    g_checks++;                                                  \
    if (!(cond)) {                                               \
        g_failures++;                                            \
        fprintf(stderr, "  ✗ %s (%s:%d)\n", msg, __FILE__, __LINE__); \
    } else {                                                     \
        printf("  ✓ %s\n", msg);                                 \
    }                                                            \
} while (0)

#define CHECK_EQ_INT(a, b, msg) do {                             \
    g_checks++;                                                  \
    long _a = (long)(a), _b = (long)(b);                         \
    if (_a != _b) {                                              \
        g_failures++;                                            \
        fprintf(stderr, "  ✗ %s (got %ld, want %ld) (%s:%d)\n",  \
                msg, _a, _b, __FILE__, __LINE__);                \
    } else {                                                     \
        printf("  ✓ %s\n", msg);                                 \
    }                                                            \
} while (0)

/* ── 造一个 JWP 文件 ─────────────────────────────────────────── */
typedef struct {
    const char *path;
    uint16_t    width, height, fps100;
    uint32_t    frame_count;
    uint32_t    transition_offset;   /* 0 = 回到第一帧 */
    uint8_t     fill_seed;           /* 每帧字节值 = i + seed */
    uint32_t    frame_size;
} jwp_spec_t;

static void write_le16(FILE *f, uint16_t v)
{
    uint8_t b[2] = { (uint8_t)(v & 0xFF), (uint8_t)(v >> 8) };
    fwrite(b, 1, 2, f);
}

static void write_le32(FILE *f, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)(v & 0xFF), (uint8_t)((v >> 8) & 0xFF),
                     (uint8_t)((v >> 16) & 0xFF), (uint8_t)((v >> 24) & 0xFF) };
    fwrite(b, 1, 4, f);
}

static void build_jwp(const jwp_spec_t *spec)
{
    FILE *f = fopen(spec->path, "wb");
    assert(f);

    fwrite("JWP1", 1, 4, f);
    write_le16(f, 1);                     /* version */
    write_le16(f, 32);                    /* header_size */
    write_le16(f, spec->width);
    write_le16(f, spec->height);
    write_le16(f, spec->fps100);
    write_le16(f, 1);                     /* pixel_format = baseline JPEG */
    write_le32(f, spec->frame_count);
    write_le32(f, spec->frame_size);      /* max_frame_bytes */
    write_le32(f, 0);                     /* payload_crc32: 0 = 未计算 */
    write_le32(f, spec->transition_offset);

    for (uint32_t i = 0; i < spec->frame_count; i++) {
        write_le32(f, spec->frame_size);
        for (uint32_t k = 0; k < spec->frame_size; k++) {
            fputc((int)((i + spec->fill_seed + k) & 0xFF), f);
        }
    }
    fclose(f);
}

/* 帧内容可预测：第 i 帧第 k 字节 = (i + seed + k) & 0xFF */
static int frame_byte(const jwp_spec_t *spec, uint32_t frame, uint32_t k)
{
    return (int)((frame + spec->fill_seed + k) & 0xFF);
}

/* ── 测试 1：头部解析 ────────────────────────────────────────── */
static void test_open_header(void)
{
    printf("\n[1] 头部解析与字段校验\n");
    jwp_spec_t spec = { .path = TMP_PREFIX "basic.jwp", .width = 1024, .height = 600,
                        .fps100 = 1500, .frame_count = 5, .frame_size = 4096 };
    build_jwp(&spec);

    jwp_reader_t r;
    CHECK_EQ_INT(jwp_reader_open(&r, spec.path), ESP_OK, "打开合法 JWP 成功");
    CHECK_EQ_INT(r.width, 1024, "width 解析正确");
    CHECK_EQ_INT(r.height, 600, "height 解析正确");
    CHECK_EQ_INT(r.fps_times_100, 1500, "fps_times_100 解析正确");
    CHECK_EQ_INT(r.frame_count, 5, "frame_count 解析正确");
    CHECK_EQ_INT(r.max_frame_bytes, 4096, "max_frame_bytes 解析正确");
    CHECK_EQ_INT(r.pixel_format, JWP_PIXEL_FORMAT_BASELINE_JPEG, "pixel_format 正确");
    CHECK_EQ_INT(r.data_offset, 32, "数据区从第 32 字节开始");
    CHECK_EQ_INT(jwp_reader_loop_offset(&r), 32, "无 transition_offset 时回跳到第一帧");
    jwp_reader_close(&r);
}

/* ── 测试 2：坏文件被拒绝 ────────────────────────────────────── */
static void test_reject_bad(void)
{
    printf("\n[2] 非法输入被拒绝\n");

    jwp_reader_t r;

    /* 不存在 */
    CHECK_EQ_INT(jwp_reader_open(&r, "no_such_file.jwp"), ESP_ERR_NOT_FOUND,
                 "不存在的文件返回 ESP_ERR_NOT_FOUND");

    /* magic 错误 */
    jwp_spec_t spec = { .path = TMP_PREFIX "badmagic.jwp", .width = 1024, .height = 600,
                        .fps100 = 1500, .frame_count = 2, .frame_size = 1024 };
    build_jwp(&spec);
    FILE *f = fopen(spec.path, "r+b");
    assert(f);
    fwrite("XXXX", 1, 4, f);       /* 破坏 magic */
    fclose(f);
    CHECK_EQ_INT(jwp_reader_open(&r, spec.path), ESP_ERR_INVALID_ARG,
                 "magic 非 JWP1 被拒绝");

    /* version 错误 */
    spec.path = TMP_PREFIX "badver.jwp";
    build_jwp(&spec);
    f = fopen(spec.path, "r+b");
    assert(f);
    fseek(f, 4, SEEK_SET);
    write_le16(f, 9);
    fclose(f);
    CHECK_EQ_INT(jwp_reader_open(&r, spec.path), ESP_ERR_INVALID_VERSION,
                 "version != 1 被拒绝");

    /* pixel_format 错误 */
    spec.path = TMP_PREFIX "badfmt.jwp";
    build_jwp(&spec);
    f = fopen(spec.path, "r+b");
    assert(f);
    fseek(f, 14, SEEK_SET);
    write_le16(f, 7);
    fclose(f);
    CHECK_EQ_INT(jwp_reader_open(&r, spec.path), ESP_ERR_NOT_SUPPORTED,
                 "pixel_format != 1 被拒绝");

    /* 文件过短 */
    spec.path = TMP_PREFIX "short.jwp";
    f = fopen(spec.path, "wb");
    assert(f);
    fwrite("JWP1", 1, 4, f);
    fclose(f);
    CHECK_EQ_INT(jwp_reader_open(&r, spec.path), ESP_ERR_INVALID_SIZE,
                 "小于 32 字节的文件被拒绝");

    /* frame_count = 0 */
    spec.path = TMP_PREFIX "zeroframes.jwp";
    spec.frame_count = 0;
    build_jwp(&spec);
    CHECK_EQ_INT(jwp_reader_open(&r, spec.path), ESP_ERR_INVALID_SIZE,
                 "frame_count = 0 被拒绝");
}

/* ── 测试 3：顺序读帧 + 循环回跳 ──────────────────────────────── */
static void test_sequence_and_loop(void)
{
    printf("\n[3] 顺序读帧与循环回跳\n");
    jwp_spec_t spec = { .path = TMP_PREFIX "seq.jwp", .width = 1024, .height = 600,
                        .fps100 = 1500, .frame_count = 3, .frame_size = 2048,
                        .fill_seed = 17 };
    build_jwp(&spec);

    jwp_reader_t r;
    CHECK_EQ_INT(jwp_reader_open(&r, spec.path), ESP_OK, "打开成功");

    uint8_t *buf = malloc(spec.frame_size);
    assert(buf);
    size_t got = 0;

    for (uint32_t i = 0; i < 3; i++) {
        CHECK_EQ_INT(jwp_reader_read_frame(&r, buf, spec.frame_size, &got), ESP_OK,
                     "读帧成功");
        CHECK_EQ_INT(got, spec.frame_size, "帧长度正确");
        CHECK(buf[0] == frame_byte(&spec, i, 0) &&
              buf[100] == frame_byte(&spec, i, 100), "帧内容与写入一致");
    }
    CHECK_EQ_INT(r.current_frame, 3, "读完一轮后 current_frame = frame_count");

    /* 第 4 次读取应自动回跳到第 0 帧 */
    CHECK_EQ_INT(jwp_reader_read_frame(&r, buf, spec.frame_size, &got), ESP_OK,
                 "越过末尾后自动循环");
    CHECK(buf[0] == frame_byte(&spec, 0, 0), "循环后回到第 0 帧内容");

    /* 缓冲区不足必须被拒绝 */
    CHECK_EQ_INT(jwp_reader_read_frame(&r, buf, 16, &got), ESP_ERR_INVALID_SIZE,
                 "缓冲区小于 max_frame_bytes 被拒绝");

    free(buf);
    jwp_reader_close(&r);
}

/* ── 测试 4：transition_offset（片头只播一次） ───────────────── */
static void test_transition_offset(void)
{
    printf("\n[4] transition_offset 循环回跳点\n");
    jwp_spec_t spec = { .path = TMP_PREFIX "trans.jwp", .width = 512, .height = 288,
                        .fps100 = 3000, .frame_count = 4, .frame_size = 1024,
                        .fill_seed = 33 };
    /* 循环体从第 2 帧开始：片头(0,1) 只播一次 */
    spec.transition_offset = 32 + 2 * (4 + spec.frame_size);
    build_jwp(&spec);

    jwp_reader_t r;
    CHECK_EQ_INT(jwp_reader_open(&r, spec.path), ESP_OK, "打开成功");
    CHECK_EQ_INT(jwp_reader_loop_offset(&r), spec.transition_offset,
                 "loop_offset 采用 header 中的 transition_offset");

    uint8_t *buf = malloc(spec.frame_size);
    assert(buf);
    size_t got = 0;

    /* 顺序读完 4 帧 */
    for (uint32_t i = 0; i < 4; i++) {
        CHECK_EQ_INT(jwp_reader_read_frame(&r, buf, spec.frame_size, &got), ESP_OK,
                     "按顺序读帧成功");
    }
    CHECK(buf[0] == frame_byte(&spec, 3, 0), "第 4 帧内容正确");

    /* 下一次读取应回到第 2 帧（不是第 0 帧） */
    CHECK_EQ_INT(jwp_reader_read_frame(&r, buf, spec.frame_size, &got), ESP_OK,
                 "回跳后仍能读帧");
    CHECK(buf[0] == frame_byte(&spec, 2, 0), "回跳到 transition 指定的第 2 帧");
    CHECK(buf[0] != frame_byte(&spec, 0, 0), "确认没有回到第 0 帧");

    /* seek_frame 随机定位 */
    CHECK_EQ_INT(jwp_reader_seek_frame(&r, 1), ESP_OK, "seek 到第 1 帧");
    CHECK_EQ_INT(jwp_reader_read_frame(&r, buf, spec.frame_size, &got), ESP_OK,
                 "seek 后读帧成功");
    CHECK(buf[0] == frame_byte(&spec, 1, 0), "seek 后内容为第 1 帧");
    CHECK_EQ_INT(jwp_reader_seek_frame(&r, 99), ESP_ERR_INVALID_ARG,
                 "越界 seek 被拒绝");

    free(buf);
    jwp_reader_close(&r);
}

/* ── 测试 5：CRC32 ──────────────────────────────────────────── */
static void test_crc32(void)
{
    printf("\n[5] CRC32 与 zlib 一致\n");
    /* zlib.crc32(b"123456789") == 0xCBF43926（标准测试向量） */
    const char *s = "123456789";
    uint32_t crc = jwp_crc32(0, s, strlen(s));
    CHECK_EQ_INT(crc, 0xCBF43926, "标准向量 123456789 → 0xCBF43926");

    /* 分块调用必须与一次调用结果相同（转换脚本按块读取时依赖这个性质） */
    uint32_t c1 = jwp_crc32(0, s, 4);
    uint32_t c2 = jwp_crc32(c1, s + 4, strlen(s) - 4);
    CHECK_EQ_INT(c2, crc, "分块累积 CRC 与整体一致");

    CHECK_EQ_INT(jwp_crc32(0, "", 0), 0, "空数据 CRC = 0");
}

/* ── 测试 6：畸形帧在打开时拒绝 ─────────────────────────────── */
static void test_corrupt_frame_len(void)
{
    printf("\n[6] 帧边界严格校验\n");
    jwp_spec_t spec = { .path = TMP_PREFIX "corrupt.jwp", .width = 1024, .height = 600,
                        .fps100 = 1500, .frame_count = 3, .frame_size = 512,
                        .fill_seed = 5 };
    build_jwp(&spec);

    /* 把第 2 帧的长度字段改成一个超大值（> max_frame_bytes） */
    FILE *f = fopen(spec.path, "r+b");
    assert(f);
    long off = 32 + (4 + spec.frame_size) + (4 + spec.frame_size);
    fseek(f, off, SEEK_SET);
    write_le32(f, 0x7FFFFFFF);
    fclose(f);

    jwp_reader_t r;
    CHECK_EQ_INT(jwp_reader_open(&r, spec.path), ESP_ERR_INVALID_SIZE,
                 "超长帧在打开时拒绝");

    spec.path = TMP_PREFIX "badloop.jwp";
    spec.transition_offset = 33; /* 指向第一帧长度字段内部 */
    build_jwp(&spec);
    CHECK_EQ_INT(jwp_reader_open(&r, spec.path), ESP_ERR_INVALID_SIZE,
                 "循环偏移不在帧边界时拒绝");

    spec.path = TMP_PREFIX "truncated.jwp";
    spec.transition_offset = 0;
    build_jwp(&spec);
    f = fopen(spec.path, "r+b"); assert(f);
    fseek(f, -1, SEEK_END);
    long new_end = ftell(f);
    fclose(f);
    /* 写出少一个字节的副本，避免依赖平台专属 truncate。 */
    f = fopen(spec.path, "rb"); assert(f);
    FILE *short_file = fopen(TMP_PREFIX "shortframe.jwp", "wb"); assert(short_file);
    for (long i = 0; i < new_end; i++) fputc(fgetc(f), short_file);
    fclose(short_file); fclose(f);
    CHECK_EQ_INT(jwp_reader_open(&r, TMP_PREFIX "shortframe.jwp"), ESP_ERR_INVALID_SIZE,
                 "截断末帧在打开时拒绝");

    spec.path = TMP_PREFIX "badcount.jwp";
    build_jwp(&spec);
    f = fopen(spec.path, "r+b"); assert(f);
    fseek(f, 16, SEEK_SET); write_le32(f, 4); fclose(f);
    CHECK_EQ_INT(jwp_reader_open(&r, spec.path), ESP_ERR_INVALID_SIZE,
                 "声明帧数超过实有帧数时拒绝");
}

/* 测试用的临时文件全部写在工作目录下的这个前缀里，跑完即删，
 * 避免污染仓库（此前直接在仓库根运行会留下 t_*.jwp）。 */
#define TMP_PREFIX "jwp_test_tmp_"

static const char *g_tmp_files[] = {
    TMP_PREFIX "basic.jwp",   TMP_PREFIX "badmagic.jwp", TMP_PREFIX "badver.jwp",
    TMP_PREFIX "badfmt.jwp",  TMP_PREFIX "short.jwp",    TMP_PREFIX "zeroframes.jwp",
    TMP_PREFIX "seq.jwp",     TMP_PREFIX "trans.jwp",    TMP_PREFIX "corrupt.jwp",
    TMP_PREFIX "badloop.jwp", TMP_PREFIX "truncated.jwp", TMP_PREFIX "shortframe.jwp",
    TMP_PREFIX "badcount.jwp",
};

int main(void)
{
    printf("=== jwp_reader 宿主机单元测试 ===\n");
    test_open_header();
    test_reject_bad();
    test_sequence_and_loop();
    test_transition_offset();
    test_crc32();
    test_corrupt_frame_len();

    for (size_t i = 0; i < sizeof(g_tmp_files) / sizeof(g_tmp_files[0]); i++) {
        remove(g_tmp_files[i]);
    }

    printf("\n=== 结果: %d 项检查, %d 项失败 ===\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
