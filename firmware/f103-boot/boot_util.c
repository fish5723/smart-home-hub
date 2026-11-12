/**
 * @file boot_util.c
 * @brief bootloader 的极简 libc 替身（-nostdlib 下由 GCC 隐式调用）
 */
#include <stddef.h>
#include <stdint.h>

void *memcpy(void *dst, const void *src, size_t len)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (len--) *d++ = *s++;
    return dst;
}

void *memset(void *dst, int value, size_t len)
{
    uint8_t *d = (uint8_t *)dst;
    while (len--) *d++ = (uint8_t)value;
    return dst;
}

int memcmp(const void *a, const void *b, size_t len)
{
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    while (len--) {
        if (*pa != *pb) return (int)*pa - (int)*pb;
        pa++;
        pb++;
    }
    return 0;
}
