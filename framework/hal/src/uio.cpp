#include "optronic/hal/uio.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>

#include <utility>

namespace optronic::hal {
namespace {

// The one place `volatile` is correct: it tells the compiler that these loads
// and stores have effects it cannot see and may not fold, reorder or elide.
// It says nothing about atomicity or about ordering against other memory -
// the fences in RegisterFile do that.
volatile std::uint32_t* word_at(void* base, std::size_t off) noexcept {
  return reinterpret_cast<volatile std::uint32_t*>(static_cast<std::byte*>(base) + off);
}

} // namespace

expected<UioRegion> UioRegion::open(const char* device, std::size_t length) {
  const int fd = ::open(device, O_RDWR | O_SYNC | O_CLOEXEC);
  if (fd < 0)
    return fail(Code::hal_open, "UioRegion::open");

  void* base = ::mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (base == MAP_FAILED) {
    ::close(fd);
    return fail(Code::hal_map, "UioRegion::mmap");
  }
  return UioRegion{fd, base, length};
}

UioRegion::UioRegion(UioRegion&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)), base_(std::exchange(other.base_, nullptr)),
      length_(std::exchange(other.length_, 0)) {}

UioRegion& UioRegion::operator=(UioRegion&& other) noexcept {
  if (this != &other) {
    this->~UioRegion();
    fd_ = std::exchange(other.fd_, -1);
    base_ = std::exchange(other.base_, nullptr);
    length_ = std::exchange(other.length_, 0);
  }
  return *this;
}

UioRegion::~UioRegion() {
  if (base_ != nullptr)
    ::munmap(base_, length_);
  if (fd_ >= 0)
    ::close(fd_);
  base_ = nullptr;
  fd_ = -1;
}

std::uint32_t UioRegion::read32(std::size_t off) const noexcept {
  if (base_ == nullptr || off % 4 != 0 || off + 4 > length_)
    return 0;
  return *word_at(base_, off);
}

void UioRegion::write32(std::size_t off, std::uint32_t value) noexcept {
  if (base_ == nullptr || off % 4 != 0 || off + 4 > length_)
    return;
  *word_at(base_, off) = value;
}

// The count the kernel returns is cumulative, so a caller can tell a missed
// interrupt from a repeated one.
expected<std::uint32_t> UioRegion::wait_irq(std::chrono::milliseconds timeout) {
  if (fd_ < 0)
    return fail(Code::hal_open, "UioRegion::wait_irq");

  pollfd pfd{fd_, POLLIN, 0};
  const int n = ::poll(&pfd, 1, static_cast<int>(timeout.count()));
  if (n == 0)
    return fail(Code::hal_irq_timeout, "UioRegion::wait_irq");
  if (n < 0)
    return fail(Code::io, "UioRegion::poll");

  std::uint32_t count = 0;
  if (::read(fd_, &count, sizeof(count)) != static_cast<ssize_t>(sizeof(count))) {
    return fail(Code::io, "UioRegion::read");
  }
  return count;
}

status UioRegion::enable_irq() {
  if (fd_ < 0)
    return fail(Code::hal_open, "UioRegion::enable_irq");
  const std::uint32_t one = 1;
  if (::write(fd_, &one, sizeof(one)) != static_cast<ssize_t>(sizeof(one))) {
    return fail(Code::io, "UioRegion::enable_irq");
  }
  return {};
}

} // namespace optronic::hal
