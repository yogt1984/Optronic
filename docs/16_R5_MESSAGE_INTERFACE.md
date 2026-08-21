# SPEC-16 Linux ↔ R5 (RPU) message interface

## 1. Purpose
The R5 owns hard real-time functions: gimbal stabilisation loop, laser rangefinder trigger and timing, safety interlocks (laser arm, shutter vs laser), power sequencing. Linux (APU) owns everything else and must command and observe the R5 without timing coupling.

## 2. Transport
- Primary: **RPMsg over OpenAMP** (`/dev/rpmsg0`, virtio vrings in reserved DDR, IPI doorbells). Endpoint `optronic-rt`, MTU 496 B.
- Fallback (external MCU variant): UART 921600 8N1, COBS framing + CRC-16. Same message layer.
- `hal::RtChannel` interface: `send(Msg)`, `receive(timeout)`, `stop_token`-aware. Implementations: `RpmsgChannel`, `UartChannel`, `FakeRtChannel` (host tests).

## 3. Message layer
Header, 8 bytes packed: `u8 version=1 · u8 type · u16 len · u16 seq · u16 crc16` (CRC over header + payload). Little-endian. All payloads are fixed-size structs with `static_assert(sizeof)`.

| Type | Dir | Payload | Period / reply |
|---|---|---|---|
| 0x01 HEARTBEAT | both | `u32 uptime_ms, u8 state, u8 faults` | 100 ms each way; 3 missed → link lost |
| 0x02 VERSION_REQ / 0x82 VERSION | A→R / R→A | — / `u8 major, minor, patch; char git[8]` | once at start; major mismatch → FAULT |
| 0x10 GIMBAL_CMD | A→R | `i32 az_mdeg, i32 el_mdeg, u16 rate_mdeg_s, u8 mode (0 hold, 1 slew, 2 track, 3 stow)` | on change, ≤ 50 Hz |
| 0x90 GIMBAL_STATE | R→A | `i32 az, el; i16 rate_az, rate_el; u8 mode; u8 flags (stab_ok, limit, motor_fault)` | 100 Hz |
| 0x20 LRF_ARM | A→R | `u8 arm, u32 token` | reply 0xA0 LRF_STATE |
| 0x21 LRF_FIRE | A→R | `u32 token` | reply 0xA1 LRF_RESULT `{u32 range_dm, u8 quality, u64 ts_ns}` within 200 ms |
| 0xA0 LRF_STATE | R→A | `u8 armed, u8 interlock_bits (shutter_closed, temp, cover, no_token)` | on change |
| 0x30 SHUTTER_REQ | A→R | `u8 close` | reply 0xB0 SHUTTER_ACK — refused while LRF armed (interlock) |
| 0x40 POWER_CMD | A→R | `u8 rail_mask, u8 on` | reply 0xC0 POWER_STATE |
| 0xF0 FAULT | R→A | `u16 code, u32 detail` | on event |
| 0xFF NACK | R→A | `u16 seq_of_bad, u8 reason` | on bad message |

## 4. Semantics
- The APU never assumes timing: commands are requests; truth is the R5 state message.
- Interlocks live on the R5 only (laser cannot fire with shutter closed, cover on, or without a fresh token). The APU side is UI/ICD and logging.
- Tokens: `LRF_ARM` returns a token valid for 5 s; `LRF_FIRE` without a valid token is NACKed.
- Loss of the R5 heartbeat → health event `RT_LINK_LOST` → gimbal/LRF reported unavailable on the ICD; video unaffected (SPEC-06 §6).
- R5 firmware update (not in v1): APU writes the image to shared memory, sends `FW_UPDATE` (0x50); R5 verifies the signature and reboots via remoteproc.

## 5. Host testing
`FakeRtChannel` with scripted responses. Tests: heartbeat-loss detection, token expiry, NACK handling, struct sizes, CRC. On QEMU RPMsg is unavailable → UART loopback variant.
