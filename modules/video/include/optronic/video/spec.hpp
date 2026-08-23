#pragma once

// A graph described as data. The description is deliberately separate from the
// construction: launch_string() is pure text, so the graph a given config
// produces can be golden-file tested on any host, with no GStreamer and no
// hardware (SPEC-19 §6).

#include "optronic/video/frame.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace optronic::video {

enum class SourceKind : std::uint8_t { test_pattern, v4l2 };

// The encoder is a configuration choice, not an #ifdef: the same binary picks
// the software encoder on a laptop and the VCU on the target.
enum class EncoderKind : std::uint8_t { h264_sw, h264_omx, h264_vcu };

enum class OutputKind : std::uint8_t { rtp_udp, file, none };

struct SourceSpec {
  SourceKind kind = SourceKind::test_pattern;
  std::string device = "/dev/video0"; // v4l2 only
  std::uint32_t width = 1280;
  std::uint32_t height = 720;
  std::uint32_t fps = 30;
  PixelFormat fmt = PixelFormat::nv12;
  // videotestsrc pattern: 0 = SMPTE bars, 18 = a moving ball. A moving image
  // shows at a glance that frames are live and not one held picture.
  std::uint32_t pattern = 0;
};

struct EncoderSpec {
  EncoderKind kind = EncoderKind::h264_sw;
  std::uint32_t bitrate_kbps = 4000;
  std::uint32_t gop = 30;
  bool low_latency = true;
};

struct OutputSpec {
  OutputKind kind = OutputKind::rtp_udp;
  std::string host = "127.0.0.1";
  std::uint16_t port = 5600;
  std::string path; // file only
};

// Bounded and leaky on purpose: under overload the pipeline drops the oldest
// frame instead of growing latency without limit (SPEC-13).
struct QueueSpec {
  bool leaky = true;
  std::uint32_t max_buffers = 4;
};

struct PipelineSpec {
  ChannelId channel = 0;
  SourceSpec source;
  std::string processor = "passthrough";
  EncoderSpec encoder;
  OutputSpec output;
  QueueSpec queues;
};

// Deterministic: same spec in, same string out, byte for byte.
[[nodiscard]] std::string launch_string(const PipelineSpec&);

[[nodiscard]] std::string_view encoder_element(EncoderKind);
[[nodiscard]] std::string_view format_string(PixelFormat);

} // namespace optronic::video
