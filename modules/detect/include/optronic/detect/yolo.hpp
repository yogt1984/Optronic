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
  std::string_view label; // into the loaded class list; valid while Yolo lives
};

// Which export the weights are in. Deduced from the file name rather than
// asked for, because getting it wrong is not a choice anybody makes on purpose.
enum class Format { darknet, onnx };

struct YoloConfig {
  // An ONNX export by default: more accurate, and its raw output has to be
  // decoded here rather than by OpenCV, which is the interesting half.
  std::string weights = "models/yolov5s.onnx";
  std::string config = "models/yolov4-tiny.cfg"; // darknet only
  std::string names = "models/coco.names";
  // The network's input, not the camera's. Inference cost scales with this
  // and barely at all with capture resolution, so this is the knob that makes
  // the detector quicker - at the price of missing small or distant objects.
  int input_size = 640;
  float confidence_threshold = 0.4F;
  float nms_threshold = 0.4F;
  // Inference on its own thread, with a one-slot mailbox: the streaming thread
  // copies the luma plane in, draws the boxes it already has, and returns.
  // Running inference inline blocks capture for its whole duration - at 300 ms
  // against a 33 ms frame period that is a visible stall every few frames
  // (SPEC-19 §4).
  bool async = true;

  // Only used when async is off. Zero means work it out from measured latency.
  int detect_every = 0;
  std::chrono::microseconds frame_period{33'333}; // 30 fps
  // OpenCV's pool competes with the pipeline's threads and buys almost nothing
  // here - measured 1002 ms against 1020 ms for yolov5s - so it stays off.
  int threads = 1;
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

  // How many frames the stage is currently skipping between inferences.
  [[nodiscard]] int stride() const noexcept;

  // Which model actually got loaded, for the log line that would otherwise
  // claim whatever was hardcoded into it.
  [[nodiscard]] std::string_view model() const noexcept;

private:
  class Impl;
  explicit Yolo(std::unique_ptr<Impl>) noexcept;
  std::unique_ptr<Impl> impl_;
};

static_assert(video::Processor<Yolo>);

} // namespace optronic::detect
