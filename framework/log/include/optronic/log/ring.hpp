#pragma once

// Single-producer single-consumer ring. One of these belongs to each producing
// thread, so the only cross-thread pair is that thread and the sink thread -
// which is what makes a lock-free implementation honest rather than a race
// with good manners (SRS-LT-01).

#include "optronic/log/record.hpp"

#include <atomic>
#include <bit>
#include <cstddef>
#include <new>

namespace optronic::log {

// False sharing between the two indices would put the producer's store and the
// consumer's load on the same cache line and undo the point of the design.
inline constexpr std::size_t kCacheLine = 64;

template <std::size_t Capacity> class SpscRing {
  static_assert(std::has_single_bit(Capacity), "capacity must be a power of two: the index wrap "
                                               "is a mask, not a modulo");
  static_assert(Capacity >= 2);

public:
  [[nodiscard]] static constexpr std::size_t capacity() noexcept { return Capacity; }

  // Producer side. Returns false when the ring is full; the caller counts that
  // as a drop rather than blocking.
  //
  // The release store on write_ is paired with the acquire load in try_pop:
  // it guarantees that the slot contents written above are visible to the
  // consumer before the index that publishes them.
  [[nodiscard]] bool try_push(const Record& r) noexcept {
    const std::size_t w = write_.load(std::memory_order_relaxed);
    const std::size_t next = w + 1;

    // Acquire on read_ pairs with the consumer's release store: once we see a
    // slot freed, we also see that the consumer finished reading it.
    if (next - read_.load(std::memory_order_acquire) > Capacity)
      return false;

    slots_[w & kMask] = r;
    write_.store(next, std::memory_order_release);
    return true;
  }

  // Consumer side.
  [[nodiscard]] bool try_pop(Record& out) noexcept {
    const std::size_t rd = read_.load(std::memory_order_relaxed);
    if (rd == write_.load(std::memory_order_acquire))
      return false;

    out = slots_[rd & kMask];
    read_.store(rd + 1, std::memory_order_release);
    return true;
  }

  // Approximate by nature: both indices move while this runs. Used for
  // diagnostics only, never for a control decision.
  [[nodiscard]] std::size_t size_approx() const noexcept {
    return write_.load(std::memory_order_relaxed) - read_.load(std::memory_order_relaxed);
  }

private:
  static constexpr std::size_t kMask = Capacity - 1;

  // Indices are free-running and never wrapped; only the mask wraps. Unsigned
  // overflow is defined and the difference stays correct across it.
  alignas(kCacheLine) std::atomic<std::size_t> write_{0};
  alignas(kCacheLine) std::atomic<std::size_t> read_{0};
  alignas(kCacheLine) Record slots_[Capacity]{};
};

} // namespace optronic::log
