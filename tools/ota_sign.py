#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
OTA manifest 签名工具（ECDSA-P256-SHA256）。

签名对象是规范化元数据，而不是裸摘要：

    canonical = "shh-ota-v1|<p4|f103>|<version>|<size>|<sha256 小写>"

设备端对 canonical 做 SHA256 后，用内置公钥验签；manifest 里的 "sig"
是 raw r||s（各 32 字节，共 128 hex）。这样 version/size/sha256 都被认证，
避免“旧镜像改高版本号”的重放。

用法：
  # 首次生成开发密钥（私钥写进 tools/ota_keys/，已 .gitignore）
  python tools/ota_sign.py --gen-key

  # 重算 size/sha256 并填 sig（保留 manifest 里已有的 url / version）
  python tools/ota_sign.py --manifest manifest.json \
      --p4-bin build/p4-hub.bin --p4-version 0.9.1-ota-sign \
      --f103-bin /path/f103-node.bin --f103-version 1.0.1

  # 只校验现有签名
  python tools/ota_sign.py --manifest manifest.json --verify
"""
import argparse
import hashlib
import json
import os
import sys
from pathlib import Path

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import (
    decode_dss_signature,
    encode_dss_signature,
)

CANON_PREFIX = "shh-ota-v1"
COMPONENTS = ("p4", "f103")
REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_KEY_DIR = Path(__file__).resolve().parent / "ota_keys"
DEFAULT_KEY = DEFAULT_KEY_DIR / "ota_priv.pem"


def canonical(component: str, version: str, size: int, sha256: str) -> bytes:
    return f"{CANON_PREFIX}|{component}|{version}|{int(size)}|{sha256.lower()}".encode()


def raw_signature(private_key, message: bytes) -> str:
    """cryptography 输出 DER，这里转成设备端要求的 raw r||s hex。"""
    der = private_key.sign(message, ec.ECDSA(hashes.SHA256()))
    r, s = decode_dss_signature(der)
    return r.to_bytes(32, "big").hex() + s.to_bytes(32, "big").hex()


def raw_verify(public_key, signature_hex: str, message: bytes) -> bool:
    try:
        raw = bytes.fromhex(signature_hex)
    except ValueError:
        return False
    if len(raw) != 64:
        return False
    r = int.from_bytes(raw[:32], "big")
    s = int.from_bytes(raw[32:], "big")
    der = encode_dss_signature(r, s)
    try:
        public_key.verify(der, message, ec.ECDSA(hashes.SHA256()))
        return True
    except Exception:
        return False


def public_hex(public_key) -> str:
    numbers = public_key.public_numbers()
    return numbers.x.to_bytes(32, "big").hex() + numbers.y.to_bytes(32, "big").hex()


def gen_key(out_dir: Path) -> int:
    out_dir.mkdir(parents=True, exist_ok=True)
    private_key = ec.generate_private_key(ec.SECP256R1())
    priv_path = out_dir / "ota_priv.pem"
    pem = private_key.private_bytes(
        serialization.Encoding.PEM,
        serialization.PrivateFormat.PKCS8,
        serialization.NoEncryption(),
    )
    priv_path.write_bytes(pem)
    try:
        os.chmod(priv_path, 0o600)
    except OSError:
        pass

    pub = public_hex(private_key.public_key())
    (out_dir / "ota_pub.hex").write_text(pub + "\n", encoding="ascii")
    print(f"private key: {priv_path}")
    print(f"public key : {pub}")
    print(f"Kconfig    : CONFIG_SMART_HOME_NET_OTA_PUBKEY_HEX=\"{pub}\"")
    return 0


def load_key(path: Path):
    return serialization.load_pem_private_key(path.read_bytes(), password=None)


def sign_component(manifest: dict, comp: str, bin_path: Path, version: str | None,
                   private_key) -> None:
    obj = manifest.get(comp)
    if not isinstance(obj, dict):
        raise SystemExit(f"manifest 缺少对象 '{comp}'")

    data = bin_path.read_bytes()
    size = len(data)
    sha256 = hashlib.sha256(data).hexdigest()
    if version:
        obj["version"] = version
    version = str(obj.get("version") or "")
    if not version:
        raise SystemExit(f"'{comp}' 缺少 version（用 --{comp}-version 指定）")

    obj["size"] = size
    obj["sha256"] = sha256
    obj["sig"] = raw_signature(
        private_key, canonical(comp, version, size, sha256))
    print(f"[{comp}] v{version} size={size} sha256={sha256}")
    print(f"[{comp}] sig={obj['sig']}")


def verify_manifest(manifest: dict, public_key) -> int:
    failed = 0
    for comp in COMPONENTS:
        obj = manifest.get(comp)
        if not isinstance(obj, dict):
            continue
        sig = obj.get("sig")
        if not isinstance(sig, str):
            print(f"[{comp}] 无 sig，跳过")
            continue
        try:
            version = str(obj["version"])
            size = int(obj["size"])
            sha256 = str(obj["sha256"])
        except (KeyError, TypeError, ValueError):
            print(f"[{comp}] 字段缺失，FAIL")
            failed += 1
            continue
        ok = raw_verify(public_key, sig, canonical(comp, version, size, sha256))
        print(f"[{comp}] v{version} {'OK' if ok else 'FAIL'}")
        failed += 0 if ok else 1
    return 1 if failed else 0


def main() -> int:
    ap = argparse.ArgumentParser(description="OTA manifest ECDSA-P256 签名工具")
    ap.add_argument("--gen-key", action="store_true", help="生成 P-256 密钥对")
    ap.add_argument("--key-dir", type=Path, default=DEFAULT_KEY_DIR)
    ap.add_argument("--key", type=Path, default=DEFAULT_KEY)
    ap.add_argument("--manifest", type=Path, help="输入/输出 manifest.json")
    ap.add_argument("--p4-bin", type=Path)
    ap.add_argument("--f103-bin", type=Path)
    ap.add_argument("--p4-version")
    ap.add_argument("--f103-version")
    ap.add_argument("--out", type=Path, help="输出路径（默认覆盖 --manifest）")
    ap.add_argument("--verify", action="store_true", help="只校验 manifest 签名")
    args = ap.parse_args()

    if args.gen_key:
        return gen_key(args.key_dir)

    if not args.manifest:
        ap.print_help()
        return 2

    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    if not isinstance(manifest, dict):
        raise SystemExit("manifest 根节点必须是对象")

    try:
        private_key = load_key(args.key)
    except FileNotFoundError:
        raise SystemExit(
            f"私钥不存在：{args.key}\n先运行 python tools/ota_sign.py --gen-key")

    if args.verify:
        return verify_manifest(manifest, private_key.public_key())

    signed = False
    if args.p4_bin:
        sign_component(manifest, "p4", args.p4_bin, args.p4_version, private_key)
        signed = True
    if args.f103_bin:
        sign_component(manifest, "f103", args.f103_bin, args.f103_version, private_key)
        signed = True

    if not signed:
        raise SystemExit("没有指定要签名的镜像（--p4-bin / --f103-bin）")

    out = args.out or args.manifest
    out.write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n",
                   encoding="utf-8")
    print(f"written: {out}")

    if verify_manifest(manifest, private_key.public_key()) != 0:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
