# Optronic

[![ci](https://github.com/yogt1984/Optronic/actions/workflows/ci.yml/badge.svg)](https://github.com/yogt1984/Optronic/actions/workflows/ci.yml)

A small Linux service that models the software of an electro-optical sensor node — the kind of unit that sits in a vehicle sight or a 360° situational-awareness system: detectors in, processed and encoded video out, a control link, health reporting, telemetry. Target class: Xilinx Zynq UltraScale+ MPSoC on a System-on-Module; everything is also buildable and testable on a PC.

## Why this repository exists

This repository was prepared by **Yiğit Onat** for the interview with **HENSOLDT Optronics** (via FERCHAU) on 26 August 2026 for the position *Senior C++ Developer / Embedded, Linux, Xilinx SoC, GStreamer*. It is stated openly: the purpose is to demonstrate an understanding of the software architecture, its constraints, and the tasks the position describes — before having seen the actual system.

Everything about the real product is therefore **an inference from the role description and the public product line**, not knowledge of HENSOLDT internals. Where a spec depends on such an assumption it says so, and the open questions are listed in `docs/00_SYSTEM_CONTEXT.md`.

## What runs today

```
cmake --preset host-debug && cmake --build --preset host-debug && ctest --preset host-debug
```

or the same thing the CI runs, in the container that carries the toolchain:

```
docker run --rm --security-opt seccomp=unconfined -v "$PWD:/work" -v optronic-ccache:/ccache optronic/debian
```

which executes lint (clang-format, clang-tidy, dependency rules), the three
host presets, the aarch64 cross build, and the cross-built suite again under
QEMU user-mode emulation - all 57 test cases on aarch64, on a laptop with no
board attached.

`tools/test.sh` runs the suite without needing to remember any of that:

```
tools/test.sh                # all of it, in the container
tools/test.sh SpscRing       # only tests matching a regex
tools/test.sh --native       # on this machine instead
tools/test.sh --preset host-tsan
```

A host without the GStreamer and mosquitto development files runs 41 of the 57
and reports the rest as absent rather than failing, which is why the container
is the reference environment.

There is also a scripted walk-through - `tools/demo.sh warm` once, then
`tests`, `service`, `nuc` and `qemu` as four separate acts.

The binary is the service itself, not a demo harness:

```
$ ./build-host-debug/optronic --seconds 5
INFO  log        sink thread running
INFO  sensor     power-on BIT passed
INFO  sensor     running gain=256 w=1280 h=720
INFO  video      pipeline PLAYING fps=30 stream=0
INFO  main       service up temp_mc=41250
INFO  video      frame seq=30 bytes=1382400
INFO  video      pipeline stopped frames=91 drops=0
INFO  sensor     stopped
INFO  log        sink stopping written=10 dropped=0
```

`--stream --port 5600` sends RTP instead of discarding it; `--help` prints the
receiver command. SIGTERM brings the components down in reverse order and the
process exits 0 in well under the two seconds SRS-LC-02 allows - measured at
686 ms with the pipeline running.

With `--broker` the service also publishes telemetry, and the interesting part
is what happens when the broker goes away:

```
$ ./build-host-debug/optronic --broker 127.0.0.1 --node sight-01 --seconds 10
INFO  telemetry  connected to broker
WARN  telemetry  publish failed - broker unreachable rc=4     # broker killed at t=3
INFO  telemetry  connected to broker                          # restarted at t=7, reconnected
INFO  telemetry  stopped published=10 failed=10
INFO  video      pipeline stopped frames=301 drops=0          # the pipeline never noticed
```

One warning rather than one per sample, automatic reconnect with backoff, and
not a single dropped frame: telemetry observes and never influences
(SRS-LT-04). A monitor sees

```
$ mosquitto_sub -v -t 'optronic/#'
optronic/sight-01/health  {"v":1,"state":"OK","uptime_s":2,"faults":0}
optronic/sight-01/sensor  {"v":1,"gain":256,"nuc":false,"temp_mc":41250,"frame_cnt":61}
optronic/sight-01/health  {"v":1,"state":"OFFLINE"}
```

The last line is the retained last will, so a monitor that connects afterwards
still learns the unit is gone.

With a camera and `--detect`, the frames go through a detection stage on their
way out:

```
$ tools/cameras.sh --probe          # which /dev/video* actually delivers frames
$ tools/demo.sh detect              # camera -> YOLO -> boxes -> RTP -> window
INFO  detect  YOLOv4-tiny loaded
INFO  detect  frame seq=30 objects=2 infer_ms=112
```

That stage is a `Processor`: it sees a `FrameView` and a `WritableFrame` and no
GStreamer type at all. Detection runs on the luma plane directly - an NV12 Y
plane is already a valid greyscale image - and only on every third frame,
because 110-170 ms of inference against a 33 ms frame period is the sort of
budget problem the design exists to make visible rather than hide. On the
target this is where a DPU would sit.

`--nuc N` runs the non-uniformity correction of `docs/04_HAL_REGISTER_MAP.md`
§3.1 - the one procedure a thermal channel has and a visual camera does not:

```
WARN  sensor  NUC started - channel DEGRADED, shutter closing
INFO  sensor  NUC done - channel OK ms=200 mean_offset=0
```

200 ms against a 600 ms budget, and both ends go out on the MQTT event topic at
QoS 1 rather than only in the 1 Hz sample - an event shorter than the sample
period is invisible to sampling. Every failure path leaves the shutter open: a
failed NUC that leaves the unit blind is worse than an uncorrected image, and
the jammed-shutter and never-finishing-accumulation cases are both tested.

The specifications in `docs/` describe the whole system. The code implements part of it, and this table is the honest split:

| Area | State |
|---|---|
| `docs/` SPEC-00 … SPEC-19 | written |
| CMake targets, presets, aarch64 toolchain file | built |
| Docker images (PetaLinux SDK, Debian fallback), GitHub Actions | built |
| C++23 language level, error model, `expected`, GoogleTest | built |
| `framework/app` — lifecycle, ordered startup, rollback, signals, watchdog | built |
| `modules/video` — pipeline, factory, `GstPtr`/`MapGuard`, `Processor` concept | built |
| `framework/log` — 256-byte records, per-thread SPSC rings, sink thread | built |
| `framework/hal` — typed registers, ISP map, host fake, UIO backend | built |
| `modules/telemetry` — MQTT over libmosquitto, last will, backoff | built |
| the service: BIT, sensor, video, telemetry and logging under one lifecycle | built |
| `qemu` stage: the aarch64 binaries run under user-mode emulation | built |
| `modules/detect` — YOLOv4-tiny over the frame path, boxes drawn in place | built |
| camera input: v4l2 source, device enumeration, format conversion | built |
| `modules/sensor` — the NUC shutter sequence, its timeouts and recovery | built |
| `framework/config · ipc · health · time` | specified, not implemented |
| `tools/nodectl`, PetaLinux QEMU target tests | specified, not implemented |

Twenty-two tests, all green, including the video pipeline running end to end against `videotestsrc`. Eleven of them need the GStreamer development files; a host without them configures, builds and tests everything else and reports the video module as skipped, which is why the container is the reference environment. Clean under ASan/UBSan with leak detection on. ThreadSanitizer covers the framework; the video module is excluded from it deliberately — see `tests/CMakeLists.txt` for why.

The legacy baseline is gone: `main.cpp` is composition now, and the `volatile` register pointer, the hand-rolled ring and the printf logging it used to contain live in `framework/hal`, `framework/log` and their tests. It remains reachable as the `v0-legacy` tag.

## What the position asks for, and where this repo answers it

| Role description | Where |
|---|---|
| Framework and cross-cutting functions | `framework/` — lifecycle, error model, logging and hardware abstraction built; configuration, control protocol, health/BIT and time specified |
| MQTT | `modules/telemetry` — retained last will, backoff reconnect, and publish failures that never move the health state |
| Embedded work packages | `modules/sensor` — the NUC shutter sequence of `docs/04 §3.1`, written and tested against a host fake before any hardware exists |
| General software work *outside* GStreamer | everything except `modules/video`; GStreamer headers are confined to that one module and `tools/check_deps.sh` fails the build if that is violated |
| GStreamer, self-taught | `modules/video` and `docs/19_GSTREAMER_INTERFACES.md` |
| Xilinx SoC and System-on-Module integration | `framework/hal` — typed registers, the ISP map of `docs/04`, a host fake with real side effects, and the UIO backend; PetaLinux-SDK cross build in Docker |
| CMake, Docker, MQTT, embedded work packages, C++20 — "to be built up" | `cmake/`, `docker/`, and the commit history itself |
| Hardware can only be tested on the real device | the `MmioBackend` seam: everything above it is tested on the host against `FakeMmio`, so bench time is spent only on what genuinely needs the unit (`docs/09_TEST_PLAN.md`) |
| Documentation in English | `docs/` |

## The interfaces designed around GStreamer

The central design decision: GStreamer is an implementation detail of one module. The rest of the system sees five narrow interfaces and a factory (full definitions in `docs/19_GSTREAMER_INTERFACES.md`):

```
                 ┌─────────────────────────── modules/video ────────────────────────────┐
 PipelineSpec ──►│ PipelineFactory ──► Pipeline (RAII owner, GstPtr, bus thread)          │
                 │      source ─► queue ─► appsink ──► FrameSink::on_frame(FrameView) ──┐ │
                 │                                                                      │ │
                 │   Processor concept  (Passthrough | EdgeOverlay | YoloBoxes) ◄────────┘ │
                 │                │                                                       │
                 │      appsrc ─► encoder ─► rtppay ─► udpsink                             │
                 │      BusSink::on_bus(BusEvent) ◄── GstBus (error, warning, qos)         │
                 └──────────────┬───────────────────────────────┬────────────────────────┘
                                │                               │
                   health::Monitor (ENCODER_LOST …)     time::LatencyTracker (t0/t1/t2)
                          (specified)                          (specified)
```

- **`Pipeline`** owns the graph: create, play, stop, stats; the destructor guarantees NULL state and joins the bus thread. Every GStreamer object lives in a `GstPtr`, every buffer map in a `MapGuard`. The GStreamer types sit behind a pimpl, so no public header includes `<gst/gst.h>` and CMake links GStreamer `PRIVATE` — the module boundary is checked by the compiler, not by review.
- **`FrameSink` / `FrameView`** hand frames *out* of the pipeline on the streaming thread as a non-owning span with timestamp, sequence and channel id — no allocation, no exceptions.
- **`Processor`** is a C++20 concept: `process(in, out)`, a declared time budget, a name. Processors never see GStreamer types.
- **`BusSink`** turns `GstMessage` into framework events on a dedicated bus thread; which element failed decides whether it is recoverable.
- **`PipelineFactory::launch_string(spec)`** maps a `PipelineSpec` to a deterministic launch string. It is pure text with no GStreamer dependency, so the graph a configuration produces is unit-tested on any machine, with no hardware and no display. The target encoder (`x264enc` / `omxh264enc` / `vvas_xvcuenc`) is a configuration choice, not an `#ifdef`.

## Repository layout

```
main.cpp     composition: three components, one lifecycle   built
framework/
  core/      error model, expected                       built
  app/       lifecycle, component order, signals         built
  log/       fixed records, SPSC rings, sink thread      built
  hal/       typed registers, ISP map, fake + UIO        built
  config/ ipc/ health/ time/                             specified
modules/
  video/     pipeline, factory, processors               built
  sensor/ telemetry/                                     specified
modules/
  telemetry/ MQTT publisher, last will, backoff        built
  sensor/    NUC shutter sequence, timeouts, recovery   built
  detect/    YOLOv4-tiny detection stage (OpenCV)        built
tests/       core, app, log, hal, video, telemetry,
             sensor - 57 host cases, all also on aarch64  built
tools/       check_deps.sh, format.sh                    built
cmake/       toolchain file, sanitizer selection         built
docker/      PetaLinux SDK image, Debian fallback, CI    built
docs/        SPEC-00 … SPEC-19                           written
```

## Measured numbers

Host: Intel i7-1165G7, 8 threads, GCC 13, `host-release`, 23 August 2026. Graph is `videotestsrc → appsink → C++ → appsrc → x264enc`, five seconds per run.

| Configuration | Achieved | Drops | Frame interval p50 / p95 |
|---|---|---|---|
| 1080p @ 30 fps | 30.2 fps | 0 | 33.34 / 34.00 ms |
| 1080p @ 60 fps | 60.2 fps | 0 | 16.66 / 16.81 ms |
| 1080p @ 120 fps | 120.2 fps | 0 | 8.34 / 9.11 ms |
| 1080p @ 240 fps requested | 149.4 fps — ceiling | 0 | 6.64 / 8.22 ms |

Pipeline construction to PLAYING: 39–42 ms warm, about 700 ms on the first run of a boot — the difference is GStreamer's plugin registry scan, not this code. Clean debug build 10 s on six jobs, 22 tests in 1.5 s, release binary 44 KB unstripped.

Two things these numbers are not. They measure frame *interval and jitter*, not glass-to-glass latency: the t0/t1/t2 marks of `docs/13_LATENCY_BUDGET.md` are specified and not yet implemented. And the drop counter stays at zero because `videotestsrc` is live-paced, so encoder backpressure slows the source instead of overflowing the queue — the leaky-queue drop path is configured but not exercised by this test.

## History as a migration

```
git log --oneline v0-legacy..HEAD
git diff --stat v0-legacy HEAD
```

The repository starts from a deliberately legacy baseline and moves toward the target state in a fixed order — CMake, presets and toolchain, container and CI, tests and C++23, then components — because that order is the answer to "CMake / Docker / CI / C++20 must be built up". The reasoning is in `docs/10_MIGRATION_PLAN.md`.

| | `v0-legacy` | now |
|---|---|---|
| Build | flat Makefile | CMake targets, presets, aarch64 toolchain file |
| Standard | C++14 | C++23 (`expected`, `jthread`, concepts) |
| Structure | one 400-line `main.cpp` | 512 lines of composition over seven libraries |
| Memory | raw `new`/`delete`, `volatile` register pointer | RAII throughout; `volatile` confined to the UIO backend |
| Registers | `uint32_t*` and offsets by hand | typed registers; writing a read-only one will not compile |
| Errors | return codes and `printf` | `expected<T, Error>` with the SPEC-06 code space |
| Logging | `printf` from the frame path | fixed 256-byte records, lock-free ring, sink thread, 0 allocations |
| Shutdown | none | ordered, reverse, rollback on failure, 686 ms on SIGTERM |
| Tests | none | 57, all of them also on aarch64; clean under ASan/UBSan and TSan |
| Reproducibility | works on my machine | one container, laptop and CI identical, CI green |

`git diff --stat v0-legacy..HEAD` is 69 files, +5810/-449, over 22 commits.

C++23 rather than C++20 for exactly one reason: `std::expected` is C++23. The rest of the code stays inside the C++20 subset of `docs/11_CODING_GUIDELINES.md`, and the language level is a property of one CMake target, not a global flag.

The build checks for `<expected>` by compiling a two-line probe rather than by
comparing a compiler version, because the version would have lied: libstdc++ 13
gates the header on `__cpp_concepts >= 202002L`, and Clang 17 and 18 both report
`201907L` since they do not implement P2113. Clang therefore cannot see
`std::expected` with libstdc++ at any currently packaged version - it works
against libc++, which then needs its own GoogleTest. That is why the CI matrix
is GCC only, and why a wrong toolchain now gets one sentence instead of a
template backtrace.

## What I would do next

1. `framework/health` — the INIT/OK/DEGRADED/FAULT machine of `docs/06`. The NUC already emits the events it would consume; today they go to the log and to MQTT, but nothing owns the state.
2. The PetaLinux QEMU machine, which boots the actual target image instead of emulating only the instruction set. The image builds; wiring it into the `qemu` stage is the remaining step, and until then the user-mode run is the honest half of the check.
3. The t0/t1/t2 latency marks and the rolling window, which turn the interval numbers above into a real glass-to-glass budget - the detector makes this concrete, since inference is 110-170 ms against a 33 ms frame period.
4. A `GstBufferPool` behind `FrameSource`, so a processor can write output frames without allocating in the frame path. Today the passthrough refs the input buffer instead.
5. `framework/config` — the JSON store and schema, replacing the command-line flags the service currently takes.

The order is deliberate: what everything else depends on comes first, and the demonstrable feature comes last.
