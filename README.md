# Optronic

[![ci](https://github.com/yogt1984/Optronic/actions/workflows/ci.yml/badge.svg)](https://github.com/yogt1984/Optronic/actions/workflows/ci.yml)

A small Linux service that models the software of an electro-optical sensor node — the kind of unit that sits in a vehicle sight or a 360° situational-awareness system: detectors in, processed and encoded video out, a control link, health reporting, telemetry. It is written **for** a Xilinx Zynq UltraScale+ MPSoC on a System-on-Module - aarch64 under Linux, an AXI4-Lite register block in the programmable logic reached through UIO, hardware or software H.264 - and it has **never run on one**. Everything here builds and runs on a PC; the aarch64 binaries are exercised under emulation. `docs/NUMBERS.md` says exactly what is measured and what is not.

## Quickstart

Debian or Ubuntu, a USB camera, and nothing else installed:

```bash
git clone https://github.com/yogt1984/Optronic.git && cd Optronic
./install.sh     # host packages, container image, detection model  (once)
./run.sh         # camera check, then the live detector
```

`install.sh` puts only what cannot live in a container on the host - Docker,
and the GStreamer decoder and sink that put the stream on screen. The compiler,
GStreamer development files, OpenCV and mosquitto stay inside the image, so the
build is the same on a laptop and in CI. It is safe to run twice; every step
checks before it acts.

`run.sh` probes each camera by grabbing a real frame from it, then streams the
chosen one through the detector: boxes drawn inside the frame path, the objects
published over MQTT, and a video window. Ctrl-C stops everything.

Then, at your own pace:

```bash
tools/test.sh                  # the suite, in the container
tools/demo.sh                  # the individual demo acts
tools/cameras.sh --probe       # which /dev/video* actually delivers frames
```

## Tech stack

| | |
|---|---|
| Language | C++23 (`std::expected`, `jthread`/`stop_token`, concepts, `<=>`), GCC 13 |
| Build | CMake 3.25 with presets, Ninja, ccache, an aarch64 toolchain file |
| Video | GStreamer 1.24 - `appsink`/`appsrc`, `x264enc`, RTP over UDP |
| Detection | OpenCV 4.6 DNN, YOLOv5s (ONNX) and YOLOv4-tiny (darknet) |
| Telemetry | MQTT via libmosquitto 2.0 |
| Tests | GoogleTest, CTest, 67 cases; ASan, UBSan and ThreadSanitizer presets |
| Static analysis | clang-format and clang-tidy 17, plus a dependency-boundary check |
| Containers | Docker - a Debian build image (used for everything here), a PetaLinux tools image (works), a PetaLinux SDK image (Dockerfile complete, the build has not finished) |
| Cross build | aarch64 against a Debian sysroot; PetaLinux 2024.1 / Yocto where the SDK is available |
| Target emulation | QEMU user-mode - the whole suite re-run on aarch64 |
| CI | GitHub Actions: image, lint, three host presets, cross, QEMU |
| Target class | written for Xilinx Zynq UltraScale+ MPSoC - A53 under Linux, AXI4-Lite over UIO, VCU or software encode. **No Zynq hardware was involved**: the register map is inferred, and the UIO path has only ever run against a host fake |

## Why this repository exists

This repository was prepared by **Yiğit Onat** for the interview with **HENSOLDT Optronics** (via FERCHAU) on 26 August 2026 for the position *Senior C++ Developer / Embedded, Linux, Xilinx SoC, GStreamer*. It is stated openly: the purpose is to demonstrate an understanding of the software architecture, its constraints, and the tasks the position describes — before having seen the actual system.

Everything about the real product is therefore **an inference from the role description and the public product line**, not knowledge of HENSOLDT internals. Where a spec depends on such an assumption it says so, and the open questions are listed in `docs/00_SYSTEM_CONTEXT.md`.

## What runs today

```
cmake --preset host-debug && cmake --build --preset host-debug && ctest --preset host-debug
```

or the whole pipeline the way CI runs it - lint, three host presets, the
aarch64 cross build, and the cross-built suite re-run under QEMU:

```
docker run --rm --security-opt seccomp=unconfined \
  -v "$PWD:/work" -v optronic-ccache:/ccache optronic/debian
```

The binary is the service, not a demo harness:

```
$ ./build-host-debug/optronic --seconds 5
INFO  log        sink thread running
INFO  sensor     power-on BIT passed
INFO  video      pipeline PLAYING fps=30 stream=0
INFO  main       service up temp_mc=41250
INFO  video      pipeline stopped frames=91 drops=0
```

`--stream` sends RTP, `--broker` publishes telemetry, `--camera --detect` puts
a camera through the detector, `--nuc N` runs the shutter sequence. `--help`
lists them; `tools/demo.sh` scripts each as its own short act.

Four MQTT topics, of which the last two are the interesting ones:

```
optronic/sight-01/health      {"v":1,"state":"OK","uptime_s":7,"faults":0}
optronic/sight-01/sensor      {"v":1,"gain":256,"nuc":false,"temp_mc":41250,"frame_cnt":71}
optronic/sight-01/event       {"v":1,"id":"NUC_STARTED","code":0,"text":"shutter closing..."}
optronic/sight-01/detections  {"v":1,"seq":90,"count":1,"objects":[
                                 {"label":"person","conf":0.72,"x":188,"y":38,"w":434,"h":413}]}
```

Events are pushed rather than sampled: a 200 ms NUC is invisible to a 1 Hz
sample, so it goes out at QoS 1 on its own topic. `health` is retained and the
last will is `{"state":"OFFLINE"}`, so a monitor that connects late still knows
the unit is gone. Kill the broker mid-run and the pipeline loses **zero
frames** - telemetry observes and never influences.

| Area | State |
|---|---|
| `docs/` SPEC-00 … SPEC-19 | written |
| CMake targets, presets, aarch64 toolchain; Docker build image; GitHub Actions | built |
| `framework/` — `core` errors, `app` lifecycle, `log` SPSC ring, `hal` typed registers | built |
| `modules/` — `video` pipeline, `telemetry` MQTT, `sensor` NUC, `detect` YOLO | built |
| camera input, QEMU stage, the service wiring it all together | built |
| PetaLinux tools image (licensed installer + ZCU104 BSP) | built |
| PetaLinux SDK image | Dockerfile complete, build unfinished |
| `framework/config · ipc · health · time`, `tools/nodectl` | specified, not implemented |

67 tests, **all of them also on aarch64** under emulation. Clean under
ASan/UBSan with leak detection and under ThreadSanitizer. Numbers and their
caveats: `docs/NUMBERS.md`.


## What the position asks for, and where this repo answers it

| Role description | Where |
|---|---|
| Framework and cross-cutting functions | `framework/` — lifecycle, error model, logging and hardware abstraction built; configuration, control protocol, health/BIT and time specified |
| MQTT | `modules/telemetry` — retained last will, backoff reconnect, and publish failures that never move the health state |
| Embedded work packages | `modules/sensor` — the NUC shutter sequence of `docs/04 §3.1`, written and tested against a host fake before any hardware exists |
| General software work *outside* GStreamer | everything except `modules/video`; GStreamer headers are confined to that one module and `tools/check_deps.sh` fails the build if that is violated |
| GStreamer, self-taught | `modules/video` and `docs/19_GSTREAMER_INTERFACES.md` |
| Xilinx SoC and System-on-Module integration | `framework/hal` — typed registers, the ISP map of `docs/04_HAL_REGISTER_MAP.md`, a host fake with real side effects, and the UIO backend; PetaLinux-SDK cross build in Docker |
| CMake, Docker, MQTT, embedded work packages, C++20 — "to be built up" | `cmake/`, `docker/`, and the commit history itself |
| Hardware can only be tested on the real device | the `MmioBackend` seam: everything above it is tested on the host against `FakeMmio`, so bench time is spent only on what genuinely needs the unit (`docs/09_TEST_PLAN.md`) |
| Documentation in English | `docs/` |

## The design decision that shapes everything

GStreamer is an implementation detail of one module. No public header includes
`<gst/gst.h>` - `Pipeline` sits behind a pimpl, GStreamer is linked `PRIVATE`,
and `tools/check_deps.sh` fails the build if it escapes. The same rule holds
mosquitto inside `modules/telemetry` and OpenCV inside `modules/detect`.

Two consequences worth the trouble:

- **The graph is data.** `launch_string(spec)` is deterministic text with no
  GStreamer dependency, so what a configuration produces is unit-tested with no
  hardware and no display. The encoder is a config value - `x264enc` here,
  `omxh264enc` or the VCU on the target - not an `#ifdef`.
- **The interesting stage sees no GStreamer.** A `Processor` gets a `FrameView`
  and a `WritableFrame`. The detector is one; on the target a DPU would be
  another, and nothing around it changes.

Full interface definitions: `docs/19_GSTREAMER_INTERFACES.md`.


## Repository layout

```
install.sh  run.sh      setup, then the live demo
main.cpp                composition: components under one lifecycle
framework/  core · app · log · hal          (config · ipc · health · time specified)
modules/    video · telemetry · sensor · detect
tests/      67 cases, all of them also run on aarch64
tools/      test.sh · demo.sh · cameras.sh · get_model.sh · check_deps.sh
cmake/ docker/ docs/    toolchain, images and CI, SPEC-00 … SPEC-19
```


## Measured numbers

Intel i7-1165G7, `host-release`. Full tables and what each figure is *not* in
`docs/NUMBERS.md`.

| | |
|---|---|
| 1080p30 through the whole chain | 0 drops; ceiling ~150 fps |
| pipeline construction to PLAYING | 39-42 ms warm |
| SIGTERM to exit 0 | 686 ms (budget 2 s) |
| NUC shutter sequence | 200 ms (budget 600 ms) |
| detection, yolov4-tiny at 320 | ~190 ms, on a worker thread |
| inference inline vs on a worker | 220 vs 361 frames delivered in 12 s |


## History as a migration

```
git log --oneline v0-legacy..v1-modern      # 36 commits
git diff --stat  v0-legacy..v1-modern       # 81 files, +7652/-447
```

It starts from a deliberately legacy baseline and moves to the target state in
a fixed order - build system, container, CI, tests, C++23, then components -
because that order is the answer to "CMake / Docker / CI / C++20 must be built
up". Reasoning in `docs/10_MIGRATION_PLAN.md`.

| | `v0-legacy` | now |
|---|---|---|
| Build | flat Makefile | CMake targets, presets, aarch64 toolchain |
| Standard | C++14 | C++23 (`expected`, `jthread`, concepts) |
| Structure | one 400-line `main.cpp` | composition over eight libraries |
| Memory | raw `new`/`delete`, `volatile` register pointer | RAII throughout; `volatile` only in the UIO backend |
| Registers | `uint32_t*` and offsets by hand | typed; writing a read-only one will not compile |
| Errors | return codes and `printf` | `expected<T, Error>` over a documented code space |
| Logging | `printf` from the frame path | 256-byte records, lock-free ring, 0 allocations |
| Shutdown | none | ordered, reverse, rollback, 686 ms on SIGTERM |
| Tests | none | 67, all also on aarch64; ASan/UBSan and TSan clean |
| Reproducibility | works on my machine | one container, laptop and CI identical, CI green |

C++23 rather than C++20 for one reason: `std::expected` is C++23. The rest
stays inside the C++20 subset of `docs/11_CODING_GUIDELINES.md`, and the build
checks for the header by compiling a probe rather than comparing a version -
libstdc++ 13 gates it on `__cpp_concepts >= 202002L` and Clang reports
`201907L`, so a version check would have lied.


## What I would do next

1. `framework/health` — the INIT/OK/DEGRADED/FAULT machine of `docs/06_ERROR_MODEL_BIT.md`. The NUC already emits the events it would consume; today they go to the log and to MQTT, but nothing owns the state.
2. The PetaLinux QEMU machine, which boots the actual target image instead of emulating only the instruction set. The tools image works and a full Yocto build ran through all 8051 tasks, but the SDK never came out packaged - the last attempt died on a transient fetch error. Until it does, the user-mode run is the honest half of the check.
3. The t0/t1/t2 latency marks and the rolling window, which turn the interval numbers above into a real glass-to-glass budget - the detector makes this concrete, since inference is 110-170 ms against a 33 ms frame period.
4. A `GstBufferPool` behind `FrameSource`, so a processor can write output frames without allocating in the frame path. Today the passthrough refs the input buffer instead.
5. `framework/config` — the JSON store and schema, replacing the command-line flags the service currently takes.

The order is deliberate: what everything else depends on comes first, and the demonstrable feature comes last.
