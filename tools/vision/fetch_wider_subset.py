"""Fetch 40 WIDER FACE validation images (with face boxes) as the PC test set.

WIDER FACE val is a 363 MB archive, so this script downloads the annotation
split (3.6 MB) and pulls only the selected JPEGs out of the remote zip with
HTTP range requests (~10 MB total).

Source : CUHK-CSE/wider_face on HuggingFace (official WIDER FACE mirror)
License: CC BY-NC-ND 4.0 — the subset stays local (inside the git-ignored
         data/ tree) and is never redistributed with the repository.

Negatives (no-face scenes) come from fetch_negatives.py.

Usage  : python tools/vision/fetch_wider_subset.py [--count 40]
Output : tools/vision/data/wider_subset/images/positives/<event>/<name>.jpg
         tools/vision/data/wider_subset/labels.json
"""
from __future__ import annotations

import argparse
import json
import tempfile
import zipfile
from collections import Counter
from pathlib import Path

from PIL import Image

from http_util import download_file
from range_zip import RemoteZip

HF_BASE = "https://huggingface.co/datasets/CUHK-CSE/wider_face/resolve/main/data"
SPLIT_ZIP = f"{HF_BASE}/wider_face_split.zip"
VAL_ZIP = f"{HF_BASE}/WIDER_val.zip"

OUT_ROOT = Path(__file__).resolve().parent / "data" / "wider_subset"

MIN_FACE_PX = 40        # ignore tiny background faces (WIDER has many)
MAX_FACES_PER_IMAGE = 4 # keep scenes readable for a hub-camera use case
PER_EVENT = 2           # diversity cap inside one WIDER event directory


def parse_gt(text: str) -> list[dict]:
    lines = [ln.strip() for ln in text.splitlines() if ln.strip()]
    records: list[dict] = []
    i = 0
    while i < len(lines):
        name = lines[i]
        if not name.lower().endswith((".jpg", ".png")):
            i += 1
            continue
        count = int(lines[i + 1])
        faces, faces_all = [], []
        for j in range(count):
            vals = lines[i + 2 + j].split()
            x, y, w, h = (int(v) for v in vals[:4])
            invalid = int(vals[9]) if len(vals) > 9 else 0
            if invalid:
                continue
            faces_all.append([x, y, w, h])
            if w >= MIN_FACE_PX and h >= MIN_FACE_PX:
                faces.append([x, y, w, h])
        records.append({"name": name, "faces": faces, "faces_all": faces_all})
        i += 2 + count
    return records


def pick(records: list[dict], count: int) -> list[dict]:
    pool = [r for r in records
            if 1 <= len(r["faces"]) <= MAX_FACES_PER_IMAGE
            and max(max(f[2], f[3]) for f in r["faces"]) <= 2000]
    picked: list[dict] = []
    per_event: Counter[str] = Counter()
    for rec in pool:
        event = rec["name"].split("/")[0]
        if per_event[event] < PER_EVENT:
            per_event[event] += 1
            picked.append(rec)
        if len(picked) == count:
            break
    return picked


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--count", type=int, default=40)
    args = ap.parse_args()

    with tempfile.TemporaryDirectory() as tmp:
        split_zip = Path(tmp) / "wider_face_split.zip"
        download_file(SPLIT_ZIP, split_zip)
        with zipfile.ZipFile(split_zip) as zf:
            gt_name = next(n for n in zf.namelist() if n.endswith("wider_face_val_bbx_gt.txt"))
            records = parse_gt(zf.read(gt_name).decode("utf-8"))

    print(f"[info] val annotations: {len(records)} images")
    positives = pick(records, args.count)
    print(f"[info] picked {len(positives)} images with faces")

    remote = RemoteZip(VAL_ZIP)
    print(f"[info] remote val zip {remote.size / 1e6:.1f} MB, {len(remote.entries)} entries")

    images_dir = OUT_ROOT / "images" / "positives"
    labels: list[dict] = []
    for rec in positives:
        member = rec["name"]
        archive_member = f"WIDER_val/images/{member}"
        if archive_member not in remote.entries:
            print(f"[warn] missing in archive: {member}")
            continue
        dest = images_dir / member
        dest.parent.mkdir(parents=True, exist_ok=True)
        if not dest.exists():
            try:
                dest.write_bytes(remote.read(archive_member))
            except Exception as exc:  # noqa: BLE001 - flaky CDN, keep going
                print(f"[warn] download failed: {member} ({exc})")
                continue
        with Image.open(dest) as im:
            width, height = im.size
        labels.append({
            "file": f"positives/{member}",
            "width": width,
            "height": height,
            "faces": rec["faces"],
            "faces_all": rec["faces_all"],
        })
        print(f"[ok] {member} {width}x{height} faces={len(rec['faces'])}")

    OUT_ROOT.mkdir(parents=True, exist_ok=True)
    (OUT_ROOT / "labels.json").write_text(json.dumps({
        "source": "WIDER FACE validation split (CUHK-CSE/wider_face mirror)",
        "license": "CC BY-NC-ND 4.0; local test subset only, not redistributed",
        "selection": {
            "count": args.count,
            "min_face_px": MIN_FACE_PX,
            "max_faces_per_image": MAX_FACES_PER_IMAGE,
            "max_faces_per_event": PER_EVENT,
        },
        "images": labels,
    }, indent=2), encoding="utf-8")
    print(f"[done] {len(labels)} images → {OUT_ROOT}")


if __name__ == "__main__":
    main()
