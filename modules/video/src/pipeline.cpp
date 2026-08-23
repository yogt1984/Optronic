#include "optronic/video/pipeline.hpp"

#include "gst_ptr.hpp"

#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>

#include <atomic>
#include <mutex>
#include <thread>

namespace optronic::video {
namespace {

// gst_init is process-global; calling it twice is harmless but racing it is
// not, and a unit test binary creates several pipelines.
void ensure_gst_initialised() {
  static std::once_flag once;
  std::call_once(once, [] { gst_init(nullptr, nullptr); });
}

PixelFormat format_from_caps(GstCaps* caps) noexcept {
  if (caps == nullptr || gst_caps_get_size(caps) == 0)
    return PixelFormat::unknown;
  const GstStructure* st = gst_caps_get_structure(caps, 0);
  const char* fmt = gst_structure_get_string(st, "format");
  if (fmt == nullptr)
    return PixelFormat::unknown;
  if (g_str_equal(fmt, "NV12"))
    return PixelFormat::nv12;
  if (g_str_equal(fmt, "I420"))
    return PixelFormat::i420;
  if (g_str_equal(fmt, "GRAY8"))
    return PixelFormat::gray8;
  if (g_str_equal(fmt, "RGB"))
    return PixelFormat::rgb;
  return PixelFormat::unknown;
}

} // namespace

class Pipeline::Impl {
public:
  Impl(GstPtr<GstElement> pipeline, GstElement* appsink, GstElement* appsrc, PipelineSpec spec,
       FrameSink& frames, BusSink& bus, Pipeline::Transform transform)
      : pipeline_(std::move(pipeline)), appsink_(appsink), appsrc_(appsrc), spec_(std::move(spec)),
        frames_(frames), bus_(bus), transform_(std::move(transform)) {}

  ~Impl() { shutdown(); }

  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;
  Impl(Impl&&) = delete;
  Impl& operator=(Impl&&) = delete;

  void install_callbacks() noexcept {
    GstAppSinkCallbacks cb{};
    cb.new_sample = &Impl::on_new_sample_trampoline;
    gst_app_sink_set_callbacks(GST_APP_SINK(appsink_), &cb, this, nullptr);
  }

  status play() {
    const GstStateChangeReturn r = gst_element_set_state(pipeline_.get(), GST_STATE_PLAYING);
    if (r == GST_STATE_CHANGE_FAILURE)
      return fail(Code::vid_state, "Pipeline::play");

    // ASYNC is normal for a live source; wait so that a caller that gets ok()
    // back really has a running graph.
    GstState state{};
    if (gst_element_get_state(pipeline_.get(), &state, nullptr, 3 * GST_SECOND) ==
        GST_STATE_CHANGE_FAILURE) {
      return fail(Code::vid_state, "Pipeline::play");
    }

    start_bus_thread();
    return {};
  }

  status stop() {
    shutdown();
    return {};
  }

  PipelineStats stats() const noexcept {
    return {frames_in_.load(std::memory_order_relaxed), frames_out_.load(std::memory_order_relaxed),
            drops_.load(std::memory_order_relaxed)};
  }

private:
  void start_bus_thread() {
    if (bus_thread_.joinable())
      return;
    GstPtr<GstBus> bus{gst_element_get_bus(pipeline_.get())};
    bus_thread_ = std::jthread{[this, b = std::move(bus)](std::stop_token stop) {
      constexpr GstMessageType kWanted = static_cast<GstMessageType>(
          GST_MESSAGE_ERROR | GST_MESSAGE_WARNING | GST_MESSAGE_EOS | GST_MESSAGE_QOS);
      while (!stop.stop_requested()) {
        GstPtr<GstMessage> msg{gst_bus_timed_pop_filtered(b.get(), 100 * GST_MSECOND, kWanted)};
        if (!msg)
          continue;
        dispatch(msg.get());
      }
    }};
  }

  // Runs on the bus thread. It only reports; changing pipeline state from here
  // would deadlock against the state change that produced the message.
  void dispatch(GstMessage* msg) noexcept {
    BusEvent ev{};
    const char* src = GST_OBJECT_NAME(GST_MESSAGE_SRC(msg));
    ev.element = src != nullptr ? src : "";

    switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_ERROR: {
      GError* err = nullptr;
      gst_message_parse_error(msg, &err, nullptr);
      ev.kind = BusEventKind::error;
      // Which element failed decides the health event: the encoder dying is
      // recoverable by rebuilding, the source dying is not (SPEC-06 §3).
      ev.code = static_cast<std::uint16_t>(ev.element.find("enc") != std::string_view::npos
                                               ? Code::vid_encoder_lost
                                               : Code::vid_build);
      ev.text = err != nullptr && err->message != nullptr ? err->message : "";
      bus_.on_bus(ev);
      if (err != nullptr)
        g_error_free(err);
      return;
    }
    case GST_MESSAGE_WARNING:
      ev.kind = BusEventKind::warning;
      break;
    case GST_MESSAGE_EOS:
      ev.kind = BusEventKind::eos;
      break;
    case GST_MESSAGE_QOS:
      ev.kind = BusEventKind::qos_drop;
      drops_.fetch_add(1, std::memory_order_relaxed);
      break;
    default:
      return;
    }
    bus_.on_bus(ev);
  }

  static GstFlowReturn on_new_sample_trampoline(GstAppSink* sink, gpointer user) noexcept {
    return static_cast<Impl*>(user)->on_new_sample(sink);
  }

  // Streaming thread. No allocation, no locks, and no exception may leave this
  // frame - it would unwind through C (SPEC-11 §3, §5).
  GstFlowReturn on_new_sample(GstAppSink* sink) noexcept {
    GstPtr<GstSample> sample{gst_app_sink_pull_sample(sink)};
    if (!sample)
      return GST_FLOW_EOS;

    GstBuffer* buffer = gst_sample_get_buffer(sample.get());
    if (buffer == nullptr)
      return GST_FLOW_OK;

    // Read-only unless a transform is installed: mapping writable on a shared
    // buffer forces a copy, and the passthrough case must not pay for a
    // feature it does not use.
    const GstMapFlags flags =
        transform_ ? static_cast<GstMapFlags>(GST_MAP_READ | GST_MAP_WRITE) : GST_MAP_READ;
    if (transform_)
      buffer = gst_buffer_make_writable(buffer);

    const MapGuard map{buffer, flags};
    if (!map.ok())
      return GST_FLOW_OK;

    FrameView view{};
    view.data = map.bytes();
    view.width = spec_.source.width;
    view.height = spec_.source.height;
    view.stride = spec_.source.width;
    view.fmt = format_from_caps(gst_sample_get_caps(sample.get()));
    view.hw_ts_ns = GST_BUFFER_PTS_IS_VALID(buffer) ? GST_BUFFER_PTS(buffer) : 0;
    view.seq = frames_in_.fetch_add(1, std::memory_order_relaxed);
    view.ch = spec_.channel;

    if (transform_) {
      WritableFrame out{};
      out.data = map.writable_bytes();
      out.width = view.width;
      out.height = view.height;
      out.stride = view.stride;
      out.fmt = view.fmt;
      if (transform_(view, out) == ProcessResult::error) {
        drops_.fetch_add(1, std::memory_order_relaxed);
      }
    }

    const FlowResult res = frames_.on_frame(view);
    if (res == FlowResult::stop)
      return GST_FLOW_EOS;
    if (res == FlowResult::drop) {
      drops_.fetch_add(1, std::memory_order_relaxed);
      return GST_FLOW_OK;
    }

    // Passthrough refs the input buffer instead of copying it: the delivery
    // half of the graph gets the same memory. A processor that writes output
    // needs a GstBufferPool - that arrives with the processor runner, because
    // allocating per frame here would break the no-allocation rule.
    if (appsrc_ != nullptr) {
      gst_buffer_ref(buffer);
      if (gst_app_src_push_buffer(GST_APP_SRC(appsrc_), buffer) != GST_FLOW_OK) {
        drops_.fetch_add(1, std::memory_order_relaxed);
        return GST_FLOW_OK;
      }
      frames_out_.fetch_add(1, std::memory_order_relaxed);
    }
    return GST_FLOW_OK;
  }

  void shutdown() noexcept {
    if (bus_thread_.joinable()) {
      bus_thread_.request_stop();
      bus_thread_.join();
    }
    if (pipeline_) {
      // Detach the callback first: a sample arriving mid-teardown would call
      // into members that are already gone.
      if (appsink_ != nullptr) {
        GstAppSinkCallbacks none{};
        gst_app_sink_set_callbacks(GST_APP_SINK(appsink_), &none, nullptr, nullptr);
      }
      gst_element_set_state(pipeline_.get(), GST_STATE_NULL);
    }
  }

  GstPtr<GstElement> pipeline_;
  GstElement* appsink_ = nullptr; // owned by the pipeline, borrowed here
  GstElement* appsrc_ = nullptr;
  PipelineSpec spec_;
  FrameSink& frames_;
  BusSink& bus_;
  Pipeline::Transform transform_;
  std::atomic<std::uint64_t> frames_in_{0};
  std::atomic<std::uint64_t> frames_out_{0};
  std::atomic<std::uint64_t> drops_{0};
  std::jthread bus_thread_;
};

expected<Pipeline> Pipeline::create(const PipelineSpec& spec, FrameSink& frames, BusSink& bus,
                                    Transform transform) {
  ensure_gst_initialised();

  const std::string desc = launch_string(spec);
  GError* err = nullptr;
  GstPtr<GstElement> pipeline{gst_parse_launch(desc.c_str(), &err)};
  if (!pipeline || err != nullptr) {
    if (err != nullptr)
      g_error_free(err);
    return fail(Code::vid_build, "Pipeline::create");
  }

  // gst_bin_get_by_name returns a ref; the elements stay alive with the
  // pipeline, so the extra ref is handed straight back.
  GstPtr<GstElement> sink{gst_bin_get_by_name(GST_BIN(pipeline.get()), "out")};
  GstPtr<GstElement> src{gst_bin_get_by_name(GST_BIN(pipeline.get()), "in")};
  if (!sink)
    return fail(Code::vid_build, "Pipeline::create");

  auto impl = std::make_unique<Impl>(std::move(pipeline), sink.get(), src.get(), spec, frames, bus,
                                     std::move(transform));
  impl->install_callbacks();
  return Pipeline{std::move(impl)};
}

Pipeline::Pipeline(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Pipeline::Pipeline(Pipeline&&) noexcept = default;
Pipeline& Pipeline::operator=(Pipeline&&) noexcept = default;
Pipeline::~Pipeline() = default;

status Pipeline::play() {
  return impl_->play();
}
status Pipeline::stop() {
  return impl_->stop();
}
PipelineStats Pipeline::stats() const noexcept {
  return impl_->stats();
}

} // namespace optronic::video
