# SPEC-13 Glass-to-glass latency budget

Operator-facing requirement: a gunner/commander sight should stay well under ~100 ms photon-to-display; situational-awareness views tolerate ~150 ms. The node owns the part from detector readout to the network/display output.

## 1. Budget (1080p30 channel, H.264 path; targets for the real device — the host demo reports its own numbers)
| Stage | Owner | Budget | Measured by |
|---|---|---|---|
| Detector integration + readout | sensor/PL | ≤ 1 frame (33 ms), outside SW control | datasheet |
| PL processing (NUC/ISP/scale) | PL | ≤ 2 ms | PL counter vs FRAME_DONE |
| DMA to DDR + V4L2 dequeue wake-up | kernel | ≤ 2 ms | `hw_ts` → `appsink` t0 |
| Processor stage (overlay/analytics) | APU, framework | ≤ 5 ms passthrough/overlay · ≤ 40 ms analytics (may drop) | t0 → t1 |
| Encoder (VCU, low-latency mode) | VCU | ≤ 1 frame, typically 8–15 ms | t1 → encoder src probe t2 |
| Packetize + send | APU | ≤ 1 ms | t2 → udpsink |
| Network | — | ≤ 1 ms on LAN | — |
| HMI decode + render | HMI | ≤ 1–2 frames | receiver clock vs `hw_ts` |
| **Node total (hw_ts → last packet out)** | | **≤ 30 ms p95** | `LatencyTracker` |

## 2. Design rules that keep the budget
- `queue leaky=downstream max-size-buffers=1` directly after the source and before the encoder; never unbounded buffering in the live path (the recording branch may buffer).
- Encoder: no B-frames, short GOP, CBR, slice-based output where supported; `tune=zerolatency` on x264 for the host.
- `appsink sync=false max-buffers=1 drop=true`; the processor runs on the streaming thread and must not block beyond budget; heavy processors run in a worker with frame skipping.
- Local display via `kmssink` from the same `tee`, never through the encoder.
- Zero-copy: dma-buf from V4L2 straight into the VCU; the processor maps read-only unless it writes the overlay.

## 3. Measurement
- `LatencyTracker` marks t0/t1/t2 per frame; rolling 256-frame p50/p95/max per stage; exported via STATUS and MQTT.
- Glass-to-glass test: a millisecond clock on the source (`clockoverlay` on the test pattern, or an LED array in the lab) decoded on the receiver; difference = end-to-end. Recorded in NUMBERS.md with the exact setup and hardware.
- Regression gate in CI (host): processor p95 and node-total p95 must not grow more than 20 % against the last release tag.

## 4. Requirements
- SRS-LB-01 The node shall expose per-stage and total latency p95 at runtime.
- SRS-LB-02 The live path shall not buffer more than one frame at any stage.
- SRS-LB-03 A processor exceeding its budget shall cause frame drops, never latency growth, and the drops shall be visible in telemetry.
