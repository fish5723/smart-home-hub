/**
 * @file fmt_fixed.h
 * @brief 定点格式化工具 —— 替代 printf 的 %f，用于移除 `-u _printf_float` 依赖
 *
 * 背景（阶段 1 验收报告 §1.2）：
 *   工程里只有约 15 处用到浮点格式（%.1f / %.2f / %.0f），但 `-u _printf_float`
 *   会把 newlib 的整套浮点格式化拉进固件，实测（Release 构建）：
 *     _dtoa_r 2984 B + _printf_float 1112 B + _printf_i 572 B ≈ 4.7 KB
 *   在 64KB Flash 的 F103C8 上占 7%，对 Debug(-O0) 构建尤其紧张。
 *
 *   本文件提供整数化格式化，功能上等价于 %f 的常见用法，且不依赖浮点 printf。
 *
 * 用法：
 *   char buf[12];
 *   fmt_fixed(buf, sizeof(buf), g_sensor_data.temperature, 1);   // 25.3 → "25.3"
 *   printf("T=%sC\r\n", buf);
 *
 * 注意：浮点【运算】仍然使用（软浮点 __adddf3 等照旧链接），
 *      本文件只替代浮点【格式化】。
 *
 * 来源：本工程新建（阶段 1 验收整改）
 */
#ifndef FMT_FIXED_H
#define FMT_FIXED_H

#include <stdio.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 把浮点值按定点小数位格式化为字符串（不使用浮点 printf）
 *
 * @param buf       输出缓冲
 * @param n         缓冲大小
 * @param v         数值
 * @param decimals  小数位数，0~4（>4 按 4 处理）
 *
 * 行为说明：
 *   - 四舍五入到指定小数位
 *   - 支持负号
 *   - 超出 int32 可表示范围（约 ±2.1e9 / scale）会被钳位，避免溢出成乱码
 *   - 缓冲不足时按 snprintf 语义截断，始终以 '\0' 结尾
 */
static inline void fmt_fixed(char *buf, size_t n, float v, unsigned decimals)
{
    if (buf == NULL || n == 0) return;

    if (decimals > 4) decimals = 4;
    static const int32_t pow10[5] = { 1, 10, 100, 1000, 10000 };
    const int32_t scale = pow10[decimals];

    const bool neg = (v < 0.0f);
    const float av  = neg ? -v : v;

    /* 四舍五入到定点整数 */
    float scaled = av * (float)scale + 0.5f;
    int32_t iv;
    if (scaled >= 2147483000.0f) {
        iv = 2147483000;            /* 钳位防溢出（本工程量程远小于此） */
    } else {
        iv = (int32_t)scaled;
    }
    if (iv < 0) iv = 0;

    const int32_t ip = iv / scale;
    const int32_t fp = iv % scale;
    const char   *sgn = neg ? "-" : "";

    /* 先格式化进本地大缓冲，再用 memcpy 手工截断拷贝给调用方。
     * 两处都是刻意的：
     *   1) 中间隔一层定长缓冲，数值指令的宽度由本文件保证，调用方只需看到一个
     *      字符串拷贝 —— 否则 GCC 在 -Os 下会按调用方的 buf 大小报
     *      -Wformat-truncation。
     *   2) 拷贝不用 snprintf("%s") —— 那同样会被 -Wformat-truncation 抓到
     *      （"up to 31 bytes into a region of size 12"）。memcpy + 手工补 '\0'
     *      不含格式指令，编译器无从告警，且语义完全等价于 snprintf 的截断。
     * 本地缓冲 32B 足够：最长内容形如 "-2147483000.0000" 共 17 字节。 */
    char tmp[32];
    switch (decimals) {
    case 1:  snprintf(tmp, sizeof(tmp), "%s%ld.%01ld", sgn, (long)ip, (long)fp); break;
    case 2:  snprintf(tmp, sizeof(tmp), "%s%ld.%02ld", sgn, (long)ip, (long)fp); break;
    case 3:  snprintf(tmp, sizeof(tmp), "%s%ld.%03ld", sgn, (long)ip, (long)fp); break;
    case 4:  snprintf(tmp, sizeof(tmp), "%s%ld.%04ld", sgn, (long)ip, (long)fp); break;
    default: snprintf(tmp, sizeof(tmp), "%s%ld",      sgn, (long)ip);            break;
    }

    size_t len = strlen(tmp);
    if (len > n - 1) len = n - 1;
    memcpy(buf, tmp, len);
    buf[len] = '\0';
}

#ifdef __cplusplus
}
#endif

#endif /* FMT_FIXED_H */
