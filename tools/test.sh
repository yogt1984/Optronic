#!/usr/bin/env bash
# Run the test suite.
#
#   tools/test.sh                    all of it, in the build container
#   tools/test.sh SpscRing           only tests matching a regex
#   tools/test.sh --native           on this machine instead of in the container
#   tools/test.sh --native Nuc
#   tools/test.sh --preset host-asan build and run under a different preset
#
# The container is the reference environment: it carries the GStreamer and
# mosquitto development files, so it runs the whole suite. A bare host runs
# everything that does not need them - which is most of it - and reports the
# rest as absent rather than failing.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

IMAGE=${OPTRONIC_IMAGE:-optronic/debian}
native=0
preset=host-debug
filter=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --native) native=1; shift ;;
    --preset) preset=${2:?--preset needs a value}; shift 2 ;;
    -h|--help) sed -n '2,14p' "${BASH_SOURCE[0]}" | sed 's/^# \?//'; exit 0 ;;
    -*) echo "unknown option: $1" >&2; exit 2 ;;
    *) filter=$1; shift ;;
  esac
done

# A build directory remembers the absolute path it was configured with, and
# that path differs inside and outside the container (/work vs the checkout).
# Separate directories rather than one shared one, so neither run has to
# reconfigure the other's.
if [[ $native -eq 1 ]]; then
  build=build-native-$preset
  cmake --preset "$preset" -B "$build" >/dev/null
  cmake --build "$build" -j"$(nproc)" >/dev/null
  exec ctest --test-dir "$build" --output-on-failure ${filter:+-R "$filter"}
fi

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
  echo "image $IMAGE not found - build it with:" >&2
  echo "  docker build -f docker/Dockerfile.debian -t $IMAGE ." >&2
  echo "or run the suite on this machine with: tools/test.sh --native" >&2
  exit 1
fi

# seccomp=unconfined: ThreadSanitizer needs setarch -R, which needs the
# personality syscall that Docker's default profile blocks.
exec docker run --rm --security-opt seccomp=unconfined \
  -v "$PWD:/work" -v optronic-ccache:/ccache \
  --entrypoint bash "$IMAGE" -c "
    set -e
    cd /work
    cmake --preset $preset -B build-container-$preset >/dev/null
    cmake --build build-container-$preset -j\$(nproc) >/dev/null
    ctest --test-dir build-container-$preset --output-on-failure ${filter:+-R '$filter'}
  "
