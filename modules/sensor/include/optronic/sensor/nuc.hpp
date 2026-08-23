#pragma once

// Non-uniformity correction: the one procedure every thermal channel has and
// no visual camera does. The detector's pixels drift apart with temperature,
// so periodically the shutter closes, the block averages N frames of a uniform
// scene, and the per-pixel offsets are recalculated (SPEC-04 §3.1).
//
// While the shutter is closed the channel cannot see. That is why this is a
// health event and not a private detail of the sensor module: the operator is
// looking at a held frame and has to know it.

#include "optronic/hal/isp_ctrl.hpp"

#include <chrono>
#include <cstdint>
#include <span>

namespace optronic::sensor {

struct NucConfig {
  std::uint32_t log2_frames = 4; // N = 16 frames averaged
  std::chrono::milliseconds shutter_timeout{150};
  std::chrono::milliseconds accumulate_timeout{400};
  std::uint32_t table_entries = 256;
};

struct NucResult {
  std::chrono::milliseconds duration{0};
  std::uint32_t entries = 0;
  std::int64_t mean_offset = 0;
};

// Every step can fail and every failure has to leave the shutter open: a unit
// that ends a failed NUC with the shutter shut is blind, which is worse than
// an uncorrected image. The caller sees one error; the recovery already
// happened.
template <hal::MmioBackend B>
[[nodiscard]] expected<NucResult> run_nuc(hal::RegisterFile<B>& regs, const NucConfig& cfg = {});

namespace detail {

// Polling rather than the frame-done interrupt: the UIO IRQ path exists in
// hal::UioRegion but cannot be exercised without the board, and a NUC that
// only works with interrupts could not be tested at all before then. The real
// implementation would wait on the IRQ and fall back to this.
template <hal::MmioBackend B, class Pred>
[[nodiscard]] bool poll_until(hal::RegisterFile<B>& regs, Pred pred,
                              std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  for (;;) {
    if (pred(regs))
      return true;
    if (std::chrono::steady_clock::now() >= deadline)
      return false;
  }
}

template <hal::MmioBackend B> void open_shutter(hal::RegisterFile<B>& regs) noexcept {
  regs.write(hal::isp::shutter, 0);
}

} // namespace detail

template <hal::MmioBackend B>
expected<NucResult> run_nuc(hal::RegisterFile<B>& regs, const NucConfig& cfg) {
  using namespace std::chrono;
  const auto started = steady_clock::now();

  if (regs.any_bit(hal::isp::shutter, hal::isp::shutter_bits::fault)) {
    return fail(Code::hal_shutter, "nuc: shutter reports FAULT");
  }

  // 1. Close the shutter and wait for it to stop moving.
  regs.write(hal::isp::shutter, hal::isp::shutter_bits::close);
  if (!detail::poll_until(
          regs, [](auto& r) { return !r.any_bit(hal::isp::shutter, hal::isp::shutter_bits::busy); },
          cfg.shutter_timeout)) {
    detail::open_shutter(regs);
    return fail(Code::hal_shutter, "nuc: shutter did not close");
  }

  // 2. Accumulate N frames of the closed-shutter scene.
  const std::uint32_t acc =
      hal::isp::nuc_bits::start |
      ((cfg.log2_frames << hal::isp::nuc_bits::log2n_shift) & hal::isp::nuc_bits::log2n_mask);
  regs.write(hal::isp::nuc_acc_ctrl, acc);
  if (!detail::poll_until(
          regs, [](auto& r) { return r.any_bit(hal::isp::nuc_acc_ctrl, hal::isp::nuc_bits::done); },
          cfg.accumulate_timeout)) {
    detail::open_shutter(regs);
    return fail(Code::timeout, "nuc: accumulation did not finish");
  }

  // 3. Read the coefficient table back. ADDR auto-increments on each DATA
  // access, so this is a loop over DATA and not an address computation per
  // element - and reading it in the wrong order silently shifts the table.
  std::int64_t sum = 0;
  regs.write(hal::isp::nuc_table_addr, 0);
  for (std::uint32_t i = 0; i < cfg.table_entries; ++i) {
    sum += static_cast<std::int64_t>(regs.read(hal::isp::nuc_table_data) & 0xFFFFu);
  }

  // 4. Open the shutter again and wait for it, so the caller returning to
  // "running" is telling the truth.
  detail::open_shutter(regs);
  if (!detail::poll_until(
          regs, [](auto& r) { return !r.any_bit(hal::isp::shutter, hal::isp::shutter_bits::busy); },
          cfg.shutter_timeout)) {
    return fail(Code::hal_shutter, "nuc: shutter did not reopen");
  }

  NucResult result;
  result.duration = duration_cast<milliseconds>(steady_clock::now() - started);
  result.entries = cfg.table_entries;
  result.mean_offset = cfg.table_entries > 0 ? sum / cfg.table_entries : 0;
  return result;
}

} // namespace optronic::sensor
