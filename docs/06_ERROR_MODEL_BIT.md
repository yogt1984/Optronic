# SPEC-06 Error Model, Health States, Built-In Test

## 1. Error code space (u16)
| Range | Domain |
|---|---|
| 0x0000 | OK |
| 0x0001–0x00FF | generic (E_INVALID_ARG 0x01, E_TIMEOUT 0x02, E_NOT_READY 0x03, E_IO 0x04, E_NO_MEM 0x05) |
| 0x0100–0x01FF | ipc / protocol (SPEC-03) |
| 0x0200–0x02FF | config (E_CFG_PARSE 0x201, E_CFG_SCHEMA 0x202, E_CFG_RANGE 0x203) |
| 0x0300–0x03FF | hal (E_HAL_OPEN 0x301, E_HAL_MAP 0x302, E_HAL_ID_MISMATCH 0x303, E_HAL_IRQ_TIMEOUT 0x304) |
| 0x0400–0x04FF | video (E_VID_BUILD 0x401, E_VID_STATE 0x402, E_VID_ENCODER_LOST 0x403, E_VID_STALL 0x404, E_VID_CAPS 0x405) |
| 0x0500–0x05FF | health/BIT (E_BIT_POWERON 0x501, E_BIT_CONTINUOUS 0x502) |
| 0x0600–0x06FF | telemetry (E_MQTT_CONNECT 0x601 — WARN only, never affects state) |
`struct Error { uint16_t code; std::string_view where; }`; carried in `std::expected<T, Error>`.

## 2. Health state machine
```
INIT ──poweron BIT pass──► OK ◄──recovered──┐
  │                         │               │
  └─ BIT fail ─► FAULT      ├─ recoverable ─► DEGRADED
                 ▲          │                   │
                 └──────────┴── unrecoverable ──┘
```
| State | Meaning | Pipeline | Heartbeat | Exit |
|---|---|---|---|---|
| INIT | starting, BIT running | not built | none | → OK / FAULT |
| OK | all checks pass | full | yes | |
| DEGRADED | service continues with reduced function | fallback (raw, low rate) | yes, flagged | → OK when cause clears |
| FAULT | cannot deliver primary function | stopped | yes, flagged | restart by supervisor (systemd `Restart=on-failure`) |

## 3. Events → transitions
| Event | From | To | Action |
|---|---|---|---|
| ENCODER_LOST (bus error on encoder) | OK | DEGRADED | rebuild pipeline with `health.degraded_fallback` |
| ENCODER_BACK (manual restart succeeds) | DEGRADED | OK | |
| FRAME_STALL (> frame_stall_ms) | OK/DEGRADED | DEGRADED | restart pipeline once; second stall within 30 s → FAULT |
| HAL_OVERFLOW | OK | DEGRADED | log, clear, count; > 10/min → FAULT |
| HAL_ID_MISMATCH / HAL_MAP fail | INIT | FAULT | |
| CONFIG_INVALID at runtime set | any | unchanged | reject with E_CFG_RANGE |
| MQTT down | any | unchanged | WARN |

## 4. Built-In Test
Power-on BIT (≤ 3 s total): config valid · HAL mapped, ID/VERSION match · SW_RESET leaves CTRL=0 · temperature in range · pipeline reaches PLAYING · first frame within 1 s.
Continuous BIT (every 1 s): FRAME_CNT advancing · no OVERFLOW · latency p95 < 3× nominal · heartbeat thread alive · sink thread backlog < 50 %.
Result struct: `{kind, timestamp, passed, failed_checks: bitmask, details[]}` → STATUS/BIT_RESULT (ICD) and MQTT `…/bit`.

## 5. Fault injection (debug builds)
INJECT_FAULT ids: 1 encoder_lost, 2 frame_stall, 3 hal_overflow, 4 hal_id_mismatch (next BIT), 5 mqtt_down, 6 rt_link_lost, 7 over_temp. Compiled out with `-DOPTRONIC_FAULT_INJECT=OFF` (release default).

## 6. Fault behaviour table — what the operator sees, what the system does, how fast
| Fault | Detection | Visible behaviour | System action | Recovery target |
|---|---|---|---|---|
| Encoder (VCU / x264) lost | bus ERROR from the encoder element | stream to the HMI switches to raw/low-rate fallback; STATUS shows DEGRADED | rebuild pipeline with the fallback branch; retry encoder every 10 s | < 1 s to fallback |
| Source stall (no frame > `frame_stall_ms`) | continuous BIT, FRAME_CNT static | last good frame held, STALE flag in STATUS/overlay; never a black screen without a reason code | restart pipeline once; second stall within 30 s → FAULT | < 2.5 s |
| PL overflow / underflow | STATUS.OVERFLOW irq | none if isolated; DEGRADED after 3 in 10 s | clear, count, step down fps or resolution tier | immediate |
| Network loss to HMI / C2 | heartbeat unacknowledged, socket error | local display unaffected; telemetry queued (bounded) | reconnect with backoff; control link re-HELLO | < 3 s after link returns |
| MQTT broker down | connect / publish failure | none | WARN once, backoff 1…30 s | n/a |
| State dir / disk full | write error on log or recording | recording stops with EVENT; pipeline unaffected | delete oldest recordings by policy | immediate |
| Over-temperature (`TEMP_MC` above limit) | continuous BIT | temperature flag; DEGRADED at warn, FAULT at critical | shed load (drop analytics processor, lower fps), then shut the sensor channel | per threshold |
| Config invalid at runtime set | schema / range check | rejected with E_CFG_RANGE, old value kept | none | immediate |
| Main loop hung (watchdog not kicked) | systemd `WatchdogSec` | service restart, ~3 s gap | `Restart=on-failure`; state restored from the state dir | < 5 s to PLAYING |
| R5 heartbeat lost | SPEC-16 heartbeat | gimbal / LRF reported unavailable | laser interlock asserted on the R5; video continues | retry R5 every 1 s |
| NUC in progress | SPEC-04 §3.1 | last frame held, `DEGRADED:NUC` for ≤ 600 ms | none; normal | automatic |

Stale-frame rule: a frame older than 2× the frame period is never presented as live; STATUS and the overlay flag it. A black screen is a fault condition, never a default.
