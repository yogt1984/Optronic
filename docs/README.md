# Optronic — Specifications (index)

Project: `Optronic` — miniature EO/IR sensor-node service on embedded Linux (Xilinx Zynq UltraScale+ class target, host-testable).
Purpose: demonstrate the "Framework-/Querschnittsfunktionen" layer around a GStreamer video pipeline, plus the "muss aufgebaut werden" items (CMake, Docker/CI, MQTT, embedded test discipline, C++20 modernisation).
Document language: English (as per customer convention). Code comments English, commit messages English.

| ID | Document | Content |
|---|---|---|
| SPEC-00 | [00_SYSTEM_CONTEXT.md](00_SYSTEM_CONTEXT.md) | Assumed target system, scope, out of scope, glossary |
| SPEC-01 | [01_SOFTWARE_REQUIREMENTS.md](01_SOFTWARE_REQUIREMENTS.md) | Functional + non-functional requirements (SRS-xxx), traceable to tests |
| SPEC-02 | [02_ARCHITECTURE.md](02_ARCHITECTURE.md) | Layers, components, threading model, data flow, dependency rules |
| SPEC-03 | [03_ICD_CONTROL_PROTOCOL.md](03_ICD_CONTROL_PROTOCOL.md) | Interface Control Document: TCP control protocol, messages, versioning |
| SPEC-04 | [04_HAL_REGISTER_MAP.md](04_HAL_REGISTER_MAP.md) | AXI4-Lite register map of the (fake) PL ISP block, UIO mapping, access rules |
| SPEC-05 | [05_CONFIG_SCHEMA.md](05_CONFIG_SCHEMA.md) | Configuration file schema, defaults, runtime-settable parameters |
| SPEC-06 | [06_ERROR_MODEL_BIT.md](06_ERROR_MODEL_BIT.md) | Error codes, health states, Built-In Test, degraded modes |
| SPEC-07 | [07_LOGGING_TELEMETRY.md](07_LOGGING_TELEMETRY.md) | Structured logging, latency markers, MQTT telemetry topics/payloads |
| SPEC-08 | [08_BUILD_CI.md](08_BUILD_CI.md) | CMake layout, presets, toolchain, Docker images, CI pipeline, Yocto hook |
| SPEC-09 | [09_TEST_PLAN.md](09_TEST_PLAN.md) | Test levels (host unit / host integration / target / HIL), coverage of SRS |
| SPEC-10 | [10_MIGRATION_PLAN.md](10_MIGRATION_PLAN.md) | Legacy→modern commit series, rationale, risks, what is deliberately not done |
| SPEC-11 | [11_CODING_GUIDELINES.md](11_CODING_GUIDELINES.md) | C++20 subset, banned constructs, error handling, concurrency rules |
| SPEC-12 | [12_MULTI_CHANNEL_SYNC.md](12_MULTI_CHANNEL_SYNC.md) | N channels, hardware timestamps, frame pairing for fusion / PiP |
| SPEC-13 | [13_LATENCY_BUDGET.md](13_LATENCY_BUDGET.md) | Glass-to-glass budget per stage, design rules, measurement, CI gate |
| SPEC-14 | — folded into SPEC-06 §6 | Fault behaviour table: operator view, system action, recovery time |
| SPEC-15 | [15_SECURITY_SEAMS.md](15_SECURITY_SEAMS.md) | Where secure boot, signed data and authenticated links attach (v1 plain) |
| SPEC-16 | [16_R5_MESSAGE_INTERFACE.md](16_R5_MESSAGE_INTERFACE.md) | Linux ↔ R5 RPMsg / UART message layer: gimbal, LRF, shutter, interlocks |
| SPEC-17 | [17_MODE_MANAGEMENT.md](17_MODE_MANAGEMENT.md) | Operator modes, hot vs cold transitions, live pipeline swap without black frames |
| SPEC-18 | [18_CALIBRATION_CONFIG_VERSIONING.md](18_CALIBRATION_CONFIG_VERSIONING.md) | Factory / config / runtime data classes, schema versions, atomic writes |
| SPEC-19 | [19_GSTREAMER_INTERFACES.md](19_GSTREAMER_INTERFACES.md) | The five interfaces that isolate GStreamer: Pipeline, FrameSink/View, FrameSource, Processor, BusSink; factory, probes, threading |

Conventions: requirement IDs `SRS-<area>-<nn>`; each SRS has at least one test ID `TST-<nn>` in SPEC-09. "Shall" = mandatory, "should" = recommended. Everything about the real customer system is an assumption (see SPEC-00 §3) and is labelled as such.
