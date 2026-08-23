// optronic - sensor node service.
//
// The whole program is three components in a declared order: logging, the
// sensor behind the HAL, then video. Nothing here does any work itself; it
// wires the pieces together and hands control to the lifecycle, which is the
// point of having a framework at all (SPEC-01 §1, SPEC-02).

#include "optronic/app/app.hpp"
#include "optronic/hal/isp_ctrl.hpp"
#include "optronic/log/logger.hpp"
#include "optronic/sensor/nuc.hpp"

#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <optional>
#include <string_view>
#include <thread>

#if OPTRONIC_WITH_VIDEO
#include "optronic/video/pipeline.hpp"
#endif
#if OPTRONIC_WITH_TELEMETRY
#include "optronic/telemetry/publisher.hpp"
#endif

namespace {

using namespace optronic;

struct Options {
  std::uint32_t gain = 0x0100;
  std::uint32_t width = 1280;
  std::uint32_t height = 720;
  std::uint32_t fps = 30;
  std::string host = "127.0.0.1";
  std::uint16_t port = 5600;
  bool stream = false; // default off: a demo should not need a receiver
  bool video = true;   // --no-video: run the service without a pipeline
  int nuc_after = 0;   // --nuc N: run a NUC N seconds after start (0 = never)
  std::string broker;  // empty = telemetry off
  std::string node = "node1";
  int seconds = 0; // 0 = run until SIGTERM
  log::Level level = log::Level::info;
};

bool parse_u32(std::string_view s, std::uint32_t& out) {
  const auto* end = s.data() + s.size();
  return std::from_chars(s.data(), end, out).ptr == end;
}

// Deliberately not a config file yet: framework/config is specified and not
// implemented, and inventing half of it here would be worse than a flag.
bool parse_args(int argc, char** argv, Options& o) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view a{argv[i]};
    const auto next = [&]() -> std::string_view {
      return (i + 1 < argc) ? std::string_view{argv[++i]} : std::string_view{};
    };

    // Each option gets its own block. Folding the parse into the condition
    // (`a == "--gain" && !parse(...)`) reads well and is wrong: on a
    // successful parse the condition is false and the argument falls through
    // to the unknown-option branch below.
    if (a == "--gain") {
      if (!parse_u32(next(), o.gain))
        return false;
    } else if (a == "--width") {
      if (!parse_u32(next(), o.width) || o.width == 0)
        return false;
    } else if (a == "--height") {
      if (!parse_u32(next(), o.height) || o.height == 0)
        return false;
    } else if (a == "--fps") {
      if (!parse_u32(next(), o.fps) || o.fps == 0)
        return false;
    } else if (a == "--host") {
      o.host = next();
      if (o.host.empty())
        return false;
      o.stream = true;
    } else if (a == "--port") {
      std::uint32_t p = 0;
      if (!parse_u32(next(), p) || p == 0 || p > 65535)
        return false;
      o.port = static_cast<std::uint16_t>(p);
      o.stream = true;
    } else if (a == "--stream") {
      o.stream = true;
    } else if (a == "--no-video") {
      o.video = false;
    } else if (a == "--nuc") {
      std::uint32_t sec = 0;
      if (!parse_u32(next(), sec) || sec == 0)
        return false;
      o.nuc_after = static_cast<int>(sec);
    } else if (a == "--broker") {
      o.broker = next();
      if (o.broker.empty())
        return false;
    } else if (a == "--node") {
      o.node = next();
      if (o.node.empty())
        return false;
    } else if (a == "--seconds") {
      std::uint32_t sec = 0;
      if (!parse_u32(next(), sec))
        return false;
      o.seconds = static_cast<int>(sec);
    } else if (a == "--debug") {
      o.level = log::Level::debug;
    } else if (a == "--quiet") {
      o.level = log::Level::warn;
    } else {
      return false; // --help included: it prints the usage and exits
    }
  }
  return true;
}

void usage() {
  std::fputs("usage: optronic [--gain N] [--width N] [--height N] [--fps N]\n"
             "                [--stream] [--host H] [--port P] [--seconds N]\n"
             "                [--broker HOST] [--node NAME] [--no-video] [--nuc N]\n"
             "                [--debug|--quiet]\n"
             "\n"
             "Without --stream the encoded frames are discarded, so the service\n"
             "runs anywhere. With it, receive them e.g. with\n"
             "  gst-launch-1.0 udpsrc port=5600 ! application/x-rtp,encoding-name=H264 \\\n"
             "    ! rtph264depay ! avdec_h264 ! autovideosink\n"
             "\n"
             "--broker enables MQTT telemetry; watch it with\n"
             "  mosquitto_sub -v -t 'optronic/#'\n",
             stderr);
}

// Owns the sink thread. First up and last down, so every other component can
// log during its own startup and shutdown.
class LogService final : public app::Component {
public:
  explicit LogService(log::Logger& logger) noexcept : logger_(logger) {}

  std::string_view name() const noexcept override { return "log"; }
  status init() override { return {}; }

  status start() override {
    logger_.start();
    logger_.log(log::Level::info, "log", "sink thread running");
    return {};
  }

  void stop() noexcept override {
    const log::Stats s = logger_.stats();
    logger_.log(log::Level::info, "log", "sink stopping",
                std::array{log::KeyValue::of("written", static_cast<std::int64_t>(s.written)),
                           log::KeyValue::of("dropped", static_cast<std::int64_t>(s.dropped))});
    logger_.stop();
  }

private:
  log::Logger& logger_;
};

// The PL block behind the HAL. On a laptop that is FakeMmio with the ISP model
// installed; on the target the same code runs against UioRegion, because both
// satisfy MmioBackend and nothing above this line knows the difference.
class SensorService final : public app::Component {
public:
  // Something that wants to know about events as they happen, rather than by
  // sampling. A 200 ms NUC is invisible to a 1 Hz telemetry sample, so state
  // that changes faster than the sample period has to be pushed, not polled
  // (SPEC-07 §4: the event topic, QoS 1).
  using EventSink =
      std::function<void(std::string_view id, std::uint16_t code, std::string_view text)>;

  SensorService(log::Logger& logger, const Options& opt) noexcept : logger_(logger), opt_(opt) {}

  void on_event(EventSink sink) { events_ = std::move(sink); }

  std::string_view name() const noexcept override { return "sensor"; }

  status init() override {
    hal::isp::install_isp_model(dev_);

    if (const status s = hal::isp::power_on_bit(regs_); !s) {
      logger_.log(log::Level::error, "sensor", "power-on BIT failed",
                  std::array{log::KeyValue::of("code", static_cast<std::int64_t>(s.error().code))});
      return s;
    }
    logger_.log(log::Level::info, "sensor", "power-on BIT passed");
    return {};
  }

  status start() override {
    // SPEC-04 §3: resolution only changes while the block is stopped.
    regs_.clear_bits(hal::isp::ctrl, hal::isp::ctrl_bits::enable);
    regs_.write(hal::isp::frame_w, opt_.width);
    regs_.write(hal::isp::frame_h, opt_.height);
    regs_.write(hal::isp::gain, opt_.gain & hal::isp::kGainMask);
    regs_.set_bits(hal::isp::ctrl, hal::isp::ctrl_bits::enable);

    if (!regs_.any_bit(hal::isp::status, hal::isp::status_bits::running)) {
      return fail(Code::not_ready, "SensorService::start");
    }

    logger_.log(log::Level::info, "sensor", "running",
                std::array{log::KeyValue::of("gain", static_cast<std::int64_t>(opt_.gain)),
                           log::KeyValue::of("w", static_cast<std::int64_t>(opt_.width)),
                           log::KeyValue::of("h", static_cast<std::int64_t>(opt_.height))});
    return {};
  }

  void stop() noexcept override {
    regs_.clear_bits(hal::isp::ctrl, hal::isp::ctrl_bits::enable);
    logger_.log(log::Level::info, "sensor", "stopped");
  }

  [[nodiscard]] std::int64_t temperature_mc() noexcept {
    return static_cast<std::int32_t>(regs_.read(hal::isp::temp_mc));
  }

  [[nodiscard]] std::int64_t gain() noexcept { return regs_.read(hal::isp::gain); }

  [[nodiscard]] std::uint64_t frame_count() noexcept { return regs_.read(hal::isp::frame_cnt); }

  [[nodiscard]] bool nuc_running() const noexcept { return nuc_.load(std::memory_order_relaxed); }

  // The channel is blind while the shutter is shut, so this is announced
  // before and after, not only when it finishes (SPEC-06: DEGRADED:NUC).
  void run_nuc() noexcept {
    nuc_.store(true, std::memory_order_relaxed);
    logger_.log(log::Level::warn, "sensor", "NUC started - channel DEGRADED, shutter closing");
    emit("NUC_STARTED", 0, "shutter closing, channel degraded");

    const auto r = sensor::run_nuc(regs_);

    nuc_.store(false, std::memory_order_relaxed);
    if (!r) {
      logger_.log(log::Level::error, "sensor", "NUC failed",
                  std::array{log::KeyValue::of("code", static_cast<std::int64_t>(r.error().code))});
      emit("NUC_FAILED", static_cast<std::uint16_t>(r.error().code), r.error().where);
      return;
    }
    logger_.log(log::Level::info, "sensor", "NUC done - channel OK",
                std::array{log::KeyValue::of("ms", static_cast<std::int64_t>(r->duration.count())),
                           log::KeyValue::of("mean_offset", r->mean_offset)});
    emit("NUC_DONE", 0, "channel OK");
  }

private:
  void emit(std::string_view id, std::uint16_t code, std::string_view text) noexcept {
    if (events_)
      events_(id, code, text);
  }

  log::Logger& logger_;
  const Options& opt_;
  EventSink events_;
  hal::FakeMmio dev_;
  hal::RegisterFile<hal::FakeMmio> regs_{dev_};
  std::atomic<bool> nuc_{false};
};

#if OPTRONIC_WITH_VIDEO
// Bridges the pipeline to the framework: frames and bus events arrive on
// GStreamer threads and leave as log records, which is the only thing that may
// happen there - no allocation, no locks, no exceptions (SPEC-19 §8).
class VideoService final : public app::Component, private video::FrameSink, private video::BusSink {
public:
  VideoService(log::Logger& logger, const Options& opt) noexcept : logger_(logger), opt_(opt) {}

  std::string_view name() const noexcept override { return "video"; }

  status init() override {
    video::PipelineSpec spec;
    spec.source.width = opt_.width;
    spec.source.height = opt_.height;
    spec.source.fps = opt_.fps;
    spec.output.kind = opt_.stream ? video::OutputKind::rtp_udp : video::OutputKind::none;
    spec.output.host = opt_.host;
    spec.output.port = opt_.port;

    logger_.log(log::Level::debug, "video", video::launch_string(spec).c_str());

    auto pipe = video::Pipeline::create(spec, *this, *this);
    if (!pipe)
      return std::unexpected(pipe.error());
    pipeline_.emplace(std::move(*pipe));
    return {};
  }

  status start() override {
    if (const status s = pipeline_->play(); !s)
      return s;
    logger_.log(log::Level::info, "video", "pipeline PLAYING",
                std::array{log::KeyValue::of("fps", static_cast<std::int64_t>(opt_.fps)),
                           log::KeyValue::of("stream", static_cast<std::int64_t>(opt_.stream))});
    return {};
  }

  [[nodiscard]] std::uint64_t frames() const noexcept {
    return pipeline_ ? pipeline_->stats().frames_in : 0;
  }

  void stop() noexcept override {
    if (!pipeline_)
      return;
    const video::PipelineStats st = pipeline_->stats();
    (void)pipeline_->stop();
    logger_.log(log::Level::info, "video", "pipeline stopped",
                std::array{log::KeyValue::of("frames", static_cast<std::int64_t>(st.frames_in)),
                           log::KeyValue::of("drops", static_cast<std::int64_t>(st.drops))});
    pipeline_.reset();
  }

private:
  video::FlowResult on_frame(const video::FrameView& f) noexcept override {
    // One line per second rather than per frame: the ring would absorb 30/s
    // happily, but a human reading the demo would not.
    if (f.seq % opt_.fps == 0) {
      logger_.log(log::Level::info, "video", "frame",
                  std::array{log::KeyValue::of("seq", static_cast<std::int64_t>(f.seq)),
                             log::KeyValue::of("bytes", static_cast<std::int64_t>(f.data.size()))});
    }
    return video::FlowResult::ok;
  }

  void on_bus(const video::BusEvent& e) noexcept override {
    if (e.kind != video::BusEventKind::error)
      return;
    logger_.log(log::Level::error, "video", "bus error",
                std::array{log::KeyValue::of("code", static_cast<std::int64_t>(e.code))});
  }

  log::Logger& logger_;
  const Options& opt_;
  std::optional<video::Pipeline> pipeline_;
};
#endif

#if OPTRONIC_WITH_TELEMETRY
// Publishes on its own thread at a fixed period. It reads other components
// through a sampler function rather than holding references to them, so
// telemetry depends on nothing and can be removed without touching them - the
// direction of the dependency is the whole design (SRS-LT-04).
class TelemetryService final : public app::Component {
public:
  using Sampler = std::function<telemetry::SensorSample()>;

  TelemetryService(log::Logger& logger, const Options& opt, Sampler sampler)
      : logger_(logger), opt_(opt), sampler_(std::move(sampler)) {}

  std::string_view name() const noexcept override { return "telemetry"; }

  status init() override {
    telemetry::Config cfg;
    cfg.name = opt_.node;
    cfg.host = opt_.broker;
    auto pub = telemetry::Publisher::create(cfg, logger_);
    if (!pub)
      return std::unexpected(pub.error());
    pub_.emplace(std::move(*pub));
    return {};
  }

  status start() override {
    if (const status s = pub_->start(); !s)
      return s;
    started_ = std::chrono::steady_clock::now();

    worker_ = std::jthread{[this](std::stop_token stop) {
      while (!stop.stop_requested()) {
        const auto up = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - started_);
        pub_->publish_health({"OK", static_cast<std::uint64_t>(up.count()), 0});
        pub_->publish_sensor(sampler_());
        std::this_thread::sleep_for(std::chrono::milliseconds{1000});
      }
    }};

    logger_.log(log::Level::info, "telemetry", "publishing");
    return {};
  }

  void publish_event(std::string_view id, std::uint16_t code, std::string_view text) noexcept {
    if (pub_)
      pub_->publish_event(id, code, text);
  }

  void stop() noexcept override {
    if (worker_.joinable()) {
      worker_.request_stop();
      worker_.join();
    }
    if (pub_) {
      const telemetry::Stats s = pub_->stats();
      pub_->stop();
      logger_.log(log::Level::info, "telemetry", "stopped",
                  std::array{log::KeyValue::of("published", static_cast<std::int64_t>(s.published)),
                             log::KeyValue::of("failed", static_cast<std::int64_t>(s.failed))});
      pub_.reset();
    }
  }

private:
  log::Logger& logger_;
  const Options& opt_;
  Sampler sampler_;
  std::optional<telemetry::Publisher> pub_;
  std::jthread worker_;
  std::chrono::steady_clock::time_point started_{};
};
#endif

} // namespace

int main(int argc, char** argv) {
  Options opt;
  if (!parse_args(argc, argv, opt)) {
    usage();
    return 2;
  }

  log::Logger logger{stderr};
  logger.set_level(opt.level);

  app::App app;
  LogService log_service{logger};
  SensorService sensor{logger, opt};
  app.add(log_service);
  app.add(sensor);

#if OPTRONIC_WITH_VIDEO
  VideoService video_service{logger, opt};
  if (opt.video)
    app.add(video_service);
#else
  logger.log(log::Level::warn, "main", "built without GStreamer - no video pipeline");
#endif

#if OPTRONIC_WITH_TELEMETRY
  // Last up and first down: everything it reports on is already running when
  // it starts, and it stops before the things it samples go away.
  // The frame count comes from whoever actually counts frames: the pipeline
  // when there is one, the block's own register otherwise.
  TelemetryService telemetry_service{logger, opt, [&] {
                                       telemetry::SensorSample s{};
                                       s.gain = sensor.gain();
                                       s.temp_mc = sensor.temperature_mc();
                                       s.nuc = sensor.nuc_running();
#if OPTRONIC_WITH_VIDEO
                                       s.frame_cnt = video_service.frames();
#else
                                       s.frame_cnt = sensor.frame_count();
#endif
                                       return s;
                                     }};
  if (!opt.broker.empty()) {
    app.add(telemetry_service);
    // Events are pushed, not sampled: a 200 ms NUC never appears in a 1 Hz
    // sensor sample, so it goes out on the event topic at QoS 1 instead.
    sensor.on_event(
        [&telemetry_service](std::string_view id, std::uint16_t code, std::string_view text) {
          telemetry_service.publish_event(id, code, text);
        });
  }
#endif

  // Installed before start(): a SIGTERM arriving during startup must still be
  // seen, not lost between the components coming up.
  const app::SignalStop signals{app};

  if (const status s = app.start(); !s) {
    // SRS-LC-03: name the component and leave with a non-zero code. The
    // lifecycle has already stopped whatever did come up.
    std::fprintf(stderr, "startup aborted at '%.*s' (code 0x%04x)\n",
                 static_cast<int>(s.error().where.size()), s.error().where.data(),
                 static_cast<unsigned>(s.error().code));
    return 1;
  }

  logger.log(log::Level::info, "main", "service up",
             std::array{log::KeyValue::of("temp_mc", sensor.temperature_mc())});

  std::optional<std::jthread> nuc_timer;
  if (opt.nuc_after > 0) {
    nuc_timer.emplace([&sensor, after = opt.nuc_after](std::stop_token stop) {
      for (int i = 0; i < after * 10 && !stop.stop_requested(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
      if (!stop.stop_requested())
        sensor.run_nuc();
    });
  }

  if (opt.seconds > 0) {
    std::jthread timer{[&app, s = opt.seconds] {
      std::this_thread::sleep_for(std::chrono::seconds{s});
      app.request_stop();
    }};
    app.wait_for_stop();
  } else {
    app.wait_for_stop();
  }

  // SRS-LC-02: components stop in reverse order and the process exits 0.
  app.stop();
  return 0;
}
