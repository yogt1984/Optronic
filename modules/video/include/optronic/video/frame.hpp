#pragma once

// Frames as the framework sees them: a non-owning view over memory GStreamer
// owns. No GStreamer type appears here - that is the whole point of the
// module boundary (SPEC-19 §2).

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace optronic::video {

enum class PixelFormat : std::uint8_t { unknown, nv12, i420, gray8, rgb };

using ChannelId = std::uint8_t;

// Valid only for the duration of the callback that produced it. Holding one
// past onFrame() means reading a buffer GStreamer has already recycled.
struct FrameView {
  std::span<const std::byte> data;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t stride = 0;
  PixelFormat fmt = PixelFormat::unknown;
  std::uint64_t hw_ts_ns = 0;
  std::uint64_t seq = 0;
  ChannelId ch = 0;
};

enum class FlowResult : std::uint8_t { ok, drop, stop };

// Implemented by the framework, called on a GStreamer streaming thread:
// no allocation, no locks, no exceptions (SPEC-19 §8).
struct FrameSink {
  virtual FlowResult on_frame(const FrameView&) noexcept = 0;

protected:
  ~FrameSink() = default;
};

enum class BusEventKind : std::uint8_t { error, warning, eos, state_changed, qos_drop };

struct BusEvent {
  BusEventKind kind = BusEventKind::warning;
  std::string_view element; // literal or GStreamer-owned name, valid in the call
  std::uint16_t code = 0;   // SPEC-06 code the health monitor maps on
  std::string_view text;
};

struct BusSink {
  virtual void on_bus(const BusEvent&) noexcept = 0;

protected:
  ~BusSink() = default;
};

} // namespace optronic::video
