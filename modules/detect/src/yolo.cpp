#include "optronic/detect/yolo.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <mutex>
#include <opencv2/core.hpp>
#include <opencv2/dnn.hpp>
#include <opencv2/imgproc.hpp>
#include <thread>

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

  ~Impl() { stop_worker(); }

  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;

  static Format format_of(const std::string& path) {
    return path.size() > 5 && path.compare(path.size() - 5, 5, ".onnx") == 0 ? Format::onnx
                                                                             : Format::darknet;
  }

  status load() {
    format_ = format_of(cfg_.weights);

    // A darknet model is two files. Whichever one the caller named, derive the
    // other: asking for the pair separately is a way to get them mismatched.
    if (format_ == Format::darknet) {
      const auto ext = cfg_.weights.rfind('.');
      if (ext != std::string::npos && cfg_.weights.compare(ext, 4, ".cfg") == 0) {
        cfg_.config = cfg_.weights;
        cfg_.weights = cfg_.weights.substr(0, ext) + ".weights";
      } else if (ext != std::string::npos) {
        cfg_.config = cfg_.weights.substr(0, ext) + ".cfg";
      }
      if (cfg_.input_size == YoloConfig{}.input_size)
        cfg_.input_size = 416; // what tiny-yolo was trained at, unless asked otherwise
    }

    std::ifstream names{cfg_.names};
    if (!names)
      return fail(Code::io, "yolo: cannot read the class names");
    for (std::string line; std::getline(names, line);)
      if (!line.empty())
        classes_.push_back(line);

    try {
      net_ = format_ == Format::onnx ? cv::dnn::readNetFromONNX(cfg_.weights)
                                     : cv::dnn::readNetFromDarknet(cfg_.config, cfg_.weights);
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

    // One thread, deliberately. This runs on a GStreamer streaming thread, and
    // OpenCV's own pool would compete with the pipeline's threads for the same
    // cores while making the frame time unpredictable - the opposite of what a
    // declared budget is for. It is also why ThreadSanitizer can see through
    // this module at all: an uninstrumented pool inside a library reports
    // races that are not ours and hides any that are.
    cv::setNumThreads(cfg_.threads);

    // Only the darknet export goes through DetectionModel; the ONNX one is
    // decoded by hand below, because DetectionModel cannot read its layout.
    if (format_ == Format::darknet) {
      model_ = std::make_unique<cv::dnn::DetectionModel>(net_);
      model_->setInputParams(1.0 / 255.0, cv::Size{cfg_.input_size, cfg_.input_size}, cv::Scalar{},
                             true);
    }
    return {};
  }

  video::ProcessResult process(const video::FrameView& in, video::WritableFrame& out) noexcept {
    if (in.data.empty() || out.data.empty())
      return video::ProcessResult::error;

    const auto w = static_cast<int>(in.width);
    const auto h = static_cast<int>(in.height);
    ++frame_;

    if (cfg_.async) {
      offer(in, w, h);
    } else if (frame_ % static_cast<std::uint64_t>(stride()) == 0) {
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

  // YOLOv5 emits one tensor of [1, 25200, 85]: per candidate, four box values
  // in input-space centre form, one objectness score, then a score per class.
  // Nothing about that is standardised, so it is decoded here rather than by
  // the library - and it is why the darknet path exists alongside, as a
  // reference the decode can be checked against.
  void decode_v5(const cv::Mat& out, int img_w, int img_h, std::vector<int>& ids,
                 std::vector<float>& scores, std::vector<cv::Rect>& boxes) const {
    const int rows = out.size[1];
    const int stride = out.size[2];
    const auto* p = reinterpret_cast<const float*>(out.data);

    // The blob was a plain resize, not a letterbox, so the mapping back is one
    // scale factor per axis. It costs aspect fidelity and buys a decode with
    // no padding arithmetic to get wrong.
    const float sx = static_cast<float>(img_w) / static_cast<float>(cfg_.input_size);
    const float sy = static_cast<float>(img_h) / static_cast<float>(cfg_.input_size);

    for (int i = 0; i < rows; ++i, p += stride) {
      const float objectness = p[4];
      if (objectness < cfg_.confidence_threshold)
        continue;

      const cv::Mat class_scores{1, stride - 5, CV_32FC1, const_cast<float*>(p + 5)};
      cv::Point best;
      double best_score = 0.0;
      cv::minMaxLoc(class_scores, nullptr, &best_score, nullptr, &best);

      const auto confidence = static_cast<float>(best_score) * objectness;
      if (confidence < cfg_.confidence_threshold)
        continue;

      const float cx = p[0] * sx, cy = p[1] * sy;
      const float bw = p[2] * sx, bh = p[3] * sy;
      ids.push_back(best.x);
      scores.push_back(confidence);
      boxes.emplace_back(static_cast<int>(cx - bw / 2), static_cast<int>(cy - bh / 2),
                         static_cast<int>(bw), static_cast<int>(bh));
    }
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
      if (format_ == Format::darknet) {
        model_->detect(bgr, ids, scores, boxes, cfg_.confidence_threshold, cfg_.nms_threshold);
      } else {
        const cv::Mat blob =
            cv::dnn::blobFromImage(bgr, 1.0 / 255.0, cv::Size{cfg_.input_size, cfg_.input_size},
                                   cv::Scalar{}, true, false);
        net_.setInput(blob);
        const cv::Mat raw = net_.forward();

        std::vector<int> raw_ids;
        std::vector<float> raw_scores;
        std::vector<cv::Rect> raw_boxes;
        decode_v5(raw, w, h, raw_ids, raw_scores, raw_boxes);

        // Overlapping candidates are the norm, not the exception: without
        // suppression one object arrives as a dozen boxes.
        std::vector<int> keep;
        cv::dnn::NMSBoxes(raw_boxes, raw_scores, cfg_.confidence_threshold, cfg_.nms_threshold,
                          keep);
        for (int k : keep) {
          ids.push_back(raw_ids[static_cast<std::size_t>(k)]);
          scores.push_back(raw_scores[static_cast<std::size_t>(k)]);
          boxes.push_back(raw_boxes[static_cast<std::size_t>(k)]);
        }
      }
      const auto dt = std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - t0);

      std::vector<Detection> found;
      found.reserve(ids.size());
      for (std::size_t i = 0; i < ids.size() && i < boxes.size(); ++i) {
        const auto id = static_cast<std::size_t>(ids[i]);
        found.push_back(
            {ids[i], scores[i], boxes[i].x, boxes[i].y, boxes[i].width, boxes[i].height,
             id < classes_.size() ? std::string_view{classes_[id]} : std::string_view{"?"}});
      }

      const std::scoped_lock lock{mutex_};
      last_ = std::move(found);
      inference_us_.store(dt.count(), std::memory_order_relaxed);
    } catch (const cv::Exception&) {
      // A frame that OpenCV refuses is a dropped detection, never a crash on
      // the streaming thread.
    }
  }

  // Streaming thread. Copies the luma plane into the mailbox and returns; if
  // the worker is still busy the frame is simply not offered, which is what
  // one slot means - always the newest, never a queue that grows.
  void offer(const video::FrameView& in, int w, int h) noexcept {
    if (!worker_.joinable())
      start_worker();

    const std::size_t need = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    const std::unique_lock lock{mailbox_mutex_, std::try_to_lock};
    if (!lock.owns_lock() || pending_)
      return; // worker still has the last one

    if (mailbox_.size() != need)
      mailbox_.resize(need); // once, then never again for this geometry
    const std::size_t stride = in.stride != 0 ? in.stride : static_cast<std::size_t>(w);
    for (int row = 0; row < h; ++row) {
      const auto* src = in.data.data() + static_cast<std::size_t>(row) * stride;
      std::memcpy(mailbox_.data() + static_cast<std::size_t>(row) * static_cast<std::size_t>(w),
                  src, static_cast<std::size_t>(w));
    }
    mailbox_w_ = w;
    mailbox_h_ = h;
    pending_ = true;
    mailbox_cv_.notify_one();
  }

  void start_worker() {
    worker_ = std::jthread{[this](std::stop_token stop) {
      std::vector<std::byte> frame;
      int w = 0, h = 0;
      while (!stop.stop_requested()) {
        {
          std::unique_lock lock{mailbox_mutex_};
          mailbox_cv_.wait_for(lock, std::chrono::milliseconds{100},
                               [&] { return pending_ || stop.stop_requested(); });
          if (stop.stop_requested())
            return;
          if (!pending_)
            continue;
          frame = mailbox_;
          w = mailbox_w_;
          h = mailbox_h_;
        }

        video::FrameView v{};
        v.data = frame;
        v.width = static_cast<std::uint32_t>(w);
        v.height = static_cast<std::uint32_t>(h);
        v.stride = static_cast<std::uint32_t>(w);
        detect(v, w, h);

        {
          const std::scoped_lock lock{mailbox_mutex_};
          pending_ = false;
        }
      }
    }};
  }

  void stop_worker() noexcept {
    if (!worker_.joinable())
      return;
    worker_.request_stop();
    mailbox_cv_.notify_all();
    worker_.join();
  }

  [[nodiscard]] std::vector<Detection> last() const {
    const std::scoped_lock lock{mutex_};
    return last_;
  }

  [[nodiscard]] std::int64_t inference_us() const noexcept {
    return inference_us_.load(std::memory_order_relaxed);
  }

  // One inference every N frames, where N is what the last one cost. Before
  // anything has been measured it runs on the next frame; after that it paces
  // itself, so a slow model degrades the detection rate rather than the frame
  // rate.
  [[nodiscard]] int stride() const noexcept {
    if (cfg_.detect_every > 0)
      return cfg_.detect_every;
    const std::int64_t us = inference_us_.load(std::memory_order_relaxed);
    if (us <= 0)
      return 1;
    const auto period = cfg_.frame_period.count();
    return static_cast<int>(std::max<std::int64_t>(1, (us + period - 1) / period));
  }

  [[nodiscard]] std::string_view model() const noexcept { return cfg_.weights; }

  [[nodiscard]] const std::vector<std::string>& classes() const noexcept { return classes_; }

private:
  YoloConfig cfg_;
  Format format_ = Format::darknet;
  cv::dnn::Net net_;
  std::unique_ptr<cv::dnn::DetectionModel> model_;
  std::vector<std::string> classes_;

  mutable std::mutex mutex_;
  std::vector<Detection> last_;

  // The one-slot mailbox between the streaming thread and the worker.
  std::mutex mailbox_mutex_;
  std::condition_variable mailbox_cv_;
  std::vector<std::byte> mailbox_;
  int mailbox_w_ = 0, mailbox_h_ = 0;
  bool pending_ = false;
  std::jthread worker_;
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

int Yolo::stride() const noexcept {
  return impl_->stride();
}

std::string_view Yolo::model() const noexcept {
  return impl_->model();
}

} // namespace optronic::detect
