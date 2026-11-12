#!/usr/bin/env python3
"""合并 F103 bootloader + app 为一张可直接烧到 0x08000000 的完整镜像。

bootloader 固定占用 8KB（0x08000000-0x08001FFF），不足部分用 0xFF 填充；
app 从 0x08002000 开始，最大 56KB。
"""
import argparse
import pathlib

BOOT_SIZE = 0x2000
APP_MAX = 0xE000
APP_BASE = 0x08002000
FILL = 0xFF


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("boot", type=pathlib.Path, help="f103-boot.bin")
    parser.add_argument("app", type=pathlib.Path, help="f103-node.bin")
    parser.add_argument("out", type=pathlib.Path, help="输出合并镜像")
    args = parser.parse_args()

    boot = args.boot.read_bytes()
    app = args.app.read_bytes()

    if len(boot) > BOOT_SIZE:
        raise SystemExit(f"bootloader {len(boot)} > {BOOT_SIZE} bytes")
    if len(app) > APP_MAX:
        raise SystemExit(f"app {len(app)} > {APP_MAX} bytes")
    if not app:
        raise SystemExit("app image is empty")

    merged = boot + bytes([FILL]) * (BOOT_SIZE - len(boot)) + app
    args.out.write_bytes(merged)
    print(
        f"{args.out}: {len(merged)} bytes "
        f"(boot {len(boot)}/{BOOT_SIZE}, app {len(app)}/{APP_MAX}, "
        f"app base 0x{APP_BASE:08X})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
