#pragma once

// The pluggable stage. A concept rather than a virtual interface: the frame
// path calls it once per frame, so the call should inline (SPEC-11 §7).
// Processors never see a GStreamer type.

#include "optronic/video/frame.hpp"

#include <chrono>
#include <concepts>
#include <cstring>
#include <string_view>

namespace optronic::video {

enum class ProcessResult : std::uint8_t { kept, dropped, error };

// Writable destination handed to the processor; memory belongs to a buffer
// pool, so producing a frame costs no allocation.
struct WritableFrame {
  std::span<std::byte> data;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t stride = 0;
  PixelFormat fmt = PixelFormat::unknown;
};

template <class P>
concept Processor = requires(P p, const FrameView& in, WritableFrame& out) {
  { p.process(in, out) } noexcept -> std::same_as<ProcessResult>;
  { p.budget() } noexcept -> std::same_as<std::chrono::microseconds>;
  { P::name } -> std::convertible_to<std::string_view>;
};

// The identity stage. It exists to prove the seam end to end before anything
// interesting sits in it - and it is the fallback in degraded mode (SPEC-06).
class Passthrough {
public:
  static constexpr std::string_view name = "passthrough";

  ProcessResult process(const FrameView& in, WritableFrame& out) noexcept {
    if (out.data.size() < in.data.size())
      return ProcessResult::error;
    std::memcpy(out.data.data(), in.data.data(), in.data.size());
    out.width = in.width;
    out.height = in.height;
    out.stride = in.stride;
    out.fmt = in.fmt;
    return ProcessResult::kept;
  }

  std::chrono::microseconds budget() const noexcept { return std::chrono::microseconds{2000}; }
};

static_assert(Processor<Passthrough>);

} // namespace optronic::video
