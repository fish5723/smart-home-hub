"""Download the YuNet face detector (FP32 ONNX) used as the PC baseline.

Source : OpenCV Zoo, model face_detection_yunet_2023mar (Apache-2.0)
Usage  : python tools/vision/fetch_yunet.py
Output : tools/vision/models/face_detection_yunet_2023mar.onnx
"""
from pathlib import Path

from http_util import download_file

URL = ("https://github.com/opencv/opencv_zoo/raw/main/"
       "models/face_detection_yunet/face_detection_yunet_2023mar.onnx")
# Official git-lfs pointer for this file (from opencv_zoo, LFS oid sha256).
SHA256 = "8f2383e4dd3cfbb4553ea8718107fc0423210dc964f9f4280604804ed2552fa4"

MODEL = Path(__file__).resolve().parent / "models" / "face_detection_yunet_2023mar.onnx"


def main() -> None:
    download_file(URL, MODEL, sha256=SHA256)


if __name__ == "__main__":
    main()
