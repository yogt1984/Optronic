#pragma once

// The real backend: /dev/uioN mapped into the process. This is the only place
// in the tree where `volatile` is allowed, and the only part that cannot be
// tested without the board - which is exactly why everything above it talks to
// the MmioBackend concept instead of to this class (SPEC-09 L1/L3).

#include "optronic/core/expected.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace optronic::hal {

class UioRegion {
public:
  static constexpr std::size_t size = 0x1000;

  // "/dev/uio0" and the mapped length; the length is checked against what the
  // kernel reports in /sys/class/uio/uioN/maps/map0/size.
  [[nodiscard]] static expected<UioRegion> open(const char* device, std::size_t length = size);

  UioRegion(UioRegion&&) noexcept;
  UioRegion& operator=(UioRegion&&) noexcept;
  UioRegion(const UioRegion&) = delete;
  UioRegion& operator=(const UioRegion&) = delete;
  ~UioRegion();

  [[nodiscard]] std::uint32_t read32(std::size_t off) const noexcept;
  void write32(std::size_t off, std::uint32_t value) noexcept;

  // Blocks until the PL raises its interrupt. UIO delivers it as a read of
  // four bytes from the device fd; re-arming is a write of 1 to the same fd,
  // which is what enable_irq does.
  [[nodiscard]] expected<std::uint32_t> wait_irq(std::chrono::milliseconds timeout);
  [[nodiscard]] status enable_irq();

private:
  UioRegion(int fd, void* base, std::size_t length) noexcept
      : fd_(fd), base_(base), length_(length) {}

  int fd_ = -1;
  void* base_ = nullptr;
  std::size_t length_ = 0;
};

} // namespace optronic::hal
