#!/usr/bin/env bash
# Fetch the detection model. Not in the repository: 24 MB of weights that
# change independently of this code and carry their own licence.
#
# YOLOv4-tiny in darknet format rather than a newer ONNX export, because
# OpenCV 4.6 - what Ubuntu 24.04 packages - reads it directly through
# cv::dnn::DetectionModel, non-maximum suppression included. A YOLOv8 ONNX
# needs post-processing this does not have to carry.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../models"

base=https://raw.githubusercontent.com/AlexeyAB/darknet/master
weights=https://github.com/AlexeyAB/darknet/releases/download/darknet_yolo_v4_pre/yolov4-tiny.weights

get() {
  [[ -s $2 ]] && { echo "  have $2"; return; }
  echo "  fetching $2"
  curl -fsSL -o "$2" "$1"
}

get "$base/cfg/yolov4-tiny.cfg"   yolov4-tiny.cfg
get "$base/data/coco.names"       coco.names
get "$weights"                    yolov4-tiny.weights

printf '\n'
ls -l yolov4-tiny.cfg coco.names yolov4-tiny.weights | awk '{printf "  %-24s %s bytes\n", $9, $5}'
echo
echo "ready - run the service with --detect"
