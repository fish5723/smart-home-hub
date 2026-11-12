/**
 * @file frame.c
 * @brief 阶段二帧格式复用（算法与 docs/03 §2.3 完全一致，便于与 F103/P4 对拍）
 */
#include "frame.h"

#include <string.h>

uint16_t frame_crc16(const uint8_t *data, uint16_t len)
{
    uint16_t crc = 0xFFFF;

    while (len--) {
        crc ^= (uint16_t)(*data++) << 8;
        for (int i = 0; i < 8; i++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021)
                                 : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

uint16_t frame_build(uint8_t *out, size_t cap, uint8_t type, uint16_t seq,
                     const uint8_t *payload, uint16_t len)
{
    const size_t total = FRAME_HEADER_SIZE + len + FRAME_CRC_SIZE;

    if (out == NULL || cap < total || len > FRAME_MAX_PAYLOAD) {
        return 0;
    }
    if (len > 0 && payload == NULL) {
        return 0;   /* 有载荷长度却没有载荷指针：宁可返回 0，也不要组出空洞 CRC */
    }

    out[0] = FRAME_SOF0;
    out[1] = FRAME_SOF1;
    out[2] = FRAME_VER;
    out[3] = type;
    out[4] = (uint8_t)(seq & 0xFF);
    out[5] = (uint8_t)(seq >> 8);
    out[6] = (uint8_t)(len & 0xFF);
    out[7] = (uint8_t)(len >> 8);

    if (len > 0 && payload != NULL) {
        memcpy(&out[FRAME_HEADER_SIZE], payload, len);
    }
    const uint16_t crc = frame_crc16(&out[2], (uint16_t)(total - 2 - 2));
    out[total - 2] = (uint8_t)(crc & 0xFF);
    out[total - 1] = (uint8_t)(crc >> 8);

    return (uint16_t)total;
}

int frame_selftest(void)
{
    static const uint8_t vec[] = "123456789";
    return frame_crc16(vec, (uint16_t)(sizeof(vec) - 1)) == FRAME_CRC_CHECK;
}
