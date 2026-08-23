// Graph description -> gst-launch text. No GStreamer here on purpose: this is
// the part that must be testable without a runtime, a display or hardware.

#include "optronic/video/spec.hpp"

#include <format>

namespace optronic::video {
namespace {

std::string_view source_element(SourceKind k) {
  switch (k) {
  case SourceKind::v4l2:
    return "v4l2src";
  case SourceKind::test_pattern:
    break;
  }
  return "videotestsrc";
}

// A leaky queue drops the oldest buffer; a blocking one would push the stall
// upstream into the capture thread, which is worse than losing a frame.
std::string queue(const QueueSpec& q) {
  return std::format("queue leaky={} max-size-buffers={} max-size-bytes=0 max-size-time=0",
                     q.leaky ? "downstream" : "no", q.max_buffers);
}

} // namespace

std::string_view format_string(PixelFormat f) {
  switch (f) {
  case PixelFormat::nv12:
    return "NV12";
  case PixelFormat::i420:
    return "I420";
  case PixelFormat::gray8:
    return "GRAY8";
  case PixelFormat::rgb:
    return "RGB";
  case PixelFormat::unknown:
    break;
  }
  return "NV12";
}

std::string_view encoder_element(EncoderKind k) {
  switch (k) {
  case EncoderKind::h264_omx:
    return "omxh264enc";
  case EncoderKind::h264_vcu:
    return "vvas_xvcuenc";
  case EncoderKind::h264_sw:
    break;
  }
  return "x264enc";
}

namespace {

std::string encoder_chain(const EncoderSpec& e) {
  const std::string_view element = encoder_element(e.kind);
  switch (e.kind) {
  case EncoderKind::h264_sw:
    // x264enc takes kbit/s directly; zerolatency disables lookahead and
    // B-frames, which is what costs frames in a sight.
    return std::format("{} bitrate={} key-int-max={}{}", element, e.bitrate_kbps, e.gop,
                       e.low_latency ? " tune=zerolatency speed-preset=ultrafast" : "");
  case EncoderKind::h264_omx:
  case EncoderKind::h264_vcu:
    // The hardware encoders take bit/s, not kbit/s - a unit mismatch here is
    // a factor of 1000 in bitrate and looks like a broken encoder.
    return std::format("{} target-bitrate={} gop-length={}{}", element, e.bitrate_kbps * 1000u,
                       e.gop, e.low_latency ? " control-rate=low-latency" : "");
  }
  return std::string{element};
}

// async=false on every delivery sink is load-bearing, not tidying. The
// delivery chain starts empty because its appsrc is fed by the capture chain,
// so a sink that waits to preroll holds the whole pipeline in PAUSED - and the
// capture chain can never run to produce the buffer it is waiting for.
std::string output_chain(const OutputSpec& o) {
  switch (o.kind) {
  case OutputKind::rtp_udp:
    return std::format(
        "rtph264pay config-interval=1 pt=96 ! udpsink host={} port={} sync=false async=false",
        o.host, o.port);
  case OutputKind::file:
    return std::format("mp4mux ! filesink location={} async=false", o.path);
  case OutputKind::none:
    break;
  }
  return "fakesink sync=false async=false";
}

} // namespace

std::string launch_string(const PipelineSpec& s) {
  const std::string caps =
      std::format("video/x-raw,format={},width={},height={},framerate={}/1",
                  format_string(s.source.fmt), s.source.width, s.source.height, s.source.fps);

  // Two chains in one pipeline, bridged in C++ by the processor. One pipeline
  // means one bus and one state machine for both halves.
  const std::string capture = std::format(
      "{}{} ! {} ! {} ! appsink name=out sync=false max-buffers=1 drop=true emit-signals=false",
      source_element(s.source.kind),
      s.source.kind == SourceKind::v4l2 ? std::format(" device={}", s.source.device)
                                        : std::string{" is-live=true"},
      caps, queue(s.queues));

  const std::string deliver = std::format(
      "appsrc name=in is-live=true format=time block=false caps={} ! {} ! {} ! h264parse ! {}",
      caps, queue(s.queues), encoder_chain(s.encoder), output_chain(s.output));

  return capture + "  " + deliver;
}

} // namespace optronic::video
