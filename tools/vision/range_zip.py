"""Minimal HTTP-range reader for remote ZIP archives.

Only what this directory needs: list entries from the central directory and
extract single members without downloading the whole archive (WIDER FACE val
is 363 MB; the subset is a few MB).

Supports stored (method 0) and deflated (method 8) members, non-Zip64 files
up to 4 GiB. Every range request is retried via http_util.http_get because
the network drops TLS handshakes intermittently.
"""
from __future__ import annotations

import struct
import zlib

from http_util import http_get

_EOCD = b"PK\x05\x06"
_CENTRAL = b"PK\x01\x02"
_LOCAL = b"PK\x03\x04"


class RemoteZip:
    def __init__(self, url: str) -> None:
        self.url = url
        self.size = 0
        self.entries: dict[str, dict] = {}
        self._load_central_directory()

    # ── public ────────────────────────────────────────────────
    def namelist(self) -> list[str]:
        return list(self.entries)

    def read(self, name: str) -> bytes:
        ent = self.entries[name]
        head = self._range(ent["local_offset"], ent["local_offset"] + 29)
        if head[:4] != _LOCAL:
            raise RuntimeError(f"bad local header for {name}")
        name_len, extra_len = struct.unpack("<HH", head[26:30])
        data_start = ent["local_offset"] + 30 + name_len + extra_len
        raw = self._range(data_start, data_start + ent["compress_size"] - 1)

        if ent["method"] == 0:
            return raw
        if ent["method"] == 8:
            return zlib.decompress(raw, -15)  # zip stores raw deflate, no zlib header
        raise RuntimeError(f"unsupported compression method {ent['method']} for {name}")

    # ── internals ─────────────────────────────────────────────
    def _load_central_directory(self) -> None:
        tail_len = 1 << 16
        tail = self._range_suffix(tail_len)
        pos = tail.rfind(_EOCD)
        if pos < 0:
            raise RuntimeError("EOCD not found (archive too large for tail window?)")
        comment_len = struct.unpack("<H", tail[pos + 20:pos + 22])[0]
        eocd = tail[pos:]
        if comment_len and len(tail) - pos < 22 + comment_len:
            tail = self._range_suffix(22 + comment_len + 64)
            pos = tail.rfind(_EOCD)
            eocd = tail[pos:]
        _, _, _, _, total_entries, cd_size, cd_offset, _ = struct.unpack(
            "<IHHHHIIH", eocd[:22])

        cd = self._range(cd_offset, cd_offset + cd_size - 1)
        idx = 0
        for _ in range(total_entries):
            if cd[idx:idx + 4] != _CENTRAL:
                break
            (method, _mtime, _mdate, _crc, comp_size, uncomp_size, name_len,
             extra_len, comment_len, _disk, _int_attr, _ext_attr,
             local_offset) = struct.unpack("<HHHIIIHHHHHII", cd[idx + 10:idx + 46])
            raw_name = cd[idx + 46:idx + 46 + name_len]
            name = raw_name.decode("utf-8", errors="replace")
            self.entries[name] = {
                "method": method,
                "compress_size": comp_size,
                "size": uncomp_size,
                "local_offset": local_offset,
            }
            idx += 46 + name_len + extra_len + comment_len

    def _range_suffix(self, nbytes: int) -> bytes:
        resp = http_get(self.url, headers={"Range": f"bytes=-{nbytes}"}, timeout=120)
        if resp.status_code != 206:
            raise RuntimeError(f"server does not honour range requests (HTTP {resp.status_code})")
        content_range = resp.headers["Content-Range"]
        self.size = int(content_range.rsplit("/", 1)[1])
        return resp.content

    def _range(self, start: int, end: int) -> bytes:
        resp = http_get(self.url, headers={"Range": f"bytes={start}-{end}"}, timeout=120)
        if resp.status_code != 206:
            raise RuntimeError(f"range {start}-{end} failed (HTTP {resp.status_code})")
        return resp.content
