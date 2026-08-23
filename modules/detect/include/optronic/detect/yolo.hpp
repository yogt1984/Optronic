#pragma once

// A detection stage that satisfies video::Processor. It sees a FrameView and a
// WritableFrame and no GStreamer type at all - which is the point of the
// concept: the thing doing the interesting work does not need to know how the
// frames arrive.
//
// On the target this is where a DPU or the VCU-adjacent accelerator would sit.
// Here it is OpenCV's DNN module on the CPU, which is slow enough to make the
// budget question real rather than theoretical.

#include "optronic/core/expected.hpp"
#include "optronic/video/frame.hpp"
#include "optronic/video/processor.hpp"

#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace optronic::detect {

struct Detection {
  int class_id = 0;
  float confidence = 0.0F;
  int x = 0, y = 0, w = 0, h = 0;
};

struct YoloConfig {
  std::string config = "models/yolov4-tiny.cfg";
  std::string weights = "models/yolov4-tiny.weights";
  std::string names = "models/coco.names";
  int input_size = 416;
  float confidence_threshold = 0.4F;
  float nms_threshold = 0.4F;
  // Detection is far slower than a frame period on a CPU. Rather than drop
  // frames, the detector runs on every Nth and the boxes from the last run are
  // drawn on the frames in between - which is what a tracker would do anyway.
  int detect_every = 3;
};

class Yolo {
public:
  static constexpr std::string_view name = "yolo";

  [[nodiscard]] static expected<Yolo> create(const YoloConfig& = {});

  Yolo(Yolo&&) noexcept;
  Yolo& operator=(Yolo&&) noexcept;
  Yolo(const Yolo&) = delete;
  Yolo& operator=(const Yolo&) = delete;
  ~Yolo();

  // video::Processor. noexcept because it runs on a GStreamer streaming
  // thread, where an escaping exception would unwind through C.
  video::ProcessResult process(const video::FrameView& in, video::WritableFrame& out) noexcept;

  [[nodiscard]] std::chrono::microseconds budget() const noexcept;

  [[nodiscard]] std::vector<Detection> last() const;
  [[nodiscard]] std::chrono::microseconds last_inference() const noexcept;

private:
  class Impl;
  explicit Yolo(std::unique_ptr<Impl>) noexcept;
  std::unique_ptr<Impl> impl_;
};

static_assert(video::Processor<Yolo>);

} // namespace optronic::detect
