# SPEC-02 Software Architecture

## 1. Layers and dependency rule
```
modules/   (video, sensor, telemetry)       may depend on framework/, never on each other
framework/ (app, config, log, ipc, hal, health, time)   may depend on std + third-party only
third-party: GStreamer (modules/video only), mosquitto (modules/telemetry only), nlohmann/json (config, ipc)
```
Rule: no `#include <gst/...>` outside `modules/video`; no `#include <mosquitto.h>` outside `modules/telemetry`. Enforced by a CMake target visibility (PRIVATE link) and a grep in CI.

## 2. Components
| Component | Responsibility | Threads |
|---|---|---|
| `app::Service` | owns components, lifecycle order, signals, watchdog | main thread |
| `config::Store` | load/validate/expose config; runtime set with change notification | none (called from ipc thread) |
| `log::Logger` | lock-free SPSC ring per producer thread → sink thread | 1 sink thread |
| `ipc::Server` | TCP accept, framing, dispatch to handlers | 1 thread (epoll) |
| `hal::MmioRegion`, `hal::Register`, `hal::Irq` | UIO mmap, typed registers, irq wait | caller's thread |
| `health::Monitor` | state machine, BIT, event intake | 1 thread (timer) |
| `time::Clock`, `time::LatencyTracker` | monotonic stamps, p50/p95 | none |
| `video::Pipeline` | GStreamer graph, appsink/appsrc, bus → health | GStreamer streaming threads + 1 bus thread |
| `sensor::IspControl` | gain/NUC via hal registers | none |
| `telemetry::MqttPublisher` | publish health/latency | 1 thread (mosquitto loop) |

## 3. Startup order
`log → config → hal → health → sensor → video → ipc → telemetry`. Shutdown is the reverse. Rationale: logging first so every failure is visible; ipc last so no command arrives before the pipeline exists.

## 4. Data flow (frame path)
```
source ─► caps ─► appsink ──(GstSample, zero-copy map)──► Processor::process(FrameView) ──► appsrc ─► encoder ─► rtppay ─► udpsink
                     │ t0 stamp                                  │ t1                         │ t2 (encoder out probe)
                     └──────────────────── LatencyTracker(t0,t1,t2) ──────────────────────────┘
```
No allocation in the frame path; `FrameView` = `std::span<const std::byte>` + stride/format. Processor runs in the appsink callback thread (GStreamer streaming thread). If processing cost > frame period, a leaky queue drops oldest (configurable).

## 5. Control flow
ipc thread parses a request → looks up handler → handler calls into `config::Store` or posts a command to the owning component via a thread-safe mailbox → reply. Handlers never block on the pipeline for more than 100 ms (SRS-CI-02); long operations (restart) reply ACCEPTED and completion is an event.

## 6. Error handling
- Inside framework: `std::expected<T, Error>` (SPEC-06 codes). Exceptions only for programmer errors / at startup (config parse), never across C callbacks or threads.
- GStreamer bus errors → `health::Event{ENCODER_LOST, ...}` → state machine decides.

## 7. Concurrency rules
- Each component owns its threads; cross-component communication via mailboxes or atomics, never shared mutable state without a documented owner.
- Lock ordering: `config → health → video`; never the reverse.
- All threads `std::jthread`; stop via `stop_token`; no detached threads.

## 8. Reserved extension points
- `hal::RpmsgChannel` interface (R5 bridge) — declared, not implemented.
- `Processor` plugin registry for future PL/DPU-backed processors.
