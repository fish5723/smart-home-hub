#!/usr/bin/env python3
"""
protocol_sniffer.py — P4-F103 二进制协议抓包工具
阶段 2A-7

用法:
  python protocol_sniffer.py COM3 115200
  python protocol_sniffer.py COM3 115200 --hex-only
  python protocol_sniffer.py COM3 115200 --save capture.bin

依赖:
  pip install pyserial
"""

import sys
import struct
import time
import argparse
from datetime import datetime
import serial

# ════════════════════════════════════════════════════
#  协议常量（与 firmware/APP/comm/protocol.h 同步）
# ════════════════════════════════════════════════════

SOF1 = 0xAA
SOF2 = 0x55
HEADER_SIZE = 8
CRC_SIZE = 2

FRAME_TYPES = {
    0x01: "HELLO_REQ",   0x02: "HELLO_RSP",
    0x03: "PING",        0x04: "PONG",
    0x7E: "NACK",        0x7F: "ERROR",
    0x10: "SENSOR_REPORT",  0x11: "SENSOR_REPORT_ACK",
    0x12: "ALARM_EVENT",    0x13: "ALARM_EVENT_ACK",
    0x20: "ACTUATOR_CMD",   0x21: "ACTUATOR_CMD_ACK",
    0x22: "ACTUATOR_STATUS",
    0x30: "CONFIG_GET",  0x31: "CONFIG_SET",
    0x32: "CONFIG_DATA", 0x33: "STATS_GET",
    0x34: "STATS_DATA",
    0x50: "LOG_FRAGMENT",
    0x60: "OTA_BEGIN",  0x61: "OTA_DATA",
    0x62: "OTA_END",    0x63: "OTA_RESULT",
}

ERROR_CODES = {
    0x01: "CRC",           0x02: "LEN",
    0x03: "UNKNOWN_TYPE",  0x04: "VERSION",
    0x05: "RX_FULL",       0x10: "UNKNOWN_DEV",
    0x11: "PARAM",         0x12: "BUSY",
    0x13: "DEV_FAULT",     0x14: "SAFETY_LOCK",
    0x20: "EXEC_TIMEOUT",  0x21: "NOT_READY",
    0x22: "STORAGE",       0x23: "STATE_MISMATCH",
}

# ════════════════════════════════════════════════════
#  CRC16-CCITT (同上表)
# ════════════════════════════════════════════════════

_CRC16_TABLE = None

def _crc16_init():
    global _CRC16_TABLE
    if _CRC16_TABLE:
        return
    _CRC16_TABLE = []
    for i in range(256):
        crc = 0
        c = i << 8
        for _ in range(8):
            if (crc ^ c) & 0x8000:
                crc = (crc << 1) ^ 0x1021
            else:
                crc <<= 1
            c <<= 1
        _CRC16_TABLE.append(crc & 0xFFFF)

def crc16_compute(buf: bytes) -> int:
    _crc16_init()
    crc = 0xFFFF
    for b in buf:
        crc = ((crc << 8) ^ _CRC16_TABLE[((crc >> 8) ^ b) & 0xFF]) & 0xFFFF
    return crc

# ════════════════════════════════════════════════════
#  帧解析器
# ════════════════════════════════════════════════════

class FrameParser:
    IDLE = 0
    SOF1 = 1
    SOF2 = 2
    HEADER = 3
    PAYLOAD = 4
    CRC = 5
    DONE = 6
    ERROR = 7

    def __init__(self):
        self.state = self.IDLE
        self.buf = bytearray()

    def feed(self, b: int):
        if self.state == self.IDLE:
            if b == SOF1:
                self.state = self.SOF1
        elif self.state == self.SOF1:
            if b == SOF2:
                self.buf = bytearray()
                self.state = self.SOF2
                self._need = HEADER_SIZE
        elif self.state == self.SOF2:
            self.buf.append(b)
            self._need -= 1
            if self._need == 0:
                self._parse_header()
        elif self.state == self.PAYLOAD:
            self.buf.append(b)
            self._need -= 1
            if self._need == 0:
                self._verify_crc()
                if self._crc_ok:
                    self.state = self.DONE
                else:
                    self.state = self.ERROR
        elif self.state == self.ERROR:
            if time.time() - self._err_time > 0.1:
                self.state = self.IDLE
        # CRC/DONE → caller must call reset()

    def _parse_header(self):
        ver, typ = self.buf[0], self.buf[1]
        seq = self.buf[2] | (self.buf[3] << 8)
        length = self.buf[4] | (self.buf[5] << 8)
        self._ver = ver
        self._type = typ
        self._seq = seq
        self._len = length
        self._raw = bytes(self.buf[:6])
        if length > 256:
            self.state = self.ERROR
            return
        self._need = length + CRC_SIZE
        self.state = self.PAYLOAD

    def _verify_crc(self):
        payload = bytes(self.buf[:self._len])
        crc_received = self.buf[self._len] | (self.buf[self._len + 1] << 8)
        full = self._raw + payload
        crc_computed = crc16_compute(full)
        self._crc_ok = (crc_received == crc_computed)
        self._crc_expected = crc_computed
        self._crc_got = crc_received

    def reset(self):
        self.state = self.IDLE
        self.buf = bytearray()

    @property
    def frame(self):
        if self.state != self.DONE:
            return None
        return {
            'ver': self._ver,
            'type': self._type,
            'seq': self._seq,
            'len': self._len,
            'payload': bytes(self.buf[:self._len]),
            'crc_ok': self._crc_ok,
            'crc_expected': self._crc_expected,
            'crc_got': self._crc_got,
        }

# ════════════════════════════════════════════════════
#  人可读解码
# ════════════════════════════════════════════════════

def decode_frame(frame: dict) -> str:
    """返回帧的人可读描述（简短）"""
    typ = frame['type']
    tname = FRAME_TYPES.get(typ, f"0x{typ:02X}")
    seq = frame['seq']
    payload = frame['payload']
    crc = "OK" if frame['crc_ok'] else f"FAIL(exp={frame['crc_expected']:04X} got={frame['crc_got']:04X})"

    desc = ""

    # HELLO_REQ / HELLO_RSP
    if typ in (0x01, 0x02) and len(payload) >= 4:
        our_ver = payload[0]
        min_ver = payload[1]
        cap = struct.unpack('<H', payload[2:4])[0]
        desc = f"ver={our_ver}.{min_ver} caps=0x{cap:04X}"

    # PING/PONG
    elif typ in (0x03, 0x04):
        desc = ""

    # NACK
    elif typ == 0x7E and len(payload) >= 3:
        ref = payload[0] | (payload[1] << 8)
        err = payload[2]
        ename = ERROR_CODES.get(err, f"0x{err:02X}")
        desc = f"ref_seq={ref} err={ename}"

    # SENSOR_REPORT / ALARM_EVENT
    elif typ in (0x10, 0x12):
        desc = f"payload={len(payload)}B"

    # SENSOR_REPORT_ACK / ALARM_EVENT_ACK
    elif typ in (0x11, 0x13) and len(payload) >= 2:
        ref = payload[0] | (payload[1] << 8)
        desc = f"ref_seq={ref}"

    # ACTUATOR_CMD
    elif typ == 0x20 and len(payload) >= 7:
        dev = payload[0]
        act = payload[1]
        ttl = payload[5] | (payload[6] << 8)
        actions = {0: "OFF", 1: "ON", 2: "PULSE", 3: "SET", 4: "QUERY"}
        desc = f"dev=0x{dev:02X} action={actions.get(act, str(act))} ttl={ttl}ms"

    # ACTUATOR_CMD_ACK
    elif typ == 0x21 and len(payload) >= 3:
        result = payload[1]
        state = payload[2]
        desc = f"result={result} state={'ON' if state else 'OFF'}"

    # 通用 payload dump
    if not desc and len(payload) > 0 and len(payload) <= 32:
        desc = f"payload={payload.hex()}"
    elif not desc and len(payload) > 32:
        desc = f"payload={payload[:32].hex()}... ({len(payload)}B)"

    return f"{tname:20s} seq={seq:<5d} crc={crc:4s}  {desc}"

# ════════════════════════════════════════════════════
#  主循环
# ════════════════════════════════════════════════════

def main():
    parser = argparse.ArgumentParser(description='P4-F103 二进制协议抓包工具')
    parser.add_argument('port', help='COM 口名称，如 COM3')
    parser.add_argument('baud', type=int, nargs='?', default=115200, help='波特率（默认 115200）')
    parser.add_argument('--hex-only', action='store_true', help='仅显示原始 HEX，不做解析')
    parser.add_argument('--save', help='保存原始字节到文件')
    parser.add_argument('-v', '--verbose', action='store_true', help='显示更详细的帧内容')
    args = parser.parse_args()

    ser = serial.Serial(args.port, args.baud, timeout=0.01)
    print(f"[sniffer] opened {args.port} @ {args.baud}")
    print()

    fout = None
    if args.save:
        fout = open(args.save, 'wb')
        print(f"[sniffer] saving raw data to {args.save}")

    fparser = FrameParser()
    frame_count = 0
    err_count = 0

    try:
        while True:
            b = ser.read(1)
            if not b:
                continue

            if fout:
                fout.write(b)

            fparser.feed(b[0])

            if fparser.state == FrameParser.DONE:
                frame = fparser.frame
                if frame:
                    frame_count += 1
                    ts = datetime.now().strftime('%H:%M:%S.%f')[:-3]

                    crc_flag = "" if frame['crc_ok'] else " [CRC FAIL]"
                    if args.hex_only:
                        payload_hex = frame['payload'].hex()
                        print(f"[{ts}] SOF VER={frame['ver']} TYPE=0x{frame['type']:02X} "
                              f"SEQ={frame['seq']} LEN={frame['len']} PAYLOAD={payload_hex} "
                              f"CRC={frame['crc_got']:04X}{crc_flag}")
                    else:
                        line = decode_frame(frame)
                        print(f"[{ts}] {line}{crc_flag}")

                    if args.verbose and not args.hex_only:
                        if len(frame['payload']) > 0:
                            hexdump(frame['payload'])

                fparser.reset()
            elif fparser.state == FrameParser.ERROR:
                err_count += 1
                fparser.reset()

    except KeyboardInterrupt:
        print()
        print(f"[sniffer] stopped.  frames={frame_count}  errors={err_count}")
    finally:
        ser.close()
        if fout:
            fout.close()

def hexdump(data: bytes):
    """紧凑 HEX dump"""
    for i in range(0, len(data), 16):
        chunk = data[i:i+16]
        hex_part = ' '.join(f'{b:02X}' for b in chunk)
        ascii_part = ''.join(chr(b) if 32 <= b < 127 else '.' for b in chunk)
        print(f'        {i:04X}: {hex_part:<48s} {ascii_part}')

if __name__ == '__main__':
    main()