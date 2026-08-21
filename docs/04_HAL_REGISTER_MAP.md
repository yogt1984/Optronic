# SPEC-04 HAL Register Map — PL block "ISP_CTRL" v1

## 1. Mapping
Device tree node `isp_ctrl@a0000000` (AXI4-Lite, 4 KiB), bound to `uio_pdrv_genirq`; appears as `/dev/uio0`. Interrupt line PL→PS IRQ0 (frame done). On the host the same map is backed by `hal::FakeMmio` (4 KiB `std::array<std::byte>` with side-effect hooks).

## 2. Registers (32-bit, offsets from base)
| Offset | Name | Access | Reset | Bits |
|---|---|---|---|---|
| 0x000 | ID | RO | 0x4953_5031 ("ISP1") | magic |
| 0x004 | VERSION | RO | 0x0001_0000 | [31:16] major, [15:0] minor |
| 0x008 | CTRL | RW | 0x0 | [0] ENABLE, [1] NUC_EN, [2] SW_RESET (self-clear), [8] TEST_PATTERN |
| 0x00C | STATUS | RO | 0x0 | [0] RUNNING, [1] FRAME_DONE (sticky, W1C via IRQ_CLR), [4] OVERFLOW, [5] UNDERFLOW |
| 0x010 | GAIN | RW | 0x0100 | [11:0] gain Q4.8 |
| 0x014 | OFFSET | RW | 0x0 | [15:0] signed offset |
| 0x018 | FRAME_W | RW | 1920 | [15:0] |
| 0x01C | FRAME_H | RW | 1080 | [15:0] |
| 0x020 | FRAME_CNT | RO | 0 | [31:0] frames processed |
| 0x024 | IRQ_EN | RW | 0x0 | [0] FRAME_DONE, [4] OVERFLOW |
| 0x028 | IRQ_STAT | RO | 0x0 | mirrors STATUS sticky bits |
| 0x02C | IRQ_CLR | WO | — | write-1-to-clear |
| 0x030 | TEMP_MC | RO | — | die temperature in milli-°C (signed) |
| 0x034 | TS_LO | RO | 0 | [31:0] timestamp of last FRAME_DONE, ns, low word (PTP-disciplined PL counter) |
| 0x038 | TS_HI | RO | 0 | [31:0] high word; read LO then HI (HI latched on LO read) |
| 0x03C | SHUTTER | RW | 0x0 | [0] CLOSE (1 = closed), [4] BUSY (RO), [8] FAULT (RO) |
| 0x040 | NUC_ACC_CTRL | RW | 0x0 | [0] START accumulation, [7:4] log2(N frames), [8] DONE (sticky, W1C) |
| 0x100 | NUC_TABLE_ADDR | RW | 0 | [15:0] index |
| 0x104 | NUC_TABLE_DATA | RW | 0 | [15:0] coefficient (auto-increment ADDR) |

## 3. Access rules
- All accesses 32-bit aligned; byte/halfword access is undefined.
- Writes to CTRL while RUNNING take effect at next frame boundary.
- Sequence to change resolution: CTRL.ENABLE=0 → wait STATUS.RUNNING=0 (≤ 2 frame periods) → FRAME_W/H → CTRL.ENABLE=1.
- Read of STATUS after IRQ: read IRQ_STAT, then write IRQ_CLR, then re-enable UIO irq (write 1 to fd).
- Memory ordering: `Register::write` issues a store with `std::atomic_thread_fence(release)` before; `read` with acquire after. No `volatile` outside `hal/`.

### 3.1 NUC (shutter) sequence — the one optronics-specific sequence every thermal channel has
1. `SHUTTER.CLOSE = 1` → poll `SHUTTER.BUSY == 0` (≤ 150 ms) else `E_HAL_SHUTTER`.
2. `NUC_ACC_CTRL = START | log2(N)` (N = 16 typical) → wait for IRQ or poll `DONE` (≤ N frame periods + 20 %).
3. Read the accumulated offsets via `NUC_TABLE_ADDR/DATA` (auto-increment), compute the new offset table on the APU (or let the PL apply it directly when `CTRL.NUC_EN`), write it back.
4. `SHUTTER.CLOSE = 0` → poll `BUSY == 0`.
5. Emit `EVENT NUC_DONE{duration_ms, mean_offset}`; health marks the channel `OK` again (it is `DEGRADED:NUC` while the shutter is closed — the operator sees the last frame held, flagged).
Triggered by operator command, timer (every 5–15 min) or temperature delta (`TEMP_MC` changed > 2 °C since the last NUC). Whole sequence ≤ 600 ms, and it must never run during a laser-ranging window (interlock with the R5, SPEC-16).

### 3.2 Timestamps
`TS_LO/HI` latch the PL counter at FRAME_DONE; the counter is disciplined to the PS PTP clock, or free-running with an offset measured at start. The video module copies the value into the `GstBuffer` PTS and frame meta so every frame carries its hardware capture time (SPEC-12).

## 4. C++ representation
```cpp
struct IspCtrl : hal::RegisterBank<0x1000> {
  Reg<0x000, ro_t> id;      Reg<0x004, ro_t> version;
  Reg<0x008, rw_t> ctrl;    Reg<0x00C, ro_t> status;
  Reg<0x010, rw_t> gain;    /* ... */
};
static_assert(IspCtrl::offset_of(&IspCtrl::gain) == 0x010);
```
Layout is the single source of truth; this document is generated from it (`tools/gen_regmap_md`).

## 5. BIT hooks
Power-on BIT: ID == "ISP1", VERSION.major == 1, SW_RESET then CTRL == 0, TEMP_MC within −40 000..+105 000. Continuous BIT: FRAME_CNT advances ≥ 1 per 2 s while ENABLE; OVERFLOW bit → DEGRADED event.
