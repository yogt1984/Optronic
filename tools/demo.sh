#!/usr/bin/env bash
# Live demo driver. Everything runs inside the build container, so it behaves
# the same on any machine with Docker and needs nothing installed on the host.
#
# The build directories it creates are named build-container-* because they are
# configured inside the container: CMake records absolute paths, and /work does
# not exist on the host, so ctest from outside cannot read them. Use
# tools/test.sh, which keeps container and native builds apart.
#
#   tools/demo.sh warm      once, before the meeting: build so nothing compiles live
#   tools/demo.sh tests     the suite, host and aarch64            (~30 s)
#   tools/demo.sh service   the service, MQTT, and a broker outage (~25 s)
#   tools/demo.sh nuc       the NUC shutter sequence, live         (~10 s)
#   tools/demo.sh qemu      the same binary on aarch64             (~15 s)
#   tools/demo.sh live      runs until Ctrl-C: video window + live MQTT
#   tools/demo.sh detect    camera + YOLO, boxes drawn in the frame path
#
# `live` needs a decoder on this machine:
#   sudo apt install gstreamer1.0-libav gstreamer1.0-plugins-bad
#
# Each act stops at the end. Run them one at a time and talk in between.
set -euo pipefail

IMAGE=${OPTRONIC_IMAGE:-optronic/debian}
# The detector needs OpenCV, which only the :cv image carries.
DETECT_IMAGE=${OPTRONIC_DETECT_IMAGE:-optronic/debian:cv}
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

run() {
  docker run --rm --security-opt seccomp=unconfined \
    -v "$HERE:/work" -v optronic-ccache:/ccache \
    --entrypoint bash "$IMAGE" -c "$1"
}

rule() { printf '\n\033[1m── %s ──\033[0m\n\n' "$1"; }

case "${1:-help}" in

warm)
  rule "Building both targets so the demo compiles nothing"
  run 'cd /work &&
       cmake --preset host-debug -B build-container-demo >/dev/null &&
       cmake --build build-container-demo -j"$(nproc)" >/dev/null &&
       cmake --preset arm64-debian -B build-container-arm64 >/dev/null &&
       cmake --build build-container-arm64 -j"$(nproc)" >/dev/null &&
       echo "warming the aarch64 GStreamer registry - 20 s now instead of live" &&
       GST_REGISTRY_FORK=no GST_REGISTRY=/work/.gst-registry-arm64.bin timeout 120 qemu-aarch64-static -L /opt/sysroot-arm64 ./build-container-arm64/optronic --width 320 --height 240 --seconds 1 >/dev/null 2>&1 &&
       echo "ready: build-container-demo (x86) and build-container-arm64 (aarch64)"'
  ;;

tests)
  rule "The suite on this machine"
  run 'cd /work && ctest --test-dir build-container-demo --output-on-failure 2>&1 | tail -4'
  rule "The same code, cross-compiled, on aarch64 under emulation"
  run 'cd /work && file build-container-arm64/optronic | cut -d, -f1-2 &&
       tools/qemu/run.sh build-container-arm64 2>&1 | grep -E "^  ok|^  FAIL|aarch64:"'
  ;;

service)
  rule "The service: power-on BIT, pipeline, telemetry - and a broker that dies"
  run 'cd /work
       mosquitto -d -p 1883 2>/dev/null; sleep 1
       ( mosquitto_sub -v -t "optronic/#" | sed -u "s/^/  [monitor] /" ) &
       sub=$!
       ./build-container-demo/optronic --broker 127.0.0.1 --node sight-01 --seconds 11 2>&1 |
         sed -u "s/^/  [service] /" &
       svc=$!
       sleep 4; echo; echo "  >>> killing the broker"; echo
       pkill mosquitto || true
       sleep 4; echo; echo "  >>> bringing it back"; echo
       mosquitto -d -p 1883 2>/dev/null || true
       wait $svc; sleep 1; kill $sub 2>/dev/null || true
       pkill mosquitto || true'
  ;;

nuc)
  rule "Non-uniformity correction: the channel goes blind for 200 ms"
  run 'cd /work
       mosquitto -d -p 1883 2>/dev/null; sleep 1
       ( mosquitto_sub -v -t "optronic/+/sensor" -t "optronic/+/event" | sed -u "s/^/  [monitor] /" ) &
       sub=$!
       ./build-container-demo/optronic --broker 127.0.0.1 --node sight-01 --nuc 3 --seconds 6 2>&1 |
         grep --line-buffered -E "NUC|frame seq|PLAYING" | sed -u "s/^/  [service] /"
       sleep 1; kill $sub 2>/dev/null || true; pkill mosquitto || true'
  ;;

qemu)
  rule "The aarch64 binary, emulated, with the full pipeline and telemetry"
  run 'cd /work
       mosquitto -d -p 1883 2>/dev/null; sleep 1
       ( mosquitto_sub -v -t "optronic/+/sensor" | sed -u "s/^/  [monitor] /" ) &
       sub=$!
       GST_REGISTRY_FORK=no GST_REGISTRY=/work/.gst-registry-arm64.bin \
       qemu-aarch64-static -L /opt/sysroot-arm64 \
         ./build-container-arm64/optronic --width 320 --height 240 \
         --broker 127.0.0.1 --node sight-arm64 --seconds 6 2>&1 |
         sed -u "s/^/  [aarch64] /"
       sleep 1; kill $sub 2>/dev/null || true; pkill mosquitto || true'
  ;;

detect)
  for f in models/yolov4-tiny.weights models/coco.names; do
    [[ -s $HERE/$f ]] || { echo "missing $f - run tools/get_model.sh" >&2; exit 1; }
  done
  [[ -e /dev/video0 ]] || { echo "no /dev/video0" >&2; exit 1; }
  gst-inspect-1.0 avdec_h264 >/dev/null 2>&1 ||
    { echo "sudo apt install gstreamer1.0-libav gstreamer1.0-plugins-bad" >&2; exit 1; }

  rule "Camera through the detector: boxes drawn inside the frame path"
  echo "  point the camera at yourself, a chair, a cup - COCO classes."
  echo "  Ctrl-C to stop."
  echo

  # --group-add: the device is owned by the host's video group, and the
  # container's user is not in it.
  cid=$(docker run -d --rm --network host --security-opt seccomp=unconfined \
          --device /dev/video0 --group-add "$(stat -c '%g' /dev/video0)" \
          -v "$HERE:/work" --entrypoint bash "$DETECT_IMAGE" -c '
            mosquitto -d -p 1883 2>/dev/null; sleep 1
            exec /work/build-cv/optronic --camera --width 640 --height 480 \
              --detect --stream --host 127.0.0.1 --port 5600 \
              --broker 127.0.0.1 --node sight-01')

  gst-launch-1.0 -q udpsrc port=5600 caps="application/x-rtp,media=video,encoding-name=H264,payload=96" \
    ! rtpjitterbuffer latency=50 ! rtph264depay ! h264parse ! avdec_h264 \
    ! videoconvert ! autovideosink sync=false >/dev/null 2>&1 &
  viewer=$!
  cleanup() { kill "$viewer" 2>/dev/null || true; docker kill "$cid" >/dev/null 2>&1 || true; echo; echo "stopped."; }
  trap cleanup EXIT INT TERM

  sleep 2
  docker logs -f "$cid" 2>&1 | grep --line-buffered -E 'detect|NUC|PLAYING' | sed -u 's/^/  /'
  ;;

live)
  # Everything on the host network so the RTP stream and the broker are
  # reachable from outside the container without port bookkeeping.
  for e in avdec_h264 h264parse; do
    if ! gst-inspect-1.0 "$e" >/dev/null 2>&1; then
      echo "missing GStreamer element '$e' on this machine." >&2
      echo "  sudo apt install gstreamer1.0-libav gstreamer1.0-plugins-bad" >&2
      exit 1
    fi
  done

  rule "Live: the service streams H.264 and publishes telemetry until Ctrl-C"

  cid=$(docker run -d --rm --network host --security-opt seccomp=unconfined \
          -v "$HERE:/work" --entrypoint bash "$IMAGE" -c '
            mosquitto -d -p 1883 2>/dev/null; sleep 1
            exec /work/build-container-demo/optronic \
              --stream --host 127.0.0.1 --port 5600 \
              --broker 127.0.0.1 --node sight-01 --nuc 20 --debug')

  # The window is opened by the host, which already has a display; the
  # container only produces the stream.
  gst-launch-1.0 -q udpsrc port=5600 caps="application/x-rtp,media=video,encoding-name=H264,payload=96" \
    ! rtpjitterbuffer latency=50 ! rtph264depay ! h264parse ! avdec_h264 \
    ! videoconvert ! autovideosink sync=false >/dev/null 2>&1 &
  viewer=$!

  cleanup() {
    kill "$viewer" 2>/dev/null || true
    docker kill "$cid" >/dev/null 2>&1 || true
    echo; echo "stopped."
  }
  trap cleanup EXIT INT TERM

  echo "  video window opening; telemetry below. Ctrl-C to stop."
  echo "  (a NUC runs 20 s in - watch the event topic)"
  echo
  sleep 2
  docker run --rm --network host --entrypoint mosquitto_sub "$IMAGE" \
    -v -t 'optronic/#' | sed -u 's/^/  /'
  ;;

*)
  sed -n '2,16p' "${BASH_SOURCE[0]}" | sed 's/^# \?//'
  ;;
esac
