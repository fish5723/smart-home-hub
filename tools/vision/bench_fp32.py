"""PC-side FP32 baseline: YuNet (OpenCV Zoo) on the WIDER FACE subset.

Runs the publicly available FP32 ONNX model with onnxruntime on the CPU and
reports, on the same test set that will be used for the on-device INT8 run:

  * accuracy   : precision / recall / F1 at IoU 0.5, plus FP on no-face images
  * latency    : median inference time per frame (warmed up, N repetitions)
  * memory     : process peak working set (peak_wset) before/after

Note: this is NOT the same network as esp-dl's two-stage MSR+MNP detector used
on the device (Espressif does not publish the float weights). The comparison is
task-level only — see docs/19.

Usage:
  python tools/vision/fetch_wider_subset.py
  python tools/vision/fetch_negatives.py
  python tools/vision/bench_fp32.py [--runs 20] [--score 0.6] [--json out.json]

Input : tools/vision/data/wider_subset/{labels,negatives}.json
Model : tools/vision/models/face_detection_yunet_2023mar.onnx
"""
from __future__ import annotations

import argparse
import json
import time
from pathlib import Path

import numpy as np
import onnxruntime as ort
import psutil
from PIL import Image

ROOT = Path(__file__).resolve().parent
DATA = ROOT / "data" / "wider_subset"
MODEL = ROOT / "models" / "face_detection_yunet_2023mar.onnx"

INPUT_W = INPUT_H = 640
STRIDES = (8, 16, 32)
IOU_MATCH = 0.5
IGNORE_IOU = 0.3


# ── preprocessing ────────────────────────────────────────────
def letterbox(bgr: np.ndarray, size: int = INPUT_W) -> tuple[np.ndarray, float, int, int]:
    """Keep aspect ratio, pad right/bottom with zeros (OpenCV YuNet layout)."""
    h, w = bgr.shape[:2]
    scale = min(size / w, size / h)
    nw, nh = int(round(w * scale)), int(round(h * scale))
    resized = np.asarray(Image.fromarray(bgr).resize((nw, nh), Image.BILINEAR))
    canvas = np.zeros((size, size, 3), dtype=np.uint8)
    canvas[:nh, :nw] = resized
    return canvas, scale, nw, nh


def to_tensor(bgr: np.ndarray) -> np.ndarray:
    chw = bgr.transpose(2, 0, 1).astype(np.float32)  # BGR, raw 0..255
    return np.ascontiguousarray(chw[None])


# ── postprocessing ───────────────────────────────────────────
def decode(outputs: dict[str, np.ndarray]) -> tuple[np.ndarray, np.ndarray]:
    boxes, scores = [], []
    for stride in STRIDES:
        cls = outputs[f"cls_{stride}"][0].reshape(-1)
        obj = outputs[f"obj_{stride}"][0].reshape(-1)
        bbox = outputs[f"bbox_{stride}"][0].reshape(-1, 4)
        score = np.sqrt(np.clip(cls, 0, None) * np.clip(obj, 0, None))

        cols = INPUT_W // stride
        idx = np.arange(cls.shape[0])
        row, col = idx // cols, idx % cols

        cx = (col + bbox[:, 0]) * stride
        cy = (row + bbox[:, 1]) * stride
        w = np.exp(bbox[:, 2]) * stride
        h = np.exp(bbox[:, 3]) * stride

        boxes.append(np.stack([cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2], axis=1))
        scores.append(score)
    return np.concatenate(boxes), np.concatenate(scores)


def nms(boxes: np.ndarray, scores: np.ndarray, iou_thr: float) -> np.ndarray:
    order = scores.argsort()[::-1]
    keep = []
    while order.size:
        i = order[0]
        keep.append(i)
        if order.size == 1:
            break
        rest = order[1:]
        ious = iou(boxes[i], boxes[rest])
        order = rest[ious <= iou_thr]
    return np.asarray(keep, dtype=np.int64)


def iou(box: np.ndarray, boxes: np.ndarray) -> np.ndarray:
    x1 = np.maximum(box[0], boxes[:, 0])
    y1 = np.maximum(box[1], boxes[:, 1])
    x2 = np.minimum(box[2], boxes[:, 2])
    y2 = np.minimum(box[3], boxes[:, 3])
    inter = np.clip(x2 - x1, 0, None) * np.clip(y2 - y1, 0, None)
    area_a = (box[2] - box[0]) * (box[3] - box[1])
    area_b = (boxes[:, 2] - boxes[:, 0]) * (boxes[:, 3] - boxes[:, 1])
    return inter / np.clip(area_a + area_b - inter, 1e-9, None)


# ── model ───────────────────────────────────────────────────
def build_session(threads: int) -> ort.InferenceSession:
    opts = ort.SessionOptions()
    opts.graph_optimization_level = ort.GraphOptimizationLevel.ORT_ENABLE_ALL
    opts.intra_op_num_threads = threads
    opts.inter_op_num_threads = 1
    return ort.InferenceSession(str(MODEL), sess_options=opts, providers=["CPUExecutionProvider"])


def detect(sess, image: np.ndarray, score_thr: float, nms_thr: float) -> tuple[np.ndarray, np.ndarray]:
    canvas, scale, _, _ = letterbox(image)
    tensor = to_tensor(canvas)
    outputs = dict(zip([o.name for o in sess.get_outputs()], sess.run(None, {"input": tensor})))
    boxes, scores = decode(outputs)

    mask = scores >= score_thr
    boxes, scores = boxes[mask], scores[mask]
    if boxes.size:
        keep = nms(boxes, scores, nms_thr)
        boxes, scores = boxes[keep], scores[keep]
    boxes = boxes / scale  # back to original image coordinates
    return boxes, scores


# ── evaluation ──────────────────────────────────────────────
def load_manifest(path: Path) -> list[dict]:
    if not path.exists():
        return []
    return json.loads(path.read_text(encoding="utf-8")).get("images", [])


def match(boxes: np.ndarray, scores: np.ndarray, gts: np.ndarray,
          ignore_gts: np.ndarray) -> tuple[int, int, int, int]:
    """Greedy match by score.

    Returns (tp, fp, fn, ignored). Detections that only overlap faces which were
    filtered out of the reference set (smaller than MIN_FACE_PX) count as
    ignored, not as false positives — WIDER annotates tiny background faces
    that were deliberately excluded from the recall reference.
    """
    matched = set()
    tp = 0
    leftovers: list[int] = []
    for i in scores.argsort()[::-1]:
        best, best_iou = -1, IOU_MATCH
        for j, gt in enumerate(gts):
            if j in matched:
                continue
            v = iou(boxes[i], gt[None])[0]
            if v >= best_iou:
                best, best_iou = j, v
        if best >= 0:
            matched.add(best)
            tp += 1
        else:
            leftovers.append(i)

    ignored = 0
    for i in leftovers:
        if ignore_gts.size and iou(boxes[i], ignore_gts).max() >= IGNORE_IOU:
            ignored += 1
    fp = len(leftovers) - ignored
    return tp, fp, len(gts) - tp, ignored


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--runs", type=int, default=20)
    ap.add_argument("--score", type=float, default=0.6)
    ap.add_argument("--nms", type=float, default=0.3)
    ap.add_argument("--threads", type=int, default=4)
    ap.add_argument("--json", type=Path, default=DATA.parent / "results" / "fp32_yunet.json")
    args = ap.parse_args()

    positives = load_manifest(DATA / "labels.json")
    negatives = load_manifest(DATA / "negatives.json")
    if not positives:
        raise SystemExit("no positives — run fetch_wider_subset.py first")

    proc = psutil.Process()
    peak_before = getattr(proc.memory_info(), "peak_wset", proc.memory_info().rss)

    sess = build_session(args.threads)
    t_load = time.perf_counter()

    tp = fp = fn = fp_neg = ignored = 0
    det_total = 0
    latencies: list[float] = []

    for rec in positives:
        image = np.asarray(Image.open(DATA / "images" / rec["file"]).convert("RGB"))[:, :, ::-1]
        boxes, scores = detect(sess, np.ascontiguousarray(image), args.score, args.nms)
        gts = np.asarray(rec["faces"], dtype=np.float32)
        gts[:, 2:] += gts[:, :2]
        all_gts = np.asarray(rec.get("faces_all") or rec["faces"], dtype=np.float32)
        all_gts[:, 2:] += all_gts[:, :2]
        a, b, c, d = match(boxes, scores, gts, all_gts)
        tp, fp, fn, ignored = tp + a, fp + b, fn + c, ignored + d
        det_total += len(boxes)

    for rec in negatives:
        image = np.asarray(Image.open(DATA / "images" / rec["file"]).convert("RGB"))[:, :, ::-1]
        boxes, _ = detect(sess, np.ascontiguousarray(image), args.score, args.nms)
        fp_neg += len(boxes)
        det_total += len(boxes)

    # latency: same frame repeated, warmed up
    sample = positives[0]
    image = np.asarray(Image.open(DATA / "images" / sample["file"]).convert("RGB"))[:, :, ::-1]
    image = np.ascontiguousarray(image)
    canvas, _, _, _ = letterbox(image)
    tensor = to_tensor(canvas)
    feed = {"input": tensor}
    for _ in range(3):
        sess.run(None, feed)
    for _ in range(args.runs):
        t0 = time.perf_counter()
        sess.run(None, feed)
        latencies.append((time.perf_counter() - t0) * 1000)

    info = proc.memory_info()
    peak_after = getattr(info, "peak_wset", info.rss)

    total_gt = sum(len(r["faces"]) for r in positives)
    precision = tp / max(tp + fp + fp_neg, 1)
    recall = tp / max(total_gt, 1)
    f1 = 2 * precision * recall / max(precision + recall, 1e-9)

    result = {
        "model": MODEL.name,
        "model_bytes": MODEL.stat().st_size,
        "runtime": f"onnxruntime {ort.__version__} CPUExecutionProvider, intra_op={args.threads}",
        "input": f"{INPUT_W}x{INPUT_H} letterbox BGR fp32",
        "test_set": {
            "positives": len(positives),
            "negatives": len(negatives),
            "gt_faces": total_gt,
        },
        "params": {"score_threshold": args.score, "nms_iou": args.nms, "match_iou": IOU_MATCH},
        "accuracy": {
            "tp": tp,
            "fn": fn,
            "fp": fp,
            "fp_negatives": fp_neg,
            "ignored_small_faces": ignored,
            "precision": round(precision, 4),
            "recall": round(recall, 4),
            "f1": round(f1, 4),
            "detections": det_total,
        },
        "latency_ms": {
            "runs": args.runs,
            "median": round(sorted(latencies)[len(latencies) // 2], 2),
            "mean": round(sum(latencies) / len(latencies), 2),
            "min": round(min(latencies), 2),
            "max": round(max(latencies), 2),
        },
        "memory": {
            "peak_wset_before_mb": round(peak_before / 1e6, 1),
            "peak_wset_after_mb": round(peak_after / 1e6, 1),
            "delta_mb": round((peak_after - peak_before) / 1e6, 1),
            "note": "process peak working set (includes interpreter + model load)",
        },
        "session_load_ms": round((time.perf_counter() - t_load) * 1000, 1),
    }

    args.json.parent.mkdir(parents=True, exist_ok=True)
    args.json.write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps(result, indent=2))
    print(f"[done] → {args.json}")


if __name__ == "__main__":
    main()
