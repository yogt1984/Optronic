#!/usr/bin/env bash
# Container entry point: runs the CI stages against the mounted source tree.
#
#   STAGES   comma-separated subset of: lint,host,cross,qemu   (default: lint,host,cross)
#   HOST_PRESETS  presets for the host stage (default: host-debug,host-asan,host-tsan)
#   CROSS_PRESET  preset for the cross stage (default: arm64-petalinux if an SDK
#                 environment is present, otherwise arm64-debian)
#
# Any other arguments are executed instead of the stages (e.g. a shell).

set -euo pipefail

if [[ $# -gt 0 ]]; then
  exec "$@"
fi

STAGES=${STAGES:-lint,host,cross}
HOST_PRESETS=${HOST_PRESETS:-host-debug,host-asan,host-tsan}

# Yocto/PetaLinux SDK: source it if present so CC/CXX/sysroot are in the environment.
if [[ -z "${OECORE_TARGET_SYSROOT:-}" ]]; then
  for f in /opt/sdk/environment-setup-*; do
    # shellcheck disable=SC1090
    [[ -f "$f" ]] && source "$f" && break
  done
fi
if [[ -n "${OECORE_TARGET_SYSROOT:-}" ]]; then
  CROSS_PRESET=${CROSS_PRESET:-arm64-petalinux}
else
  CROSS_PRESET=${CROSS_PRESET:-arm64-debian}
fi

# ThreadSanitizer needs reduced ASLR entropy on recent kernels (vm.mmap_rnd_bits=32
# breaks its shadow mapping). setarch -R disables ASLR for the process; it needs
# the personality syscall, which Docker's default seccomp profile blocks - run the
# container with --security-opt seccomp=unconfined, or lower vm.mmap_rnd_bits on
# the host. Without either, TSan tests run unprotected and may abort.
run_tests() {
  local preset=$1
  if [[ "$preset" == *tsan* ]] && setarch "$(uname -m)" -R true 2>/dev/null; then
    setarch "$(uname -m)" -R ctest --preset "$preset"
  else
    [[ "$preset" == *tsan* ]] && echo "warning: setarch -R unavailable; TSan may abort under high ASLR entropy"
    ctest --preset "$preset"
  fi
}

stage() { echo; echo "=== $1 ==="; }

# A build directory configured from another path (e.g. the host checkout
# bind-mounted here) cannot be reused; start it fresh. ccache keeps it cheap.
fresh_build_dir() {
  local dir=build-$1 cache
  cache=$dir/CMakeCache.txt
  if [[ -f "$cache" ]] && ! grep -q "^CMAKE_HOME_DIRECTORY:INTERNAL=$PWD\$" "$cache"; then
    echo "discarding $dir (configured for another source path)"
    rm -rf "$dir"
  fi
}

for s in ${STAGES//,/ }; do
  case "$s" in
    lint)
      stage lint
      tools/format.sh --check
      tools/check_deps.sh
      ;;
    host)
      for p in ${HOST_PRESETS//,/ }; do
        stage "host: $p"
        fresh_build_dir "$p"
        cmake --preset "$p"
        cmake --build --preset "$p"
        run_tests "$p"
      done
      ;;
    cross)
      stage "cross: $CROSS_PRESET"
      fresh_build_dir "$CROSS_PRESET"
      cmake --preset "$CROSS_PRESET"
      cmake --build --preset "$CROSS_PRESET"
      out=build-$CROSS_PRESET
      file "$out/optronic"
      tar -C "$out" -czf "optronic-aarch64.tar.gz" optronic
      echo "artifact: optronic-aarch64.tar.gz"
      ;;
    qemu)
      stage qemu
      if [[ -x tools/qemu/run.sh ]]; then
        tools/qemu/run.sh "build-$CROSS_PRESET"
      else
        echo "qemu stage not wired up yet (tools/qemu/run.sh missing)"
      fi
      ;;
    *)
      echo "unknown stage: $s" >&2; exit 2 ;;
  esac
done
