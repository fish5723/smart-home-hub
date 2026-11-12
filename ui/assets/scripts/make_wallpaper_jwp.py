#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
make_wallpaper_jwp.py — 把视频/图片序列打包成 JWP 动态壁纸（ADR-020）

JWP 是参考工程 ESP32-S3-LVGL-Clock 使用的容器格式（Apache-2.0），
格式定义（本文档与固件 jwp_reader.c 必须保持一致）:

    offset  size  field
    ------  ----  --------------------------------------------
    0       4     magic[4] = "JWP1"
    4       2     version  = 1
    6       2     header_size = 32
    8       2     width
    10      2     height
    12      2     fps_times_100 = round(fps * 100)
    14      2     pixel_format = 1 (baseline JPEG)
    16      4     frame_count
    20      4     max_frame_bytes
    24      4     payload_crc32      (zlib crc32，0 = 未计算)
    28      4     transition_offset  (循环回跳字节偏移，0 = 回到第一帧)

    之后 frame_count 次:
        uint32  jpeg_frame_length   (little-endian)
        uint8   jpeg_frame_data[jpeg_frame_length]

用法示例:
    # 视频 → JWP（需要 ffmpeg；可用 --ffmpeg 指定路径）
    python ui/assets/scripts/make_wallpaper_jwp.py \\
        --input rain.mp4 --output ui/assets/wallpaper/rain/day.jwp \\
        --width 1024 --height 600 --fps 15 --quality 4 --duration 10

    # 图片序列 → JWP（不需要 ffmpeg）
    python ui/assets/scripts/make_wallpaper_jwp.py \\
        --frames-dir /tmp/frames --output out.jwp --fps 15

    # 只检查一个已有 JWP
    python ui/assets/scripts/make_wallpaper_jwp.py --inspect out.jwp

设计约束（与固件侧对齐）:
  * 所有帧必须是 baseline JPEG（非 progressive），且尺寸严格等于 --width × --height。
    固件播放器按 JWP 头声明的尺寸分配帧缓冲，尺寸不符会拒播。
  * 逐帧长度不得大于 --max-frame（KB），对应 Kconfig
    SMART_HOME_WALLPAPER_MAX_FRAME_KB，超限直接报错——避免上板才发现读不了。
"""

import argparse
import glob
import io
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

HEADER_SIZE = 32
MAGIC = b"JWP1"
VERSION = 1
PIXEL_FORMAT_BASELINE_JPEG = 1


# ────────────────────────────── JPEG 校验 ──────────────────────────────

def jpeg_info(data: bytes):
    """返回 (width, height, is_baseline)。仅解析 SOF 段，不做完整解码。"""
    if len(data) < 4 or data[0:2] != b"\xFF\xD8":
        raise ValueError("不是 JPEG（缺少 SOI 标记）")

    i = 2
    n = len(data)
    while i + 3 < n:
        if data[i] != 0xFF:
            i += 1
            continue
        marker = data[i + 1]
        i += 2
        # 无载荷的标记
        if marker in (0xD8, 0xD9) or 0xD0 <= marker <= 0xD7:
            continue
        if marker == 0xDA:            # SOS：之后是熵编码数据
            break
        if i + 1 >= n:
            break
        seg_len = struct.unpack(">H", data[i:i + 2])[0]
        if marker in (0xC0, 0xC1, 0xC2, 0xC3, 0xC5, 0xC6, 0xC7,
                      0xC9, 0xCA, 0xCB, 0xCD, 0xCE, 0xCF):
            if i + 7 >= n:
                break
            height, width = struct.unpack(">HH", data[i + 3:i + 7])
            # SOF0/SOF1 为 baseline/扩展顺序；SOF2 为 progressive（固件不支持）
            is_baseline = marker in (0xC0, 0xC1)
            return width, height, is_baseline
        i += seg_len
    raise ValueError("未找到 SOF 段")


# ────────────────────────────── 帧来源 ──────────────────────────────

def frames_from_video(args):
    """用 ffmpeg 抽帧为 baseline JPEG 序列，返回临时目录与文件列表。"""
    ffmpeg = args.ffmpeg or shutil.which("ffmpeg")
    if not ffmpeg:
        sys.exit("找不到 ffmpeg。请用 --ffmpeg 指定路径，或先用其它工具导出 JPEG 序列后"
                 "改用 --frames-dir。")
    tmpdir = tempfile.mkdtemp(prefix="jwp_frames_")
    pattern = os.path.join(tmpdir, "f_%05d.jpg")

    # 调色分级（沿用 make_wallpaper_video.sh / ADR-019 的定稿）：
    #   day      = 轻度压暗，保证待机层白字可读
    #   night    = 素材本身是夜景，只做极轻处理
    #   nightdark= 用白天素材凑夜景（深压暗 + 夜色偏置）
    grade = {
        "day": "eq=brightness=-0.05:saturation=0.92",
        "night": "eq=brightness=-0.04:saturation=0.9",
        "nightdark": "eq=brightness=-0.14:saturation=0.75,colorbalance=rs=-0.08:bs=0.10",
        "none": None,
    }[args.grade]

    vf = (f"scale={args.width}:{args.height}:force_original_aspect_ratio=increase,"
          f"crop={args.width}:{args.height},fps={args.fps}")
    if grade:
        vf += f",{grade}"
    vf += ",format=yuv420p"

    cmd = [ffmpeg, "-y", "-v", "error", "-i", args.input]
    if args.duration:
        cmd += ["-t", str(args.duration)]
    cmd += ["-vf", vf, "-c:v", "mjpeg", "-q:v", str(args.quality),
            "-an", "-f", "image2", pattern]

    print(f"[ffmpeg] {' '.join(cmd)}")
    subprocess.run(cmd, check=True)

    files = sorted(glob.glob(os.path.join(tmpdir, "f_*.jpg")))
    if not files:
        sys.exit("ffmpeg 没有产出任何帧")
    return tmpdir, files


def frames_from_dir(path, args):
    files = sorted(
        f for f in glob.glob(os.path.join(path, "*"))
        if f.lower().endswith((".jpg", ".jpeg"))
    )
    if not files:
        sys.exit(f"{path} 下没有 JPEG 文件")
    return None, files


def frames_from_image(path, args):
    """单张图 → 多次重复的帧（用于生成极小的测试样例/静态兜底壁纸）。"""
    from PIL import Image
    tmpdir = tempfile.mkdtemp(prefix="jwp_img_")
    img = Image.open(path).convert("RGB")
    img = fit_cover(img, args.width, args.height)
    files = []
    for i in range(max(1, args.repeat)):
        out = os.path.join(tmpdir, f"f_{i:05d}.jpg")
        img.save(out, "JPEG", quality=args.pil_quality, optimize=True)
        files.append(out)
    return tmpdir, files


def fit_cover(img, width, height):
    """等比缩放铺满后居中裁剪（与 make_wallpaper_video.sh 的 crop 语义一致）。"""
    from PIL import Image
    src_ratio = img.width / img.height
    dst_ratio = width / height
    if src_ratio > dst_ratio:
        new_h = height
        new_w = int(round(height * src_ratio))
    else:
        new_w = width
        new_h = int(round(width / src_ratio))
    img = img.resize((new_w, new_h), Image.LANCZOS)
    left = (new_w - width) // 2
    top = (new_h - height) // 2
    return img.crop((left, top, left + width, top + height))


def frames_from_gradient(args):
    """纯渐变 + 移动光斑的合成帧（无素材时生成测试样例）。"""
    from PIL import Image, ImageDraw
    tmpdir = tempfile.mkdtemp(prefix="jwp_gen_")
    w, h = args.width, args.height
    files = []
    count = max(1, args.repeat)
    for i in range(count):
        img = Image.new("RGB", (w, h))
        dr = ImageDraw.Draw(img)
        # 垂直渐变（昼/夜两套配色）
        if args.night:
            top, bottom = (11, 16, 36), (27, 42, 74)
        else:
            top, bottom = (46, 111, 183), (156, 200, 232)
        for y in range(h):
            t = y / max(1, h - 1)
            dr.line([(0, y), (w, y)],
                    fill=(int(top[0] + (bottom[0] - top[0]) * t),
                          int(top[1] + (bottom[1] - top[1]) * t),
                          int(top[2] + (bottom[2] - top[2]) * t)))
        # 移动光斑：给播放器一个"帧在变"的直观依据
        cx = int(w * (0.25 + 0.5 * (i / max(1, count))))
        r = h // 6
        dr.ellipse([cx - r, h // 3 - r, cx + r, h // 3 + r],
                   fill=(255, 236, 190) if not args.night else (210, 220, 255))
        out = os.path.join(tmpdir, f"f_{i:05d}.jpg")
        img.save(out, "JPEG", quality=args.pil_quality, optimize=True)
        files.append(out)
    return tmpdir, files


# ────────────────────────────── 打包 ──────────────────────────────

def pack_jwp(files, args):
    frames = []
    for path in files:
        with open(path, "rb") as f:
            data = f.read()
        w, h, baseline = jpeg_info(data)
        if (w, h) != (args.width, args.height):
            sys.exit(f"{os.path.basename(path)} 尺寸 {w}×{h} != {args.width}×{args.height}；"
                     f"固件播放器要求与 JWP 头完全一致。")
        if not baseline:
            sys.exit(f"{os.path.basename(path)} 是 progressive JPEG —— 固件（硬件解码器）"
                     f"只支持 baseline。请用 ffmpeg -huffman optimal 或 PIL 重新编码。")
        if len(data) > args.max_frame_bytes:
            sys.exit(f"{os.path.basename(path)} 单帧 {len(data)} 字节 > 上限 "
                     f"{args.max_frame_bytes} 字节（--max-frame）；"
                     f"请提高 --quality 数值（更差画质=更小体积）或缩短 --duration。")
        frames.append(data)

    frame_count = len(frames)
    max_frame_bytes = max(len(d) for d in frames)

    # transition_offset：默认 0（回到第一帧）。--transition-frame N 表示
    # "循环体从第 N 帧开始"，用于带片头的壁纸（片头只播一次）。
    transition_offset = 0
    if args.transition_frame:
        n = args.transition_frame
        if not 0 <= n < frame_count:
            sys.exit(f"--transition-frame {n} 超出范围 0..{frame_count - 1}")
        transition_offset = HEADER_SIZE + sum(4 + len(d) for d in frames[:n])

    payload = io.BytesIO()
    for data in frames:
        payload.write(struct.pack("<I", len(data)))
        payload.write(data)
    payload_bytes = payload.getvalue()
    crc = zlib.crc32(payload_bytes) & 0xFFFFFFFF

    header = struct.pack(
        "<4sHHHHHHIIII",
        MAGIC, VERSION, HEADER_SIZE,
        args.width, args.height, int(round(args.fps * 100)),
        PIXEL_FORMAT_BASELINE_JPEG,
        frame_count, max_frame_bytes, crc, transition_offset,
    )
    assert len(header) == HEADER_SIZE, len(header)

    out_dir = os.path.dirname(os.path.abspath(args.output))
    os.makedirs(out_dir, exist_ok=True)
    with open(args.output, "wb") as f:
        f.write(header)
        f.write(payload_bytes)

    sizes = [len(d) for d in frames]
    print(f"完成: {args.output}")
    print(f"  分辨率   : {args.width}×{args.height}")
    print(f"  帧率     : {args.fps} fps ({int(round(args.fps * 100))} × 0.01)")
    print(f"  帧数     : {frame_count}")
    print(f"  单帧字节 : min={min(sizes)} avg={sum(sizes) // frame_count} max={max_frame_bytes}"
          f"  (上限 {args.max_frame_bytes})")
    print(f"  CRC32    : 0x{crc:08X}   transition_offset={transition_offset}")
    print(f"  文件大小 : {HEADER_SIZE + len(payload_bytes)} 字节")
    if abs(args.fps - args.firmware_fps) > 0.01:
        print(f"  ⚠️  JWP 帧率 {args.fps} != 固件默认目标 {args.firmware_fps} fps；"
              f"播放器按自己的目标帧率顺序取帧，帧率不一致会改变播放快慢。")
    return 0


def inspect(path, max_frame_kb):
    with open(path, "rb") as f:
        raw = f.read()
    if len(raw) < HEADER_SIZE:
        sys.exit("文件小于 32 字节，不是 JWP")
    (magic, ver, hsize, w, h, fps100, fmt,
     count, maxb, crc, toff) = struct.unpack("<4sHHHHHHIIII", raw[:HEADER_SIZE])
    if magic != MAGIC:
        sys.exit(f"magic 不是 JWP1：{magic!r}")
    payload = raw[HEADER_SIZE:]
    real_crc = zlib.crc32(payload) & 0xFFFFFFFF
    print(f"{path}")
    print(f"  version={ver} header_size={hsize} {w}×{h} fps={fps100 / 100:.2f} "
          f"pixel_format={fmt}")
    print(f"  frame_count={count} max_frame_bytes={maxb} transition_offset={toff}")
    print(f"  payload_crc32=0x{crc:08X} 实测=0x{real_crc:08X} "
          f"{'一致' if crc == real_crc else '不一致（0 表示未写入）'}")

    off = HEADER_SIZE
    n = 0
    sizes = []
    while off + 4 <= len(raw) and n < count:
        (ln,) = struct.unpack("<I", raw[off:off + 4])
        off += 4
        if off + ln > len(raw):
            print(f"  ⚠️ 第 {n} 帧被截断（声明 {ln} 字节，剩余 {len(raw) - off}）")
            break
        data = raw[off:off + ln]
        off += ln
        try:
            fw, fh, baseline = jpeg_info(data)
        except ValueError as e:
            print(f"  ⚠️ 第 {n} 帧 JPEG 解析失败: {e}")
            break
        if (fw, fh) != (w, h):
            print(f"  ⚠️ 第 {n} 帧尺寸 {fw}×{fh} != 头部 {w}×{h}")
        if not baseline:
            print(f"  ⚠️ 第 {n} 帧是 progressive JPEG（固件不支持）")
        if ln > max_frame_kb * 1024:
            print(f"  ⚠️ 第 {n} 帧 {ln} 字节 > 固件上限 {max_frame_kb} KB")
        sizes.append(ln)
        n += 1

    print(f"  实际读取 {n} 帧；单帧 min={min(sizes) if sizes else 0} "
          f"max={max(sizes) if sizes else 0}")
    return 0


# ────────────────────────────── CLI ──────────────────────────────

def main():
    p = argparse.ArgumentParser(
        description="视频/图片序列 → JWP 动态壁纸（ADR-020）",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--input", help="输入视频（mp4/avi/mov/mkv）")
    p.add_argument("--frames-dir", help="输入：已有的 JPEG 序列目录")
    p.add_argument("--image", help="输入：单张图片（可配合 --repeat 生成静态壁纸）")
    p.add_argument("--gradient", action="store_true",
                   help="无素材时合成渐变测试帧（配合 --repeat）")
    p.add_argument("--night", action="store_true", help="合成/调色用夜间配色")
    p.add_argument("--output", help="输出 .jwp 路径")
    p.add_argument("--width", type=int, default=1024, help="宽（默认 1024）")
    p.add_argument("--height", type=int, default=600, help="高（默认 600）")
    p.add_argument("--fps", type=float, default=15.0,
                   help="JWP 帧率（默认 15，与固件 SMART_HOME_WALLPAPER_FPS 一致）")
    p.add_argument("--firmware-fps", type=float, default=15.0,
                   help="固件目标帧率，仅用于提示帧率不一致（默认 15）")
    p.add_argument("--quality", type=int, default=4,
                   help="ffmpeg MJPEG 质量，2(最好)~31(最差)，默认 4")
    p.add_argument("--grade", choices=["day", "night", "nightdark", "none"], default="day",
                   help="调色分级（仅视频输入）：day=轻度压暗保白字可读(默认) / "
                        "night=素材本身是夜景 / nightdark=白天素材凑夜景 / none=原样")
    p.add_argument("--pil-quality", type=int, default=80,
                   help="PIL 重编码质量（--image/--gradient 用），默认 80")
    p.add_argument("--duration", type=float, default=10.0, help="视频截取秒数，默认 10")
    p.add_argument("--max-frame", type=int, default=512,
                   help="单帧上限 KB，须与 SMART_HOME_WALLPAPER_MAX_FRAME_KB 一致")
    p.add_argument("--transition-frame", type=int, default=0,
                   help="循环回跳的帧号（片头只播一次时使用），默认 0")
    p.add_argument("--repeat", type=int, default=4,
                   help="--image/--gradient 生成多少帧，默认 4")
    p.add_argument("--ffmpeg", help="ffmpeg 可执行文件路径")
    p.add_argument("--inspect", help="只检查一个已有 JWP 文件")
    args = p.parse_args()

    args.max_frame_bytes = args.max_frame * 1024

    if args.inspect:
        return inspect(args.inspect, args.max_frame)

    if not args.output:
        p.error("需要 --output（或使用 --inspect）")

    tmpdir = None
    if args.input:
        tmpdir, files = frames_from_video(args)
    elif args.frames_dir:
        tmpdir, files = frames_from_dir(args.frames_dir, args)
    elif args.image:
        tmpdir, files = frames_from_image(args.image, args)
    elif args.gradient:
        tmpdir, files = frames_from_gradient(args)
    else:
        p.error("需要 --input / --frames-dir / --image / --gradient 之一")

    try:
        return pack_jwp(files, args)
    finally:
        if tmpdir:
            shutil.rmtree(tmpdir, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())