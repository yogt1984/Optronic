#include "optronic/detect/yolo.hpp"

#include <atomic>
#include <fstream>
#include <mutex>
#include <opencv2/dnn.hpp>
#include <opencv2/imgproc.hpp>

namespace optronic::detect {
namespace {

// NV12 is what the pipeline carries: a full-resolution luma plane followed by
// interleaved chroma at half resolution. Boxes are drawn into the luma plane
// only - a white outline needs no colour, and touching just one plane keeps
// the drawing trivially correct.
constexpr std::uint8_t kBoxLuma = 235; // studio-swing white

void draw_rect(std::span<std::byte> plane, std::uint32_t stride, std::uint32_t height, int x, int y,
               int w, int h) noexcept {
  const auto put = [&](int px, int py) {
    if (px < 0 || py < 0 || static_cast<std::uint32_t>(px) >= stride ||
        static_cast<std::uint32_t>(py) >= height)
      return;
    const std::size_t i = static_cast<std::size_t>(py) * stride + static_cast<std::size_t>(px);
    if (i < plane.size())
      plane[i] = static_cast<std::byte>(kBoxLuma);
  };
  constexpr int t = 2; // line thickness, in pixels
  for (int i = 0; i < w; ++i)
    for (int k = 0; k < t; ++k) {
      put(x + i, y + k);
      put(x + i, y + h - 1 - k);
    }
  for (int j = 0; j < h; ++j)
    for (int k = 0; k < t; ++k) {
      put(x + k, y + j);
      put(x + w - 1 - k, y + j);
    }
}

} // namespace

class Yolo::Impl {
public:
  explicit Impl(YoloConfig cfg) : cfg_(std::move(cfg)) {}

  status load() {
    std::ifstream names{cfg_.names};
    if (!names)
      return fail(Code::io, "yolo: cannot read the class names");
    for (std::string line; std::getline(names, line);)
      if (!line.empty())
        classes_.push_back(line);

    try {
      net_ = cv::dnn::readNetFromDarknet(cfg_.config, cfg_.weights);
    } catch (const cv::Exception&) {
      // The only place an exception is caught: OpenCV throws on a missing or
      // malformed model, and this has to become an Error before it reaches
      // anything that runs on a streaming thread.
      return fail(Code::io, "yolo: cannot read the model");
    }
    if (net_.empty())
      return fail(Code::io, "yolo: model loaded empty");

    net_.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
    net_.setPreferableTarget(cv::dnn::DNN_TARGET_CPU);

    model_ = std::make_unique<cv::dnn::DetectionModel>(net_);
    model_->setInputParams(1.0 / 255.0, cv::Size{cfg_.input_size, cfg_.input_size}, cv::Scalar{},
                           true);
    return {};
  }

  video::ProcessResult process(const video::FrameView& in, video::WritableFrame& out) noexcept {
    if (in.data.empty() || out.data.empty())
      return video::ProcessResult::error;

    const auto w = static_cast<int>(in.width);
    const auto h = static_cast<int>(in.height);

    if (++frame_ % static_cast<std::uint64_t>(cfg_.detect_every) == 0) {
      detect(in, w, h);
    }

    // Boxes from the most recent inference are drawn on every frame, so the
    // overlay does not flicker at one third of the frame rate.
    const std::scoped_lock lock{mutex_};
    for (const Detection& d : last_) {
      draw_rect(out.data, out.stride != 0 ? out.stride : in.width, in.height, d.x, d.y, d.w, d.h);
    }
    return video::ProcessResult::kept;
  }

  void detect(const video::FrameView& in, int w, int h) noexcept {
    try {
      // The luma plane alone is a valid greyscale image, so detection runs on
      // it directly: no colour conversion of a 1.4 MB frame per inference.
      const cv::Mat luma{h, w, CV_8UC1, const_cast<void*>(static_cast<const void*>(in.data.data())),
                         in.stride != 0 ? in.stride : static_cast<std::size_t>(w)};
      cv::Mat bgr;
      cv::cvtColor(luma, bgr, cv::COLOR_GRAY2BGR);

      std::vector<int> ids;
      std::vector<float> scores;
      std::vector<cv::Rect> boxes;

      const auto t0 = std::chrono::steady_clock::now();
      model_->detect(bgr, ids, scores, boxes, cfg_.confidence_threshold, cfg_.nms_threshold);
      const auto dt = std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - t0);

      std::vector<Detection> found;
      found.reserve(ids.size());
      for (std::size_t i = 0; i < ids.size() && i < boxes.size(); ++i) {
        found.push_back(
            {ids[i], scores[i], boxes[i].x, boxes[i].y, boxes[i].width, boxes[i].height});
      }

      const std::scoped_lock lock{mutex_};
      last_ = std::move(found);
      inference_us_.store(dt.count(), std::memory_order_relaxed);
    } catch (const cv::Exception&) {
      // A frame that OpenCV refuses is a dropped detection, never a crash on
      // the streaming thread.
    }
  }

  [[nodiscard]] std::vector<Detection> last() const {
    const std::scoped_lock lock{mutex_};
    return last_;
  }

  [[nodiscard]] std::int64_t inference_us() const noexcept {
    return inference_us_.load(std::memory_order_relaxed);
  }

  [[nodiscard]] const std::vector<std::string>& classes() const noexcept { return classes_; }

private:
  YoloConfig cfg_;
  cv::dnn::Net net_;
  std::unique_ptr<cv::dnn::DetectionModel> model_;
  std::vector<std::string> classes_;

  mutable std::mutex mutex_;
  std::vector<Detection> last_;
  std::atomic<std::int64_t> inference_us_{0};
  std::uint64_t frame_ = 0;
};

expected<Yolo> Yolo::create(const YoloConfig& cfg) {
  auto impl = std::make_unique<Impl>(cfg);
  if (const status s = impl->load(); !s)
    return std::unexpected(s.error());
  return Yolo{std::move(impl)};
}

Yolo::Yolo(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Yolo::Yolo(Yolo&&) noexcept = default;
Yolo& Yolo::operator=(Yolo&&) noexcept = default;
Yolo::~Yolo() = default;

video::ProcessResult Yolo::process(const video::FrameView& in, video::WritableFrame& out) noexcept {
  return impl_->process(in, out);
}

// Honest rather than aspirational: tiny-YOLO on a CPU is tens of milliseconds,
// which is why detect_every exists.
std::chrono::microseconds Yolo::budget() const noexcept {
  return std::chrono::microseconds{40'000};
}

std::vector<Detection> Yolo::last() const {
  return impl_->last();
}

std::chrono::microseconds Yolo::last_inference() const noexcept {
  return std::chrono::microseconds{impl_->inference_us()};
}

} // namespace optronic::detect
