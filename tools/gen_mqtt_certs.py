#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
生成 MQTT TLS 测试用的 CA + 服务器证书（本地 mosquitto 用）。

为什么用 python 而不是 openssl：
  本机没有 openssl.exe，但 ESP-IDF 自带的 python 环境里装了 cryptography 48.x。

产物（私钥全部留在 %TEMP%，不进仓库）：
  %TEMP%\\mqtt-broker\\certs\\ca.crt        自签 CA 证书（公开）
  %TEMP%\\mqtt-broker\\certs\\ca.key        CA 私钥（只在本机，勿外传）
  %TEMP%\\mqtt-broker\\certs\\server.crt    服务器证书（mosquitto 用）
  %TEMP%\\mqtt-broker\\certs\\server.key    服务器私钥

同时把**CA 证书**复制到两个固件工程的 certs/ 目录（公开证书可以入库）：
  firmware/p4-hub/certs/ca.pem
  firmware/c3-node/certs/ca.pem

用法：
  python tools/gen_mqtt_certs.py [--ip 10.44.209.2]
  不带 --ip 时自动收集本机所有 IPv4 地址写进 SAN（热点换 IP 后重跑即可）。
"""
import argparse
import datetime
import ipaddress
import os
import shutil
import socket
import sys

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa
from cryptography.x509.oid import ExtendedKeyUsageOID, NameOID

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
CERT_DIR = os.path.join(os.environ.get("TEMP", "."), "mqtt-broker", "certs")
DNS_NAMES = ["localhost", "smart-home-broker.local"]


def local_ipv4s():
    """收集本机 IPv4（24.0.0.0/8 等保留网段由 SSRF 库判断，这里只筛私有地址）。"""
    ips = {"127.0.0.1"}
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            ips.add(info[4][0])
    except OSError:
        pass
    # 额外探测默认出口地址（连接不会真正建立）
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 53))
        ips.add(s.getsockname()[0])
        s.close()
    except OSError:
        pass
    return sorted(ips)


def build_ca():
    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    name = x509.Name([
        x509.NameAttribute(NameOID.ORGANIZATION_NAME, "Smart Home Hub Lab"),
        x509.NameAttribute(NameOID.COMMON_NAME, "Smart Home Hub Test CA"),
    ])
    now = datetime.datetime.now(datetime.timezone.utc)
    cert = (
        x509.CertificateBuilder()
        .subject_name(name)
        .issuer_name(name)
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now - datetime.timedelta(days=1))
        .not_valid_after(now + datetime.timedelta(days=3650))
        .add_extension(x509.BasicConstraints(ca=True, path_length=0), critical=True)
        .add_extension(
            x509.KeyUsage(
                digital_signature=True, content_commitment=False,
                key_encipherment=False, data_encipherment=False,
                key_agreement=False, key_cert_sign=True, crl_sign=True,
                encipher_only=False, decipher_only=False,
            ),
            critical=True,
        )
        .add_extension(
            x509.SubjectKeyIdentifier.from_public_key(key.public_key()), critical=False
        )
        .sign(key, hashes.SHA256())
    )
    return key, cert


def load_ca():
    """读取已有 CA；两个文件缺一不可，否则返回 None。

    CA 是设备的信任根，设备固件里烧的是 ca.pem。换 IP 时只该重签服务器证书，
    绝不能顺手换 CA —— 否则必须重烧两块板。
    """
    crt_path = os.path.join(CERT_DIR, "ca.crt")
    key_path = os.path.join(CERT_DIR, "ca.key")
    if not (os.path.isfile(crt_path) and os.path.isfile(key_path)):
        return None
    with open(crt_path, "rb") as f:
        cert = x509.load_pem_x509_certificate(f.read())
    with open(key_path, "rb") as f:
        key = serialization.load_pem_private_key(f.read(), password=None)
    return key, cert


def build_server(ca_key, ca_cert, ips):
    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    now = datetime.datetime.now(datetime.timezone.utc)
    san = [x509.DNSName(n) for n in DNS_NAMES]
    san += [x509.IPAddress(ipaddress.ip_address(ip)) for ip in ips]
    cert = (
        x509.CertificateBuilder()
        .subject_name(x509.Name([
            x509.NameAttribute(NameOID.ORGANIZATION_NAME, "Smart Home Hub Lab"),
            x509.NameAttribute(NameOID.COMMON_NAME, "smart-home-broker"),
        ]))
        .issuer_name(ca_cert.subject)
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now - datetime.timedelta(days=1))
        .not_valid_after(now + datetime.timedelta(days=825))
        .add_extension(x509.BasicConstraints(ca=False, path_length=None), critical=True)
        .add_extension(
            x509.ExtendedKeyUsage([ExtendedKeyUsageOID.SERVER_AUTH]), critical=False
        )
        .add_extension(x509.SubjectAlternativeName(san), critical=False)
        .add_extension(
            x509.AuthorityKeyIdentifier.from_issuer_public_key(ca_key.public_key()),
            critical=False,
        )
        .sign(ca_key, hashes.SHA256())
    )
    return key, cert


def write_pem(path, obj, private=False):
    if private:
        data = obj.private_bytes(
            encoding=serialization.Encoding.PEM,
            format=serialization.PrivateFormat.TraditionalOpenSSL,
            encryption_algorithm=serialization.NoEncryption(),
        )
    else:
        data = obj.public_bytes(serialization.Encoding.PEM)
    with open(path, "wb") as f:
        f.write(data)
    print("  wrote", path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ip", action="append", default=None,
                    help="额外写进 SAN 的 IP（可重复）；不给则自动收集本机 IPv4")
    args = ap.parse_args()

    ips = set(args.ip or []) | set(local_ipv4s())
    ips = sorted(ips)

    os.makedirs(CERT_DIR, exist_ok=True)
    print("证书目录:", CERT_DIR)
    print("SAN IP  :", ", ".join(ips))
    print("SAN DNS :", ", ".join(DNS_NAMES))

    existing = load_ca()
    if existing is not None:
        ca_key, ca_cert = existing
        print("CA 已复用（未变更）——换 IP 只重签服务器证书，设备无需重烧")
    else:
        ca_key, ca_cert = build_ca()
        print("CA 已新建")
        write_pem(os.path.join(CERT_DIR, "ca.crt"), ca_cert)
        write_pem(os.path.join(CERT_DIR, "ca.key"), ca_key, private=True)

    srv_key, srv_cert = build_server(ca_key, ca_cert, ips)

    write_pem(os.path.join(CERT_DIR, "server.crt"), srv_cert)
    write_pem(os.path.join(CERT_DIR, "server.key"), srv_key, private=True)

    # 公开 CA 证书复制到两个工程（供 build 时打进 crt bundle）
    for proj in ("p4-hub", "c3-node"):
        dst_dir = os.path.join(REPO_ROOT, "firmware", proj, "certs")
        os.makedirs(dst_dir, exist_ok=True)
        dst = os.path.join(dst_dir, "ca.pem")
        shutil.copyfile(os.path.join(CERT_DIR, "ca.crt"), dst)
        print("  copied ->", dst)

    fp = ca_cert.fingerprint(hashes.SHA256()).hex()
    print("\nCA SHA-256:", fp)
    print("注意：ca.key / server.key 只在 %TEMP%，不要提交进仓库。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
