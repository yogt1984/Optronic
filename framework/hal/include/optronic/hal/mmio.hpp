#pragma once

// The seam between register code and the thing behind it. Everything above
// this line is ordinary testable C++; `volatile` and /dev/uio live below it and
// nowhere else in the tree (SPEC-11 §6).

#include "optronic/core/expected.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace optronic::hal {

// Compile-time polymorphism rather than a virtual interface: a register access
// is a few instructions, and a vtable dispatch per access would dominate it.
template <class B>
concept MmioBackend = requires(B b, const B cb, std::size_t off, std::uint32_t v) {
  { cb.read32(off) } noexcept -> std::same_as<std::uint32_t>;
  { b.write32(off, v) } noexcept -> std::same_as<void>;
  { B::size } -> std::convertible_to<std::size_t>;
};

// Host stand-in for the AXI4-Lite window. It is not just an array: the point
// of a fake is to reproduce the side effects that make hardware surprising -
// self-clearing bits, write-1-to-clear, read-only enforcement - so that code
// which only works against a dumb array fails here instead of on the unit.
class FakeMmio {
public:
  static constexpr std::size_t size = 0x1000; // 4 KiB, as the device tree node
  static constexpr std::size_t word_count = size / 4;

  FakeMmio() : words_(word_count, 0) {}

  [[nodiscard]] std::uint32_t read32(std::size_t off) const noexcept {
    if (!aligned_and_inside(off)) {
      ++faults_;
      return 0;
    }
    // Reads can have effects on real hardware - a status bit that clears on
    // read, a shutter that has finished moving since the last poll - so the
    // fake allows them too. The state is mutable for exactly this reason: the
    // device changes underneath a const reader, which is the truth.
    for (const auto& h : read_hooks_) {
      if (h.offset == off)
        h.fn(const_cast<FakeMmio&>(*this));
    }
    return words_[off / 4];
  }

  void write32(std::size_t off, std::uint32_t v) noexcept {
    if (!aligned_and_inside(off)) {
      ++faults_;
      return;
    }
    if (read_only_.size() > off / 4 && read_only_[off / 4]) {
      ++faults_; // hardware ignores it; a test wants to know it happened
      return;
    }
    words_[off / 4] = v;
    for (const auto& h : hooks_) {
      if (h.offset == off)
        h.fn(*this, v);
    }
  }

  // Side effect on write, e.g. CTRL.SW_RESET clearing itself. Registered by
  // whoever models the device, not by the backend.
  void on_write(std::size_t off, std::function<void(FakeMmio&, std::uint32_t)> fn) {
    hooks_.push_back({off, std::move(fn)});
  }

  // Side effect on read: called before the value is returned, so the hook can
  // change what the reader is about to see.
  void on_read(std::size_t off, std::function<void(FakeMmio&)> fn) {
    read_hooks_.push_back({off, std::move(fn)});
  }

  void set_read_only(std::size_t off) {
    if (read_only_.size() < word_count)
      read_only_.assign(word_count, false);
    if (off / 4 < word_count)
      read_only_[off / 4] = true;
  }

  // Test-side access that bypasses read-only and hooks: this is the hardware
  // changing the register, not software writing it.
  void poke(std::size_t off, std::uint32_t v) noexcept {
    if (aligned_and_inside(off))
      words_[off / 4] = v;
  }

  [[nodiscard]] std::uint32_t peek(std::size_t off) const noexcept {
    return aligned_and_inside(off) ? words_[off / 4] : 0;
  }

  // Misaligned or out-of-range accesses. SPEC-04 §3 calls them undefined on
  // the real bus, so the fake counts them instead of guessing.
  [[nodiscard]] std::size_t faults() const noexcept { return faults_; }

private:
  static constexpr bool aligned_and_inside(std::size_t off) noexcept {
    return (off % 4 == 0) && (off + 4 <= size);
  }

  struct Hook {
    std::size_t offset;
    std::function<void(FakeMmio&, std::uint32_t)> fn;
  };

  struct ReadHook {
    std::size_t offset;
    std::function<void(FakeMmio&)> fn;
  };

  std::vector<std::uint32_t> words_;
  std::vector<bool> read_only_;
  std::vector<Hook> hooks_;
  std::vector<ReadHook> read_hooks_;
  mutable std::size_t faults_ = 0;
};

static_assert(MmioBackend<FakeMmio>);

} // namespace optronic::hal
