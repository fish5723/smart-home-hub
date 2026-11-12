"""P4 on-device batch log -> box-level metrics on the same WIDER subset.

Input: a serial log containing the firmware's machine-readable lines

    FACE_BATCH: image label=pos path=positives/... w=... h=... count=N best=...
                box=x,y,w,h boxes=x,y,w,h,score;... det_us=... dec_us=...

The matching rules mirror bench_fp32.py: greedy match by score at IoU 0.5;
detections that only overlap WIDER faces excluded from the reference set
(faces_all, IoU >= 0.3) are "ignored" rather than false positives.

Usage:
    python tools/vision/bench_p4_log.py path/to/serial.log
"""

import argparse
import json
import re
from pathlib import Path

import numpy as np

DATA = Path(__file__).resolve().parent / "data" / "wider_subset"
IOU_MATCH = 0.5
IGNORE_IOU = 0.3

LINE = re.compile(
    r"FACE_BATCH: image label=(\w+) path=(\S+) w=(\d+) h=(\d+) "
    r"count=(\d+) best=([\d.]+) box=(-?\d+),(-?\d+),(-?\d+),(-?\d+) "
    r"(?:boxes=(\S*) )?det_us=(\d+) dec_us=(\d+)"
)


def iou(box: np.ndarray, boxes: np.ndarray) -> np.ndarray:
    x1 = np.maximum(box[0], boxes[:, 0])
    y1 = np.maximum(box[1], boxes[:, 1])
    x2 = np.minimum(box[2], boxes[:, 2])
    y2 = np.minimum(box[3], boxes[:, 3])
    inter = np.clip(x2 - x1, 0, None) * np.clip(y2 - y1, 0, None)
    area_a = (box[2] - box[0]) * (box[3] - box[1])
    area_b = (boxes[:, 2] - boxes[:, 0]) * (boxes[:, 3] - boxes[:, 1])
    return inter / np.maximum(area_a + area_b - inter, 1e-9)


def parse_boxes(text: str, fallback) -> np.ndarray:
    """'x,y,w,h,score;...' -> [[x1,y1,x2,y2,score], ...]."""
    boxes = []
    for item in (text or "").split(";"):
        if not item:
            continue
        x, y, w, h, score = item.split(",")
        boxes.append([int(x), int(y), int(x) + int(w), int(y) + int(h), float(score)])
    if not boxes and fallback is not None:
        x, y, w, h = fallback
        boxes.append([x, y, x + w, y + h, 0.0])
    return np.asarray(boxes, dtype=np.float64).reshape(-1, 5)


def match(boxes: np.ndarray, gts: np.ndarray, ignore_gts: np.ndarray):
    """Greedy match by score. Returns (tp, fp, fn, ignored)."""
    order = np.argsort(boxes[:, 4])[::-1] if len(boxes) else np.array([], dtype=int)
    matched = set()
    tp = 0
    leftovers = []
    for i in order:
        best, best_iou = -1, IOU_MATCH
        for j, gt in enumerate(gts):
            if j in matched:
                continue
            v = iou(boxes[i, :4], gt[None])[0]
            if v >= best_iou:
                best, best_iou = j, v
        if best >= 0:
            matched.add(best)
            tp += 1
        else:
            leftovers.append(i)

    ignored = 0
    for i in leftovers:
        if ignore_gts.size and iou(boxes[i, :4], ignore_gts).max() >= IGNORE_IOU:
            ignored += 1
    fp = len(leftovers) - ignored
    return tp, fp, len(gts) - tp, ignored


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("log", type=Path)
    ap.add_argument("--score", type=float, default=0.0,
                    help="drop logged boxes below this score (default: keep all)")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    labels = json.loads((DATA / "labels.json").read_text(encoding="utf-8"))["images"]
    negatives = json.loads((DATA / "negatives.json").read_text(encoding="utf-8"))["images"]

    records = {}
    det_ms = []
    for line in args.log.read_text(encoding="utf-8", errors="ignore").splitlines():
        m = LINE.search(line)
        if not m:
            continue
        (label, path, _w, _h, count, _best, x, y, bw, bh,
         boxes_text, det_us, _dec_us) = m.groups()
        fallback = (int(x), int(y), int(bw), int(bh)) if int(count) > 0 else None
        boxes = parse_boxes(boxes_text, fallback)
        if args.score > 0.0 and len(boxes):
            boxes = boxes[boxes[:, 4] >= args.score]
        records[path] = (label, boxes, int(det_us))
        det_ms.append(int(det_us) / 1000.0)

    tp = fp = fn = ignored = 0
    missed = []
    for rec in labels:
        path = rec["file"]
        if path not in records:
            missed.append(path)
            continue
        _, boxes, _ = records[path]
        gts = np.asarray(rec["faces"], dtype=np.float64)
        gts[:, 2:] += gts[:, :2]
        all_gts = np.asarray(rec.get("faces_all") or rec["faces"], dtype=np.float64)
        all_gts[:, 2:] += all_gts[:, :2]
        a, b, c, d = match(boxes, gts, all_gts)
        tp, fp, fn, ignored = tp + a, fp + b, fn + c, ignored + d
        if args.verbose and c:
            print(f"  miss {c:2d} face(s): {path}")

    fp_neg = 0
    for rec in negatives:
        path = rec["file"]
        if path in records:
            fp_neg += len(records[path][1])

    precision = tp / (tp + fp) if (tp + fp) else 0.0
    recall = tp / (tp + fn) if (tp + fn) else 0.0
    f1 = 2 * precision * recall / (precision + recall) if (precision + recall) else 0.0

    print(f"images                : {len(records)} "
          f"(pos {len(labels) - len(missed)}/{len(labels)}, neg {len(negatives)})")
    print(f"TP / FP / FN / ignored: {tp} / {fp} / {fn} / {ignored}")
    print(f"precision / recall / F1: {precision:.3f} / {recall:.3f} / {f1:.3f}")
    print(f"negative images FP    : {fp_neg} boxes on {len(negatives)} images")
    if det_ms:
        print(f"detect time           : avg {np.mean(det_ms):.1f} ms, "
              f"min {np.min(det_ms):.1f}, max {np.max(det_ms):.1f} (n={len(det_ms)})")
        if len(det_ms) > 1:
            rest = det_ms[1:]
            print(f"detect time (excl #1) : avg {np.mean(rest):.1f} ms, "
                  f"min {np.min(rest):.1f}, max {np.max(rest):.1f} (n={len(rest)})")
    if missed:
        print(f"missing images        : {len(missed)}")
        for p in missed:
            print(f"  {p}")


if __name__ == "__main__":
    main()
