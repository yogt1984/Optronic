# SPEC-01 Software Requirements Specification

## 1. Lifecycle (LC)
- SRS-LC-01 The service shall start its components in declared dependency order and stop them in reverse order.
- SRS-LC-02 On SIGTERM the service shall stop all components and exit with code 0 within 2 s.
- SRS-LC-03 A component failing `init()` shall abort startup; the service exits non-zero with the failing component named in the log.
- SRS-LC-04 The service shall kick a software watchdog (systemd `WATCHDOG=1` via sd_notify when available) every 1 s while healthy.

## 2. Configuration (CF)
- SRS-CF-01 Configuration shall be read from a JSON file given by `--config`; the file shall be validated against the schema in SPEC-05 before any component starts.
- SRS-CF-02 Invalid configuration shall be rejected with a message naming the key and the violated rule.
- SRS-CF-03 Parameters marked *runtime* in SPEC-05 shall be settable at runtime via the control protocol without restarting the pipeline.
- SRS-CF-04 Every parameter shall have a documented default; a missing key uses the default.

## 3. Video pipeline (VP)
- SRS-VP-01 The video module shall build a GStreamer pipeline from a configurable source (`videotestsrc`, `v4l2src`) through `appsink` → processor → `appsrc` → encoder → RTP/UDP sink.
- SRS-VP-02 The encoder shall be selectable: `x264enc` (host) or a VCU/OMX element (target) via config without code change.
- SRS-VP-03 The module shall measure and expose per-stage latency (capture→appsink, processor, appsrc→encoder out) as a rolling p50/p95 in ms.
- SRS-VP-04 End-to-end added latency of the processor stage shall be ≤ 5 ms p95 at 1080p30 on the host reference machine (pass-through processor).
- SRS-VP-05 Pipeline errors reported on the GStreamer bus shall be converted into health events (SPEC-06), never into uncaught exceptions.
- SRS-VP-06 The pipeline shall be rebuildable at runtime (`restart-pipeline`) with a gap ≤ 500 ms between last and first frame.

## 4. Hardware abstraction (HW)
- SRS-HW-01 Register access to PL blocks shall go through `hal::MmioRegion`/`hal::Register` only; no raw `volatile` pointers outside `hal/`.
- SRS-HW-02 The HAL shall provide a fake backend with identical API so all logic tests run on the host.
- SRS-HW-03 Register layout shall be defined once (SPEC-04) and checked at compile time (`static_assert` on offsets and widths).
- SRS-HW-04 The HAL shall support UIO interrupt wait with timeout, cancellable via `std::stop_token`.

## 5. Control interface (CI)
- SRS-CI-01 The service shall accept one control connection on a configurable TCP port using the protocol in SPEC-03.
- SRS-CI-02 Every request shall be answered within 100 ms with either a result or an error code from SPEC-06.
- SRS-CI-03 The protocol shall carry a version; mismatched major version shall be rejected at HELLO.
- SRS-CI-04 A heartbeat shall be sent every 1 s; loss of 3 heartbeats on the client side is a link-down event.

## 6. Health / BIT (HB)
- SRS-HB-01 The service shall maintain a health state ∈ {INIT, OK, DEGRADED, FAULT} per SPEC-06.
- SRS-HB-02 Loss of the encoder shall transition to DEGRADED and fall back to a raw/low-rate stream automatically.
- SRS-HB-03 Power-on BIT shall verify: config valid, HAL region mapped and ID register matches, pipeline reaches PLAYING within 3 s.
- SRS-HB-04 Continuous BIT shall check frame flow (≥ 1 frame / 2 s) and report stall.

## 7. Logging / telemetry (LT)
- SRS-LT-01 Logging shall be structured (timestamp, level, component, message, optional key=value) and non-blocking for the producer (lock-free ring, dedicated sink thread).
- SRS-LT-02 Log production in the frame path shall not allocate.
- SRS-LT-03 Health state, latency p50/p95 and BIT results shall be published via MQTT per SPEC-07 every 1 s (configurable).
- SRS-LT-04 Telemetry loss (broker unreachable) shall not affect the pipeline or health state beyond a WARN log.

## 8. Build / quality (BQ)
- SRS-BQ-01 One CMake tree shall build host (x86-64) and target (aarch64) via presets; the target build shall use the PetaLinux SDK toolchain and sysroot (preset `arm64-petalinux`).
- SRS-BQ-02 `docker run optronic/sdk:<ver>` shall build host and target, run the host test suite, and run the target test suite in PetaLinux QEMU, with no host dependencies besides Docker (and the AMD installer for building the image).
- SRS-BQ-03 Host tests shall pass under ASan/UBSan and TSan presets.
- SRS-BQ-04 The code shall compile warning-free with `-Wall -Wextra -Wpedantic -Werror` on GCC 12 and Clang 16.
- SRS-BQ-05 CI shall run host tests, the cross build and the QEMU target tests inside the PetaLinux SDK image on every push and produce an arm64 artifact.
- SRS-BQ-06 A Debian cross-toolchain fallback image shall build the same tree for reviewers without the PetaLinux installer; its output is not release-grade and the README shall say so.

## 9. Non-functional
- SRS-NF-01 Language: C++20, subset per SPEC-11. No exceptions across C callbacks; no RTTI dependence.
- SRS-NF-02 Steady-state CPU of the framework (excluding encoder) ≤ 5 % of one A53 at 1080p30.
- SRS-NF-03 Binary shall run on a read-only root filesystem; writable paths only under a configured state dir.
- SRS-NF-04 All documents in English; all log messages in English.
