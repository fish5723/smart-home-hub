"""Fetch 10 no-face negatives (real indoor scenes) from Wikimedia Commons.

WIDER FACE validation contains no zero-face images (all 3226 have faces), and
the "background" mirror on HuggingFace ships a single 365 MB parquet row group
that cannot be range-read, so negatives come from Commons instead: photos whose
title starts with "Empty apartment/room/office", i.e. interiors without people.

Attribution (title, page, license) is stored in negatives.json next to the
images. The files stay local (data/ is git-ignored).

Usage  : python tools/vision/fetch_negatives.py [--count 10]
Output : tools/vision/data/wider_subset/images/negatives/*.jpg
         tools/vision/data/wider_subset/negatives.json
"""
from __future__ import annotations

import argparse
import json
import re
import time
from pathlib import Path

import requests
from PIL import Image

from http_util import http_get

API = "https://commons.wikimedia.org/w/api.php"
HEAD = {"User-Agent": "smart-home-hub-testset/1.0 (local evaluation)"}

OUT_ROOT = Path(__file__).resolve().parent / "data" / "wider_subset"
NEG_DIR = OUT_ROOT / "images" / "negatives"

SEARCHES = [
    'intitle:"empty apartment" filetype:bitmap',
    'intitle:"empty room" filetype:bitmap',
    'intitle:"empty office" filetype:bitmap',
]

# Titles that suggest people are present — exclude before downloading.
REJECT = re.compile(r"person|man |woman|people|child|couple|portrait|family|"
                    r"meeting|staff|worker|renovation|demolish", re.I)


def search(q: str, limit: int = 20) -> list[dict]:
    params = {
        "action": "query", "generator": "search", "gsrsearch": q,
        "gsrnamespace": "6", "gsrlimit": str(limit), "prop": "imageinfo",
        "iiprop": "url|size|extmetadata", "iiurlwidth": "800", "format": "json",
    }
    for attempt in range(6):
        try:
            resp = requests.get(API, params=params, timeout=40, headers=HEAD)
            pages = (resp.json().get("query") or {}).get("pages") or {}
            return [p for p in pages.values()
                    if (p.get("imageinfo") or [{}])[0].get("thumburl")]
        except Exception:  # noqa: BLE001 - flaky TLS to commons as well
            time.sleep(2 * (attempt + 1))
    print(f"[warn] search failed: {q}")
    return []


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--count", type=int, default=10)
    args = ap.parse_args()

    seen: set[str] = set()
    candidates: list[dict] = []
    for q in SEARCHES:
        for page in search(q):
            title = page["title"]
            if title in seen or REJECT.search(title):
                continue
            seen.add(title)
            candidates.append(page)
        if len(candidates) >= args.count * 2:
            break

    NEG_DIR.mkdir(parents=True, exist_ok=True)
    entries: list[dict] = []
    for page in candidates:
        if len(entries) >= args.count:
            break
        ii = page["imageinfo"][0]
        url = ii.get("thumburl") or ii["url"]
        try:
            resp = http_get(url, headers=HEAD, timeout=90)
            resp.raise_for_status()
        except Exception as exc:  # noqa: BLE001
            print(f"[warn] download failed: {page['title']} ({exc})")
            continue

        name = re.sub(r"[^\w.-]+", "_", page["title"].removeprefix("File:"))[:80]
        if not name.lower().endswith((".jpg", ".jpeg", ".png")):
            name += ".jpg"
        dest = NEG_DIR / name
        with open(dest, "wb") as fh:
            fh.write(resp.content)
        with Image.open(dest) as im:
            width, height = im.size

        meta = ii.get("extmetadata") or {}
        entries.append({
            "file": f"negatives/{name}",
            "width": width,
            "height": height,
            "faces": [],
            "source": "Wikimedia Commons",
            "title": page["title"],
            "page": ii.get("descriptionurl"),
            "license": (meta.get("LicenseShortName") or {}).get("value"),
            "artist": re.sub("<[^>]+>", "", (meta.get("Artist") or {}).get("value", "")),
        })
        print(f"[ok] {page['title']} {width}x{height}")

    (OUT_ROOT / "negatives.json").write_text(json.dumps({
        "source": "Wikimedia Commons — interiors whose title starts with Empty apartment/room/office",
        "count": len(entries),
        "images": entries,
    }, indent=2, ensure_ascii=False), encoding="utf-8")
    print(f"[done] {len(entries)} negatives → {NEG_DIR}")


if __name__ == "__main__":
    main()
