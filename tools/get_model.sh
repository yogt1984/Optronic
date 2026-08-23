#!/usr/bin/env bash
# Fetch the detection model. Not in the repository: 24 MB of weights that
# change independently of this code and carry their own licence.
#
# Two models, because they exercise different paths:
#
#   yolov5s.onnx      the default. More accurate, and its raw output has to be
#                     decoded and NMS-ed by hand - OpenCV's DetectionModel
#                     cannot parse it.
#   yolov4-tiny       darknet format, read straight through DetectionModel
#                     with NMS included. Faster, kept as the low-latency
#                     option and as proof the detector is not tied to one
#                     export format.
#
# Newer than v5 exists - v8 and v11 - but their output tensor is transposed
# and OpenCV 4.6, which Ubuntu 24.04 packages, does not read them cleanly.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../models"

base=https://raw.githubusercontent.com/AlexeyAB/darknet/master
weights=https://github.com/AlexeyAB/darknet/releases/download/darknet_yolo_v4_pre/yolov4-tiny.weights
# Not the ultralytics v7.0 asset: that one is exported FP16, and OpenCV 4.6
# refuses it with "Unsupported data type: FLOAT16". This is the same network
# exported FP32.
onnx=https://github.com/doleron/yolov5-opencv-cpp-python/raw/main/config_files/yolov5s.onnx

get() {
  [[ -s $2 ]] && { echo "  have $2"; return; }
  echo "  fetching $2"
  curl -fsSL -o "$2" "$1"
}

get "$base/cfg/yolov4-tiny.cfg"   yolov4-tiny.cfg
get "$base/data/coco.names"       coco.names
get "$weights"                    yolov4-tiny.weights
get "$onnx"                       yolov5s.onnx

printf '\n'
ls -l yolov4-tiny.cfg coco.names yolov4-tiny.weights yolov5s.onnx |
  awk '{printf "  %-24s %s bytes\n", $9, $5}'
echo
echo "ready - --detect uses yolov5s.onnx; --model models/yolov4-tiny.cfg for the fast one"
