#pragma once

// A register is a type, not a number. The offset and the access rights travel
// with it, so writing to a read-only register is a compile error rather than a
// silently ignored bus cycle at three in the morning on the test bench.

#include "optronic/hal/mmio.hpp"

#include <atomic>
#include <concepts>
#include <cstdint>

namespace optronic::hal {

struct ro_t {};
struct wo_t {};
struct rw_t {};

template <class A>
concept Readable = std::same_as<A, ro_t> || std::same_as<A, rw_t>;

template <class A>
concept Writable = std::same_as<A, wo_t> || std::same_as<A, rw_t>;

template <std::uint32_t Offset, class Access> struct Reg {
  static constexpr std::uint32_t offset = Offset;
  using access = Access;
};

// Deduces the offset from a pointer-to-member, so the register map can be
// checked against the documented address table with a static_assert.
template <class Bank, std::uint32_t Off, class Acc>
[[nodiscard]] constexpr std::uint32_t offset_of(Reg<Off, Acc> Bank::*) noexcept {
  return Off;
}

// Owns nothing: the backend outlives it by construction, because the device
// object that holds both is what the lifecycle starts and stops.
template <MmioBackend B> class RegisterFile {
public:
  explicit RegisterFile(B& backend) noexcept : backend_(&backend) {}

  // The acquire fence after the load pairs with the release fence before any
  // write below: it stops the compiler and the core from moving later reads of
  // device state above this one, which is how a status poll ends up observing
  // a value from before the command that caused it.
  template <std::uint32_t Off, class A>
    requires Readable<A>
  [[nodiscard]] std::uint32_t read(Reg<Off, A>) const noexcept {
    const std::uint32_t v = backend_->read32(Off);
    std::atomic_thread_fence(std::memory_order_acquire);
    return v;
  }

  template <std::uint32_t Off, class A>
    requires Writable<A>
  void write(Reg<Off, A>, std::uint32_t value) noexcept {
    std::atomic_thread_fence(std::memory_order_release);
    backend_->write32(Off, value);
  }

  template <std::uint32_t Off, class A>
    requires Readable<A> && Writable<A>
  void set_bits(Reg<Off, A> r, std::uint32_t mask) noexcept {
    write(r, read(r) | mask);
  }

  template <std::uint32_t Off, class A>
    requires Readable<A> && Writable<A>
  void clear_bits(Reg<Off, A> r, std::uint32_t mask) noexcept {
    write(r, read(r) & ~mask);
  }

  template <std::uint32_t Off, class A>
    requires Readable<A>
  [[nodiscard]] bool any_bit(Reg<Off, A> r, std::uint32_t mask) const noexcept {
    return (read(r) & mask) != 0u;
  }

  [[nodiscard]] B& backend() noexcept { return *backend_; }
  [[nodiscard]] const B& backend() const noexcept { return *backend_; }

private:
  B* backend_;
};

} // namespace optronic::hal
