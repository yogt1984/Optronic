#!/usr/bin/env bash
# Live demo driver. Everything runs inside the build container, so it behaves
# the same on any machine with Docker and needs nothing installed on the host.
#
#   tools/demo.sh warm      once, before the meeting: build so nothing compiles live
#   tools/demo.sh tests     the suite, host and aarch64            (~30 s)
#   tools/demo.sh service   the service, MQTT, and a broker outage (~25 s)
#   tools/demo.sh qemu      the same binary on aarch64             (~40 s)
#
# Each act stops at the end. Run them one at a time and talk in between.
set -euo pipefail

IMAGE=${OPTRONIC_IMAGE:-optronic/debian}
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
       cmake --preset host-debug -B build-demo >/dev/null &&
       cmake --build build-demo -j"$(nproc)" >/dev/null &&
       cmake --preset arm64-debian -B build-demo-arm64 >/dev/null &&
       cmake --build build-demo-arm64 -j"$(nproc)" >/dev/null &&
       echo "warming the aarch64 GStreamer registry - 20 s now instead of live" &&
       GST_REGISTRY_FORK=no GST_REGISTRY=/work/.gst-registry-arm64.bin timeout 120 qemu-aarch64-static -L /opt/sysroot-arm64 ./build-demo-arm64/optronic --width 320 --height 240 --seconds 1 >/dev/null 2>&1 &&
       echo "ready: build-demo (x86) and build-demo-arm64 (aarch64)"'
  ;;

tests)
  rule "The suite on this machine"
  run 'cd /work && ctest --test-dir build-demo --output-on-failure 2>&1 | tail -4'
  rule "The same code, cross-compiled, on aarch64 under emulation"
  run 'cd /work && file build-demo-arm64/optronic | cut -d, -f1-2 &&
       tools/qemu/run.sh build-demo-arm64 2>&1 | grep -E "^  ok|^  FAIL|aarch64:"'
  ;;

service)
  rule "The service: power-on BIT, pipeline, telemetry - and a broker that dies"
  run 'cd /work
       mosquitto -d -p 1883 2>/dev/null; sleep 1
       ( mosquitto_sub -v -t "optronic/#" | sed -u "s/^/  [monitor] /" ) &
       sub=$!
       ./build-demo/optronic --broker 127.0.0.1 --node sight-01 --seconds 11 2>&1 |
         sed -u "s/^/  [service] /" &
       svc=$!
       sleep 4; echo; echo "  >>> killing the broker"; echo
       pkill mosquitto || true
       sleep 4; echo; echo "  >>> bringing it back"; echo
       mosquitto -d -p 1883 2>/dev/null || true
       wait $svc; sleep 1; kill $sub 2>/dev/null || true
       pkill mosquitto || true'
  ;;

qemu)
  rule "The aarch64 binary, emulated, with the full pipeline and telemetry"
  run 'cd /work
       mosquitto -d -p 1883 2>/dev/null; sleep 1
       ( mosquitto_sub -v -t "optronic/+/sensor" | sed -u "s/^/  [monitor] /" ) &
       sub=$!
       GST_REGISTRY_FORK=no GST_REGISTRY=/work/.gst-registry-arm64.bin \
       qemu-aarch64-static -L /opt/sysroot-arm64 \
         ./build-demo-arm64/optronic --width 320 --height 240 \
         --broker 127.0.0.1 --node sight-arm64 --seconds 6 2>&1 |
         sed -u "s/^/  [aarch64] /"
       sleep 1; kill $sub 2>/dev/null || true; pkill mosquitto || true'
  ;;

*)
  sed -n '2,12p' "${BASH_SOURCE[0]}" | sed 's/^# \?//'
  ;;
esac
