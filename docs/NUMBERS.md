# NUMBERS — measured, not estimated

Everything here was measured on the machine named below and can be reproduced
with the commands given. Numbers that are still estimates are marked as such
and live in `13_LATENCY_BUDGET.md`, not here.

**Host** Intel i7-1165G7 (4 cores / 8 threads, 2.8 GHz), 31 GB RAM, NVMe,
Ubuntu 24.04, GCC 13.3, GStreamer 1.24.2
**Date** 23 August 2026
**Build** `host-debug` for the build and test figures, `host-release` for the pipeline figures

## Build and test

| Metric | Value | How |
|---|---|---|
| Configure | < 1 s | `cmake --preset host-debug` |
| Clean build, 6 jobs | 10 s | `cmake --build --preset host-debug -j6` |
| Test suite, 50 tests | 2 s | `ctest --preset host-debug`; the video and telemetry cases need the GStreamer and mosquitto development files and are skipped without them |
| Binary size | 39 KB debug, 44 KB release | `optronic`, dynamically linked, not stripped |
| C++ in tree | 3802 lines | `git ls-files '*.cpp' '*.hpp' \| xargs wc -l` |

## Video pipeline

Graph `videotestsrc → queue → appsink → C++ → appsrc → queue → x264enc →
h264parse → fakesink`, five seconds per run, frame interval measured in the
`FrameSink::on_frame` callback.

| Configuration | Achieved | In / out | Drops | Interval p50 | Interval p95 |
|---|---|---|---|---|---|
| 1080p @ 30 fps | 30.2 fps | 151 / 151 | 0 | 33.34 ms | 34.00 ms |
| 1080p @ 60 fps | 60.2 fps | 301 / 301 | 0 | 16.66 ms | 16.81 ms |
| 1080p @ 120 fps | 120.2 fps | 601 / 601 | 0 | 8.34 ms | 9.11 ms |
| 1080p @ 240 fps requested | 149.4 fps | 747 / 747 | 0 | 6.64 ms | 8.22 ms |
| 720p @ 30 fps | 30.2 fps | 151 / 151 | 0 | 33.33 ms | 33.48 ms |

Nothing is dropped anywhere up to the ceiling, and every frame that enters the
appsink leaves through the appsrc.

**Ceiling** About 150 fps at 1080p through the whole chain on this host, with
the software encoder in `tune=zerolatency speed-preset=ultrafast`. The limit is
the encoder, not the seam.

**Startup** `Pipeline::create` plus `play()` to PLAYING: 39–42 ms warm, roughly
700 ms on the first run after boot. The difference is GStreamer scanning its
plugin registry, not this code — worth knowing before treating a cold start as
a regression.

### What these numbers are not

- **Not glass-to-glass latency.** This is the interval between frames arriving
  at the appsink and its jitter. The t0/t1/t2 marks of `13_LATENCY_BUDGET.md`
  are specified and not yet implemented; until they are, this repository has no
  measured end-to-end latency and does not claim one.
- **Not a test of the drop path.** `videotestsrc` is live-paced, so encoder
  backpressure slows the source rather than overflowing the queue. The leaky
  queue is configured (`leaky=downstream`, four buffers) but this test never
  makes it drop. A real detector that does not slow down would.
- **Not the target.** All of it is x86 with `x264enc`. On the ZCU104 the
  encoder is `omxh264enc` or the VCU, and the numbers will differ. The encoder
  is a configuration value precisely so that this can be measured without
  changing code.

## aarch64 under QEMU

The cross-built binaries are run again under `qemu-aarch64-static` against the
sysroot the cross build linked them to. This is instruction-set emulation, not
a board: it proves the aarch64 code is correct C++ - alignment, atomics, type
widths, the things that differ from x86 - and nothing about the hardware.

| Binary | aarch64 cases | Result |
|---|---|---|
| `test_core` | 3 | pass |
| `test_app` | 8 | pass |
| `test_log` | 11 | pass |
| `test_hal` | 11 | pass |
| `test_telemetry` | 5 | pass |
| `test_video` | 11 | pass |
| **total** | **49** | **6/6 binaries** |

The service itself also runs emulated, end to end: power-on BIT, GStreamer
pipeline at 30 fps, and MQTT telemetry to a real broker. Startup is about 20
seconds under emulation, almost all of it GStreamer scanning its plugin
registry; the frame path then runs at rate.

What this is not: the PetaLinux QEMU machine that boots the target image with
the real kernel and device tree. That is a separate stage, and `/dev/uio0`
- the one thing the HAL ultimately talks to - exists only there and on the
unit.

## Correctness

| Check | Result |
|---|---|
| `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Werror` | 0 warnings |
| ASan + UBSan, 50 tests, `detect_leaks=1` | 0 findings |
| TSan, 39 tests (framework, telemetry) | 0 findings |
| TSan, video tests | excluded — see below |
| clang-format, clang-tidy, dependency rules | clean |
| aarch64 cross build | `ELF 64-bit LSB pie executable, ARM aarch64` |
| aarch64 under qemu-user | 49 cases, 6/6 binaries |

ThreadSanitizer is not run against `modules/video`. GLib, GObject and libx264
are not built with TSan, so the tool cannot see their locks and atomics and
reports races inside `g_queue_pop_tail`, `g_type_class_ref` and libx264's own
worker pool — none of them in this code. Suppressing that needs a list covering
five third-party libraries, which would hide a real race in the module just as
effectively. The module is covered by ASan/UBSan instead, and the reasoning
sits next to the exclusion in `tests/CMakeLists.txt`.

TSan also needs ASLR disabled on recent kernels: its shadow mapping is at fixed
addresses and high-entropy ASLR collides with it. The build disables ASLR for
the test binaries, and in a container the run needs
`--security-opt seccomp=unconfined` because the default seccomp profile blocks
the `personality` syscall.

## Reproducing

```
docker run --rm --security-opt seccomp=unconfined \
  -v "$PWD:/work" -v optronic-ccache:/ccache optronic/debian
```

Runs lint, the three host presets, the aarch64 cross build and the aarch64
suite under QEMU. The pipeline
throughput figures come from a five-second run per configuration against
`videotestsrc` with the output sink discarded.
