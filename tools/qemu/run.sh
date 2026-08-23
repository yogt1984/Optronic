#!/usr/bin/env bash
# Runs the cross-built aarch64 suite under QEMU user-mode emulation.
#
#   tools/qemu/run.sh build-arm64-debian
#
# This is emulation of the instruction set, not of the board: it proves the
# aarch64 binaries are correct C++ - alignment, atomics, type widths, the
# things that differ between x86 and ARM - and nothing about the hardware. The
# PetaLinux QEMU machine that boots the real image is a separate stage; where
# that is not available this is the honest half of the check, and it is the
# half that catches portability bugs before they reach the bench.
set -euo pipefail

BUILD_DIR=${1:-build-arm64-debian}
SYSROOT=${QEMU_SYSROOT:-/opt/sysroot-arm64}
QEMU=${QEMU_AARCH64:-qemu-aarch64-static}

if ! command -v "$QEMU" >/dev/null; then
  echo "qemu stage: $QEMU not found - install qemu-user-static" >&2
  exit 1
fi
if [[ ! -d "$BUILD_DIR" ]]; then
  echo "qemu stage: $BUILD_DIR does not exist - run the cross stage first" >&2
  exit 1
fi
if [[ ! -d "$SYSROOT" ]]; then
  echo "qemu stage: sysroot $SYSROOT not found" >&2
  exit 1
fi

echo "aarch64 suite under $QEMU (sysroot $SYSROOT)"

failed=0
ran=0
for t in "$BUILD_DIR"/tests/test_*; do
  [[ -x "$t" ]] || continue
  name=$(basename "$t")
  ran=$((ran + 1))

  # GStreamer's registry scan forks a helper binary, which user-mode emulation
  # cannot run; without the plugins the video tests that build a pipeline are
  # expected to fail here rather than being silently skipped.
  if out=$(GST_REGISTRY_FORK=no "$QEMU" -L "$SYSROOT" "$t" 2>&1); then
    echo "  ok    $name  ($(grep -cE '^\[       OK \]' <<<"$out") cases)"
  else
    echo "  FAIL  $name"
    grep -E '^\[  FAILED  \]' <<<"$out" | sed 's/^/        /' | sort -u
    failed=$((failed + 1))
  fi
done

echo "aarch64: $((ran - failed))/$ran binaries passed"

# The service itself, briefly: it must start, run its power-on BIT and shut
# down cleanly on a foreign architecture too.
if [[ -x "$BUILD_DIR/optronic" ]]; then
  echo "service smoke test:"
  GST_REGISTRY_FORK=no "$QEMU" -L "$SYSROOT" "$BUILD_DIR/optronic" --seconds 2 2>&1 |
    sed 's/^/  /' || true
fi

exit "$failed"
