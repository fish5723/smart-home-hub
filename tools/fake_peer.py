#!/usr/bin/env python3
"""
fake_peer.py — 模拟 P4 主站，用于阶段 2A 协议测试
阶段 2A-8

用法:
  python fake_peer.py COM3 115200              # 只发送 PING 心跳
  python fake_peer.py COM3 115200 --cmd relay1 on   # 发送一次执行器指令后退出
  python fake_peer.py COM3 115200 --interactive     # 交互模式，手动发指令
  python fake_peer.py COM3 115200 --test         # 自动化测试序列

依赖:
  pip install pyserial
"""

import sys
import struct
import time
import argparse
import threading
from datetime import datetime
import serial

# ════════════════════════════════════════════════════
#  协议常量（与 firmware 同步）
# ════════════════════════════════════════════════════

SOF1, SOF2 = 0xAA, 0x55
PROTO_VER = 0x02

# 消息类型
HELLO_REQ, HELLO_RSP = 0x01, 0x02
PING, PONG = 0x03, 0x04
NACK, ERROR = 0x7E, 0x7F
SENSOR_REPORT, SENSOR_REPORT_ACK = 0x10, 0x11
ALARM_EVENT, ALARM_EVENT_ACK = 0x12, 0x13
ACTUATOR_CMD, ACTUATOR_CMD_ACK = 0x20, 0x21
ACTUATOR_STATUS = 0x22

# 设备 ID
DEV_RELAY1 = 0x01
DEV_RELAY2 = 0x02
DEV_LOCK   = 0x03
DEV_BEEPER = 0x04

# 动作
ACTION_OFF   = 0
ACTION_ON    = 1
ACTION_PULSE = 2

# 结果
RESULT_OK = 0

# 帧格式
HEADER_SIZE, CRC_SIZE = 8, 2

# ════════════════════════════════════════════════════
#  CRC16-CCITT 查表
# ════════════════════════════════════════════════════

_CRC_TABLE = None

def _crc_init():
    global _CRC_TABLE
    if _CRC_TABLE:
        return
    _CRC_TABLE = []
    for i in range(256):
        crc = 0
        c = i << 8
        for _ in range(8):
            if (crc ^ c) & 0x8000:
                crc = (crc << 1) ^ 0x1021
            else:
                crc <<= 1
            c <<= 1
        _CRC_TABLE.append(crc & 0xFFFF)

def crc16(buf: bytes) -> int:
    _crc_init()
    crc = 0xFFFF
    for b in buf:
        crc = ((crc << 8) ^ _CRC_TABLE[((crc >> 8) ^ b) & 0xFF]) & 0xFFFF
    return crc

# ════════════════════════════════════════════════════
#  组包
# ════════════════════════════════════════════════════

_seq = 1

def next_seq():
    global _seq
    s = _seq
    _seq = (_seq + 1) & 0xFFFF
    if _seq == 0:
        _seq = 1
    return s

def build_frame(typ: int, seq: int, payload: bytes = b'') -> bytes:
    ver = PROTO_VER
    length = len(payload)
    header = struct.pack('<BBBBHH', SOF1, SOF2, ver, typ, seq, length)
    crc = crc16(header[2:] + payload)  # CRC over VER+TYPE+SEQ+LEN+PAYLOAD
    return header + payload + struct.pack('<H', crc)

def build_ping():
    return build_frame(PING, next_seq())

def build_pong(seq: int):
    return build_frame(PONG, seq)

def build_actuator_cmd(dev_id: int, action: int, ttl_ms: int = 0,
                       param1: int = 0, param2: int = 0):
    """payload: dev_id(1) + action(1) + param1(2) + param2(2) + ttl_ms(2)"""
    payload = struct.pack('<BBHHH', dev_id, action, param1, param2, ttl_ms)
    return build_frame(ACTUATOR_CMD, next_seq(), payload)

def build_hello_req():
    """payload: our_ver(1) + min_ver(1) + capabilities(2)"""
    payload = struct.pack('<BBH', PROTO_VER, 1, 0x0001)
    return build_frame(HELLO_REQ, next_seq(), payload)

def build_hello_rsp(seq: int):
    """payload: our_ver(1) + min_ver(1) + capabilities(2) + node_name(8)"""
    payload = struct.pack('<BBH', PROTO_VER, 1, 0x0001) + b'FAKE-P4\x00\x00'
    return build_frame(HELLO_RSP, seq, payload)

# ════════════════════════════════════════════════════
#  收包状态机（与 f103 端相同逻辑）
# ════════════════════════════════════════════════════

class FrameParser:
    IDLE, SOF1, SOF2, HEADER, PAYLOAD, CRC, DONE = 0, 1, 2, 3, 4, 5, 6

    def __init__(self):
        self.reset()

    def reset(self):
        self.state = self.IDLE
        self.buf = bytearray()
        self._need = 0

    def feed(self, b: int):
        if self.state == self.IDLE:
            if b == SOF1:
                self._want_sof2 = True
        elif self.state == self.IDLE:
            pass  # already handled
        # Simplified: track SOF1→SOF2 explicitly
        if self.state == self.IDLE and b == SOF1:
            self.state = self.SOF1
        elif self.state == self.SOF1:
            if b == SOF2:
                self.buf = bytearray()
                self.state = self.SOF2
                self._need = HEADER_SIZE
            else:
                self.state = self.IDLE
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
                self.state = self.DONE

    def _parse_header(self):
        self._ver = self.buf[0]
        self._type = self.buf[1]
        self._seq = self.buf[2] | (self.buf[3] << 8)
        self._len = self.buf[4] | (self.buf[5] << 8)
        self._raw_header = bytes(self.buf[:6])
        if self._len > 256:
            self.state = self.IDLE
            return
        self._need = self._len + CRC_SIZE
        self.state = self.PAYLOAD

    def _verify_crc(self):
        payload = bytes(self.buf[:self._len])
        crc_rcv = self.buf[self._len] | (self.buf[self._len + 1] << 8)
        full = self._raw_header + payload
        self._crc_ok = (crc_rcv == crc16(full))
        self._crc_got = crc_rcv
        self._crc_exp = crc16(full)

    @property
    def frame(self):
        if self.state != self.DONE:
            return None
        return {
            'ver': self._ver, 'type': self._type,
            'seq': self._seq, 'len': self._len,
            'payload': bytes(self.buf[:self._len]),
            'crc_ok': self._crc_ok,
        }

# ════════════════════════════════════════════════════
#  Peer 主逻辑
# ════════════════════════════════════════════════════

FRAME_NAMES = {
    PING: 'PING', PONG: 'PONG',
    SENSOR_REPORT: 'SENSOR_REPORT', SENSOR_REPORT_ACK: 'SENSOR_REPORT_ACK',
    ACTUATOR_CMD: 'ACTUATOR_CMD', ACTUATOR_CMD_ACK: 'ACTUATOR_CMD_ACK',
    HELLO_REQ: 'HELLO_REQ', HELLO_RSP: 'HELLO_RSP',
    NACK: 'NACK', ERROR: 'ERROR',
}

class FakePeer:
    def __init__(self, port: str, baud: int):
        self.ser = serial.Serial(port, baud, timeout=0.01)
        self.parser = FrameParser()
        self.running = True
        self.rx_thread = None

        # 统计
        self.rx_frames = 0
        self.tx_frames = 0
        self.last_pong = 0.0
        self.last_rx = time.time()

        # 待确认帧追踪（简单版：只追踪最后发出的需 ACK 帧）
        self.pending_seq = 0
        self.pending_time = 0
        self.pending_retry = 0
        self.pending_ack = False
        self.ack_timeout_ms = 100
        self.max_retrans = 3

    def start(self):
        self.rx_thread = threading.Thread(target=self._rx_loop, daemon=True)
        self.rx_thread.start()

    def stop(self):
        self.running = False
        if self.rx_thread:
            self.rx_thread.join(timeout=1)
        self.ser.close()

    def send_raw(self, data: bytes):
        self.ser.write(data)
        self.ser.flush()
        self.tx_frames += 1

    def send_frame(self, typ: int, payload: bytes = b'') -> int:
        seq = next_seq()
        frame = build_frame(typ, seq, payload)
        self.send_raw(frame)
        return seq

    def send_with_ack(self, typ: int, payload: bytes = b'') -> int:
        """发送需要 ACK 的帧，阻塞等待 ACK 或超时"""
        if self.pending_ack:
            return 0  # 忙

        seq = next_seq()
        frame = build_frame(typ, seq, payload)
        self.send_raw(frame)

        self.pending_seq = seq
        self.pending_time = time.time()
        self.pending_retry = 0
        self.pending_ack = True

        # 阻塞等待
        while self.pending_ack and self.pending_retry <= self.max_retrans:
            elapsed = (time.time() - self.pending_time) * 1000
            if elapsed > self.ack_timeout_ms:
                if self.pending_retry < self.max_retrans:
                    self.pending_retry += 1
                    self.ser.write(frame)
                    self.ser.flush()
                    self.pending_time = time.time()
                    print(f"  [peer] RETX #{self.pending_retry} seq={seq}")
                else:
                    print(f"  [peer] ACK timeout seq={seq} (max retries)")
                    self.pending_ack = False
                    break
            time.sleep(0.01)

        return 0 if self.pending_ack else seq

    def _rx_loop(self):
        while self.running:
            b = self.ser.read(1)
            if not b:
                continue
            self.parser.feed(b[0])
            if self.parser.state == FrameParser.DONE:
                f = self.parser.frame
                if f:
                    self.rx_frames += 1
                    self.last_rx = time.time()
                    self._handle_rx(f)
                self.parser.reset()

    def _handle_rx(self, f: dict):
        tname = FRAME_NAMES.get(f['type'], f"0x{f['type']:02X}")
        ts = datetime.now().strftime('%H:%M:%S.%f')[:-3]

        crc_flag = "" if f['crc_ok'] else " [CRC FAIL]"
        print(f"[{ts}] RX {tname:18s} seq={f['seq']:<5d} len={f['len']}{crc_flag}")

        # PONG → 刷新心跳统计
        if f['type'] == PONG:
            self.last_pong = time.time()

        # ACK → 清除 pending
        elif f['type'] == ACTUATOR_CMD_ACK:
            if self.pending_ack:
                self.pending_ack = False
                print(f"  [peer] ACK received for seq={self.pending_seq}")

        # SENSOR_REPORT → 显示内容 + 回 ACK
        elif f['type'] == SENSOR_REPORT and f['len'] > 0:
            ack = struct.pack('<H', f['seq'])
            ack_frame = build_frame(SENSOR_REPORT_ACK, next_seq(), ack)
            self.ser.write(ack_frame)
            self.ser.flush()
            self.tx_frames += 1

        # NACK
        elif f['type'] == NACK and f['len'] >= 3:
            ref = f['payload'][0] | (f['payload'][1] << 8)
            err = f['payload'][2]
            print(f"  [peer] NACK ref={ref} err={err}")

    def ping(self):
        """发送一次 PING（非阻塞）"""
        self.send_frame(PING)

# ════════════════════════════════════════════════════
#  测试序列
# ════════════════════════════════════════════════════

def run_test(args):
    peer = FakePeer(args.port, args.baud)
    peer.start()
    print("[peer] starting test sequence...")
    time.sleep(0.5)

    # 1. HELLO 握手
    print("\n=== Test 1: HELLO_REQ ===")
    seq = peer.send_frame(HELLO_REQ, struct.pack('<BBH', PROTO_VER, 1, 0x0001))
    print(f"  sent HELLO_REQ seq={seq}")
    time.sleep(2)

    # 2. PING/PONG 心跳 (5次)
    print("\n=== Test 2: PING x5 ===")
    for i in range(5):
        peer.ping()
        time.sleep(1.2)

    # 3. ACTUATOR_CMD (Relay1 ON)
    print("\n=== Test 3: ACTUATOR_CMD relay1=ON ===")
    seq = peer.send_with_ack(ACTUATOR_CMD,
        struct.pack('<BBHHH', DEV_RELAY1, ACTION_ON, 0, 0, 5000))
    if seq:
        print(f"  ACK ok, relay1 ON for 5s")
    time.sleep(2)

    # 4. ACTUATOR_CMD (Relay1 OFF)
    print("\n=== Test 4: ACTUATOR_CMD relay1=OFF ===")
    seq = peer.send_with_ack(ACTUATOR_CMD,
        struct.pack('<BBHHH', DEV_RELAY1, ACTION_OFF, 0, 0, 0))
    if seq:
        print(f"  ACK ok, relay1 OFF")
    time.sleep(1)

    # 5. 统计
    print(f"\n=== Test Complete ===")
    print(f"  TX frames: {peer.tx_frames}")
    print(f"  RX frames: {peer.rx_frames}")
    print(f"  Last PONG: {peer.last_pong:.1f}s ago" if peer.last_pong else "  No PONG")

    peer.stop()

# ════════════════════════════════════════════════════

INTERACTIVE_HELP = """
Commands:
  ping          发送 PING
  hello         发送 HELLO_REQ
  relay1 on     打开继电器 1 (5s TTL)
  relay1 off    关闭继电器 1
  relay2 on     打开继电器 2
  relay2 off    关闭继电器 2
  lock pulse    钛丝锁脉冲开锁
  beep on       蜂鸣器开 (1s)
  beep off      蜂鸣器关
  stats         显示统计信息
  quit          退出
"""

def run_interactive(args):
    peer = FakePeer(args.port, args.baud)
    peer.start()
    print(f"[peer] connected to {args.port} @ {args.baud}")
    print(INTERACTIVE_HELP)

    try:
        while True:
            cmd = input("peer> ").strip().lower()
            if not cmd:
                continue
            elif cmd == 'quit' or cmd == 'exit':
                break
            elif cmd == 'ping':
                peer.ping()
                print("  sent PING")
            elif cmd == 'hello':
                seq = peer.send_frame(HELLO_REQ,
                    struct.pack('<BBH', PROTO_VER, 1, 0x0001))
                print(f"  sent HELLO_REQ seq={seq}")
            elif cmd == 'relay1 on':
                peer.send_with_ack(ACTUATOR_CMD,
                    struct.pack('<BBHHH', DEV_RELAY1, ACTION_ON, 0, 0, 5000))
            elif cmd == 'relay1 off':
                peer.send_with_ack(ACTUATOR_CMD,
                    struct.pack('<BBHHH', DEV_RELAY1, ACTION_OFF, 0, 0, 0))
            elif cmd == 'relay2 on':
                peer.send_with_ack(ACTUATOR_CMD,
                    struct.pack('<BBHHH', DEV_RELAY2, ACTION_ON, 0, 0, 5000))
            elif cmd == 'relay2 off':
                peer.send_with_ack(ACTUATOR_CMD,
                    struct.pack('<BBHHH', DEV_RELAY2, ACTION_OFF, 0, 0, 0))
            elif cmd == 'lock pulse':
                peer.send_with_ack(ACTUATOR_CMD,
                    struct.pack('<BBHHH', DEV_LOCK, ACTION_PULSE, 0, 0, 3000))
            elif cmd == 'beep on':
                peer.send_with_ack(ACTUATOR_CMD,
                    struct.pack('<BBHHH', DEV_BEEPER, ACTION_ON, 0, 0, 1000))
            elif cmd == 'beep off':
                peer.send_with_ack(ACTUATOR_CMD,
                    struct.pack('<BBHHH', DEV_BEEPER, ACTION_OFF, 0, 0, 0))
            elif cmd == 'stats':
                print(f"  TX: {peer.tx_frames}  RX: {peer.rx_frames}")
            else:
                print(f"  unknown command: {cmd}")
    except KeyboardInterrupt:
        print()
    finally:
        peer.stop()

# ════════════════════════════════════════════════════

def run_heartbeat(args):
    """仅心跳模式：每秒 PING，打印收到的帧"""
    peer = FakePeer(args.port, args.baud)
    peer.start()
    print(f"[peer] heartbeat mode: {args.port} @ {args.baud}")
    print(f"[peer] sending PING every 1s...\n")

    try:
        while True:
            peer.ping()
            time.sleep(1.0)
    except KeyboardInterrupt:
        print(f"\n[peer] stopped.  TX={peer.tx_frames}  RX={peer.rx_frames}")
    finally:
        peer.stop()

# ════════════════════════════════════════════════════

def main():
    parser = argparse.ArgumentParser(description='fake_peer.py — 模拟 P4 主站')
    parser.add_argument('port', help='COM 口名称，如 COM3')
    parser.add_argument('baud', type=int, nargs='?', default=115200, help='波特率（默认 115200）')
    parser.add_argument('--cmd', nargs='+', help='发送一条指令后退出，如: --cmd relay1 on')
    parser.add_argument('--interactive', '-i', action='store_true', help='交互模式')
    parser.add_argument('--test', action='store_true', help='运行自动化测试序列')
    args = parser.parse_args()

    if args.test:
        run_test(args)
    elif args.interactive:
        run_interactive(args)
    elif args.cmd:
        # 一次性命令模式
        peer = FakePeer(args.port, args.baud)
        peer.start()
        time.sleep(0.3)

        cmd = ' '.join(args.cmd).lower()
        if cmd == 'relay1 on':
            peer.send_with_ack(ACTUATOR_CMD,
                struct.pack('<BBHHH', DEV_RELAY1, ACTION_ON, 0, 0, 5000))
        elif cmd == 'relay1 off':
            peer.send_with_ack(ACTUATOR_CMD,
                struct.pack('<BBHHH', DEV_RELAY1, ACTION_OFF, 0, 0, 0))
        elif cmd == 'relay2 on':
            peer.send_with_ack(ACTUATOR_CMD,
                struct.pack('<BBHHH', DEV_RELAY2, ACTION_ON, 0, 0, 5000))
        elif cmd == 'relay2 off':
            peer.send_with_ack(ACTUATOR_CMD,
                struct.pack('<BBHHH', DEV_RELAY2, ACTION_OFF, 0, 0, 0))
        elif cmd == 'beep on':
            peer.send_with_ack(ACTUATOR_CMD,
                struct.pack('<BBHHH', DEV_BEEPER, ACTION_ON, 0, 0, 1000))
        elif cmd == 'beep off':
            peer.send_with_ack(ACTUATOR_CMD,
                struct.pack('<BBHHH', DEV_BEEPER, ACTION_OFF, 0, 0, 0))
        elif cmd == 'lock pulse':
            peer.send_with_ack(ACTUATOR_CMD,
                struct.pack('<BBHHH', DEV_LOCK, ACTION_PULSE, 0, 0, 3000))
        else:
            print(f"unknown command: {cmd}")
            print("available: relay1 on/off, relay2 on/off, beep on/off, lock pulse")

        time.sleep(0.5)
        peer.stop()
    else:
        # 默认：心跳模式
        run_heartbeat(args)

if __name__ == '__main__':
    main()