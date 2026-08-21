# SPEC-03 Interface Control Document — Control Protocol v1.0

## 1. Transport
TCP, single client, port from config (`ipc.port`, default 5600). Little-endian. Framing: `u32 length` (payload bytes, excl. header) `u16 type` `u16 seq` `payload[length]`. Max payload 64 KiB.

## 2. Versioning
First message after connect is HELLO from client with `u8 major, u8 minor`. Server replies HELLO_ACK with its version. Major mismatch → ERROR(E_PROTO_VERSION) and close. Minor differences: unknown fields ignored, unknown types → ERROR(E_UNSUPPORTED).

## 3. Message types
| Type | Dir | Payload | Reply |
|---|---|---|---|
| 0x0001 HELLO | C→S | `u8 major, u8 minor, char client_id[32]` | 0x8001 HELLO_ACK |
| 0x0002 GET_STATUS | C→S | — | 0x8002 STATUS |
| 0x0003 SET_PARAM | C→S | `u16 param_id, u16 type, value[]` | 0x8000 ACK / 0x8FFF ERROR |
| 0x0004 GET_PARAM | C→S | `u16 param_id` | 0x8004 PARAM |
| 0x0005 RESTART_PIPELINE | C→S | — | 0x8000 ACK (accepted) then 0x9002 EVENT |
| 0x0006 RUN_BIT | C→S | `u8 kind (0 poweron,1 continuous)` | 0x8006 BIT_RESULT |
| 0x0007 INJECT_FAULT (debug builds only) | C→S | `u16 fault_id` | ACK |
| 0x9001 HEARTBEAT | S→C | `u64 mono_ns, u8 health_state` | — |
| 0x9002 EVENT | S→C | `u16 event_id, u64 mono_ns, u16 code, char text[64]` | — |

## 4. STATUS payload
`u8 health_state (0 INIT,1 OK,2 DEGRADED,3 FAULT)`, `u8 pipeline_state (0 NULL,1 READY,2 PAUSED,3 PLAYING)`, `u32 frames_total`, `u32 frames_dropped`, `u16 lat_capture_p95_us`, `u16 lat_proc_p95_us`, `u16 lat_encode_p95_us`, `u32 uptime_s`, `u16 active_faults`.

## 5. Parameter IDs (runtime-settable, see SPEC-05)
| ID | Name | Type | Range |
|---|---|---|---|
| 0x0100 | sensor.gain | u16 | 0..4095 |
| 0x0101 | sensor.nuc_enable | u8 | 0/1 |
| 0x0200 | video.bitrate_kbps | u32 | 500..20000 |
| 0x0201 | video.leaky_queue | u8 | 0/1 |
| 0x0300 | telemetry.period_ms | u32 | 100..60000 |

## 6. Error codes
Reuse SPEC-06 codes; protocol-specific: E_PROTO_VERSION=0x0101, E_UNSUPPORTED=0x0102, E_BAD_LENGTH=0x0103, E_BAD_PARAM=0x0104, E_OUT_OF_RANGE=0x0105, E_BUSY=0x0106.

## 7. Timing
Reply ≤ 100 ms. HEARTBEAT every 1000 ± 50 ms. Client declares link down after 3 missed heartbeats.

## 8. Compatibility
Fields are appended only; never reordered. Struct packing `#pragma pack(push,1)` with `static_assert(sizeof)` on every message in `ipc/messages.hpp`.
