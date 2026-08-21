# Optronic

A small Linux service that models the software of an electro-optical sensor node — the kind of unit that sits in a vehicle sight or a 360° situational-awareness system: detectors in, processed and encoded video out, a control link, health reporting, telemetry. Target class: Xilinx Zynq UltraScale+ MPSoC on a System-on-Module; everything is also buildable and testable on a PC.

## Why this repository exists

This repository was prepared by **Yiğit Onat** for the interview with **HENSOLDT Optronics** (via FERCHAU) on 26 August 2026 for the position *Senior C++ Developer / Embedded, Linux, Xilinx SoC, GStreamer*. It is stated openly: the purpose is to demonstrate an understanding of the software architecture, its constraints, and the tasks the position describes — before having seen the actual system.

Everything about the real product is therefore **an inference from the role description and the public product line**, not knowledge of HENSOLDT internals. Where a spec depends on such an assumption it says so, and the open questions are listed in `docs/00_SYSTEM_CONTEXT.md`.

## What the position asks for, and where this repo answers it

| Role description | Where |
|---|---|
| Framework and cross-cutting functions | `framework/` — lifecycle, configuration, logging, control protocol, hardware abstraction, health/BIT, time |
| General software work *outside* GStreamer | everything except `modules/video`; GStreamer headers are confined to that one module and the rule is enforced in the build |
| GStreamer, self-taught | `modules/video` and `docs/19_GSTREAMER_INTERFACES.md` |
| Xilinx SoC and System-on-Module integration | `framework/hal` (UIO register access with a host fake), PetaLinux-SDK cross build, QEMU tests, Yocto recipe hook, boot-chain notes |
| CMake, Docker, MQTT, embedded work packages, C++20 — "to be built up" | `cmake/`, `docker/`, `modules/telemetry`, the test plan, and the commit history itself |
| Hardware can only be tested on the real device | `docs/09_TEST_PLAN.md` — what host and QEMU tests can prove, what only the unit can |
| Documentation in English | `docs/` |

## The interfaces designed around GStreamer

The central design decision: GStreamer is an implementation detail of one module. The rest of the system sees five narrow interfaces and a factory (full definitions in `docs/19_GSTREAMER_INTERFACES.md`):

```
                 ┌─────────────────────────── modules/video ────────────────────────────┐
 PipelineSpec ──►│ PipelineFactory ──► Pipeline (RAII owner, GstPtr, bus thread)          │
                 │      source ─► queue ─► appsink ──► FrameSink::onFrame(FrameView) ──┐  │
                 │                                                                       │  │
                 │   Processor concept  (Passthrough | EdgeOverlay | YoloBoxes | Fusion2) ◄┘  │
                 │                │                                                          │
                 │      FrameSource (appsrc, buffer pool) ─► encoder ─► tee ─► rtp | kms | file│
                 │      BusSink::onBus(BusEvent) ◄── GstBus (error, warning, qos, latency)    │
                 └──────────────┬───────────────────────────────┬────────────────────────┘
                                │                               │
                   health::Monitor (ENCODER_LOST …)     time::LatencyTracker (t0/t1/t2)
```

- **`Pipeline`** owns the graph: create/play/pause/stop, hot property changes, stats; destructor guarantees NULL state. Every GStreamer object lives in a `GstPtr`, every buffer map in a `MapGuard`.
- **`FrameSink` / `FrameView`** hand frames *out* of the pipeline on the streaming thread as a non-owning span with hardware timestamp, sequence and channel id — no allocation, no exceptions.
- **`FrameSource`** hands frames *into* the pipeline from a buffer pool; backpressure is a return value, not a block.
- **`Processor`** is a C++20 concept: `process(in, out)`, a declared time budget, a name. Processors never see GStreamer types; a processor that exceeds its budget causes drops, never latency.
- **`BusSink`** turns `GstMessage` into framework events; the health monitor decides what an encoder error means.
- **`PipelineFactory`** maps a `PipelineSpec` (source, processor, encoder, outputs, queue policy) to a deterministic launch string, so graphs are golden-file tested on the host and the target encoder (`x264enc` vs VCU) is a configuration choice, not an `#ifdef`.

Around that sit the interfaces a sensor node needs anyway: the control ICD (`docs/03`), the register map of the PL block with the NUC shutter sequence (`docs/04`), the Linux ↔ R5 message layer for gimbal, rangefinder and interlocks (`docs/16`), mode management with live pipeline swaps that never show a black frame (`docs/17`), multi-channel synchronization on hardware timestamps (`docs/12`), a glass-to-glass latency budget (`docs/13`), the fault-behaviour table (`docs/06 §6`), data versioning (`docs/18`) and the security seams (`docs/15`).

## Repository layout (target state)

```
framework/   app · config · log · ipc · hal · health · time
modules/     video · sensor · telemetry
tests/       host unit and integration tests; cross-built for QEMU
tools/       nodectl (control client), latency plot
cmake/       toolchain file, presets
docker/      PetaLinux SDK build image, Debian fallback, runtime image
docs/        specifications SPEC-00 … SPEC-19
```

## History as a migration

The repository starts from a deliberately legacy baseline (`v0-legacy`: Makefile, C++14, printf logging, raw `new`, pthreads) and moves to the target state in a fixed order — CMake, presets and toolchain, PetaLinux-SDK container and CI, tests, RAII and sanitizers, C++20, logger and MQTT, documentation — because that order is the answer to "CMake / Docker / CI / C++20 must be built up". The reasoning is in `docs/10_MIGRATION_PLAN.md`.

## Status

Specifications first; code follows commit by commit. `docs/README.md` indexes the specs; each states what is built, what is specified only, and what is assumed about the real system.
