#pragma once

// The PL block "ISP_CTRL" v1 as C++ (SPEC-04 §2). This header is the single
// source of truth for the address map; the table in the document is checked
// against it by the static_asserts at the bottom, so the two cannot drift
// apart silently.

#include "optronic/hal/register.hpp"

namespace optronic::hal::isp {

// clang-format off
inline constexpr Reg<0x000, ro_t> id{};
inline constexpr Reg<0x004, ro_t> version{};
inline constexpr Reg<0x008, rw_t> ctrl{};
inline constexpr Reg<0x00C, ro_t> status{};
inline constexpr Reg<0x010, rw_t> gain{};
inline constexpr Reg<0x014, rw_t> offset{};
inline constexpr Reg<0x018, rw_t> frame_w{};
inline constexpr Reg<0x01C, rw_t> frame_h{};
inline constexpr Reg<0x020, ro_t> frame_cnt{};
inline constexpr Reg<0x024, rw_t> irq_en{};
inline constexpr Reg<0x028, ro_t> irq_stat{};
inline constexpr Reg<0x02C, wo_t> irq_clr{};
inline constexpr Reg<0x030, ro_t> temp_mc{};
inline constexpr Reg<0x034, ro_t> ts_lo{};
inline constexpr Reg<0x038, ro_t> ts_hi{};
inline constexpr Reg<0x03C, rw_t> shutter{};
inline constexpr Reg<0x040, rw_t> nuc_acc_ctrl{};
inline constexpr Reg<0x100, rw_t> nuc_table_addr{};
inline constexpr Reg<0x104, rw_t> nuc_table_data{};
// clang-format on

inline constexpr std::uint32_t kIdMagic = 0x49535031u; // "ISP1"

namespace ctrl_bits {
inline constexpr std::uint32_t enable = 1u << 0;
inline constexpr std::uint32_t nuc_en = 1u << 1;
inline constexpr std::uint32_t sw_reset = 1u << 2; // self-clearing
inline constexpr std::uint32_t test_pattern = 1u << 8;
} // namespace ctrl_bits

namespace status_bits {
inline constexpr std::uint32_t running = 1u << 0;
inline constexpr std::uint32_t frame_done = 1u << 1; // sticky, W1C via IRQ_CLR
inline constexpr std::uint32_t overflow = 1u << 4;
inline constexpr std::uint32_t underflow = 1u << 5;
} // namespace status_bits

namespace shutter_bits {
inline constexpr std::uint32_t close = 1u << 0;
inline constexpr std::uint32_t busy = 1u << 4;  // read-only
inline constexpr std::uint32_t fault = 1u << 8; // read-only
} // namespace shutter_bits

namespace nuc_bits {
inline constexpr std::uint32_t start = 1u << 0;
inline constexpr std::uint32_t done = 1u << 8; // sticky, W1C
inline constexpr std::uint32_t log2n_shift = 4;
inline constexpr std::uint32_t log2n_mask = 0xFu << log2n_shift;
} // namespace nuc_bits

inline constexpr std::uint32_t kGainMask = 0x0FFFu; // Q4.8
inline constexpr std::int32_t kTempMinMilliC = -40'000;
inline constexpr std::int32_t kTempMaxMilliC = 105'000;

// The address table of SPEC-04 §2, asserted rather than trusted.
static_assert(id.offset == 0x000 && version.offset == 0x004);
static_assert(ctrl.offset == 0x008 && status.offset == 0x00C);
static_assert(gain.offset == 0x010 && offset.offset == 0x014);
static_assert(frame_w.offset == 0x018 && frame_h.offset == 0x01C);
static_assert(frame_cnt.offset == 0x020 && irq_en.offset == 0x024);
static_assert(irq_stat.offset == 0x028 && irq_clr.offset == 0x02C);
static_assert(temp_mc.offset == 0x030);
static_assert(ts_lo.offset == 0x034 && ts_hi.offset == 0x038);
static_assert(shutter.offset == 0x03C && nuc_acc_ctrl.offset == 0x040);
static_assert(nuc_table_addr.offset == 0x100 && nuc_table_data.offset == 0x104);

// Wires a FakeMmio to behave like the block: reset values, read-only
// registers, and the side effects that a plain array would not have.
void install_isp_model(FakeMmio& m);

// Power-on BIT of SPEC-04 §5. Returns the first failure rather than a bool, so
// the health monitor can say which check failed and not merely that one did.
//
// optronic::status has to be spelled out: the register named `status` below is
// in scope here and would otherwise win the lookup.
template <MmioBackend B>
[[nodiscard]] optronic::status power_on_bit(RegisterFile<B>& regs) noexcept {
  if (regs.read(id) != kIdMagic)
    return fail(Code::hal_id_mismatch, "bit: ID");

  if ((regs.read(version) >> 16) != 1u)
    return fail(Code::hal_id_mismatch, "bit: VERSION.major");

  regs.write(ctrl, ctrl_bits::sw_reset);
  if (regs.read(ctrl) != 0u)
    return fail(Code::bit_poweron, "bit: CTRL after SW_RESET");

  const auto temp = static_cast<std::int32_t>(regs.read(temp_mc));
  if (temp < kTempMinMilliC || temp > kTempMaxMilliC)
    return fail(Code::bit_poweron, "bit: TEMP_MC");

  return {};
}

// TS_HI is latched when TS_LO is read, so the order is part of the contract:
// reading HI first returns the previous frame's high word and produces a
// timestamp that jumps by 4 seconds once every 4 seconds.
template <MmioBackend B>
[[nodiscard]] std::uint64_t read_timestamp_ns(RegisterFile<B>& regs) noexcept {
  const std::uint32_t lo = regs.read(ts_lo);
  const std::uint32_t hi = regs.read(ts_hi);
  return (static_cast<std::uint64_t>(hi) << 32) | lo;
}

} // namespace optronic::hal::isp
