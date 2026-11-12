"""Common HTTP helpers for tools/vision scripts.

All downloads in this directory are plain HTTP(S) with retries: the network
here intermittently drops TLS handshakes (SSLEOFError), so every request
gets a few retries before failing.
"""
from __future__ import annotations

import hashlib
import time
from pathlib import Path

import requests


def http_get(url: str, *, headers: dict | None = None, retries: int = 8,
             timeout: int = 60) -> requests.Response:
    """GET with retries; returns the Response (caller closes / streams)."""
    last = None
    for attempt in range(retries):
        try:
            return requests.get(url, headers=headers, allow_redirects=True, timeout=timeout)
        except Exception as exc:  # noqa: BLE001 - network layer is unreliable
            last = exc
            time.sleep(1.5 * (attempt + 1))
    raise RuntimeError(f"download failed after {retries} tries: {url}") from last


def download_file(url: str, dest: Path, *, sha256: str | None = None,
                  chunk: int = 1 << 16) -> Path:
    """Download url to dest atomically, verifying sha256 when given."""
    dest.parent.mkdir(parents=True, exist_ok=True)
    if dest.exists() and sha256 and _sha256(dest) == sha256:
        print(f"[skip] {dest.name} already present (sha256 ok)")
        return dest

    tmp = dest.with_suffix(dest.suffix + ".part")
    resp = http_get(url, timeout=120)
    resp.raise_for_status()
    with open(tmp, "wb") as fh:
        for block in resp.iter_content(chunk):
            fh.write(block)
    resp.close()

    if sha256:
        actual = _sha256(tmp)
        if actual != sha256:
            tmp.unlink(missing_ok=True)
            raise RuntimeError(f"sha256 mismatch for {dest.name}: {actual} != {sha256}")
    tmp.replace(dest)
    print(f"[ok] {dest} ({dest.stat().st_size} bytes)")
    return dest


def download_resumable(url: str, dest: Path, *, chunk: int = 4 << 20,
                       retries_per_chunk: int = 6) -> Path:
    """Download a large file in HTTP range chunks, resuming after TLS drops."""
    dest.parent.mkdir(parents=True, exist_ok=True)
    part = dest.with_suffix(dest.suffix + ".part")

    total = _remote_size(url)
    offset = part.stat().st_size if part.exists() else 0
    if offset >= total:
        part.replace(dest)
        return dest

    while offset < total:
        end = min(offset + chunk - 1, total - 1)
        last = None
        for attempt in range(retries_per_chunk):
            try:
                resp = requests.get(url, headers={"Range": f"bytes={offset}-{end}"},
                                    allow_redirects=True, timeout=120)
                if resp.status_code != 206:
                    raise RuntimeError(f"HTTP {resp.status_code} for range {offset}-{end}")
                body = resp.content
                if len(body) != end - offset + 1:
                    raise RuntimeError(f"short range body: {len(body)} bytes")
                with open(part, "ab") as fh:
                    fh.write(body)
                offset += len(body)
                break
            except Exception as exc:  # noqa: BLE001
                last = exc
                time.sleep(1.5 * (attempt + 1))
        else:
            raise RuntimeError(f"chunk at {offset} failed: {url}") from last
        print(f"\r[dl] {dest.name} {offset / 1e6:.1f}/{total / 1e6:.1f} MB", end="")

    print()
    part.replace(dest)
    print(f"[ok] {dest} ({dest.stat().st_size} bytes)")
    return dest


def _remote_size(url: str) -> int:
    resp = http_get(url, headers={"Range": "bytes=0-0"}, timeout=60)
    if resp.status_code != 206:
        raise RuntimeError(f"range requests unsupported for {url} (HTTP {resp.status_code})")
    return int(resp.headers["Content-Range"].rsplit("/", 1)[1])


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as fh:
        for block in iter(lambda: fh.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()
