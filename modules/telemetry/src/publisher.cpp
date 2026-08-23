#include "optronic/telemetry/publisher.hpp"

#include "optronic/log/logger.hpp"

#include <mosquitto.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <mutex>

namespace optronic::telemetry {
namespace {

// libmosquitto wants a process-wide init/cleanup pair. Reference counted so
// several publishers in one process (or one test binary) behave.
std::mutex g_lib_mutex;
int g_lib_users = 0;

void lib_ref() {
  const std::scoped_lock lock{g_lib_mutex};
  if (g_lib_users++ == 0)
    mosquitto_lib_init();
}

void lib_unref() {
  const std::scoped_lock lock{g_lib_mutex};
  if (--g_lib_users == 0)
    mosquitto_lib_cleanup();
}

struct MosqDelete {
  void operator()(mosquitto* m) const noexcept {
    if (m != nullptr)
      mosquitto_destroy(m);
  }
};
using MosqPtr = std::unique_ptr<mosquitto, MosqDelete>;

// QoS 0 for periodic samples: the next one supersedes a lost one within a
// second, so retransmission buys nothing. QoS 1 for events, which are not
// repeated and whose loss is silent.
constexpr int kQosPeriodic = 0;
constexpr int kQosEvent = 1;

} // namespace

class Publisher::Impl {
public:
  Impl(const Config& cfg, log::Logger& logger) : cfg_(cfg), logger_(logger) { lib_ref(); }

  ~Impl() {
    stop();
    client_.reset();
    lib_unref();
  }

  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;
  Impl(Impl&&) = delete;
  Impl& operator=(Impl&&) = delete;

  status construct() {
    client_.reset(mosquitto_new(cfg_.name.c_str(), true, this));
    if (!client_)
      return fail(Code::mqtt_connect, "mosquitto_new");

    mosquitto_connect_callback_set(client_.get(), &Impl::on_connect);
    mosquitto_disconnect_callback_set(client_.get(), &Impl::on_disconnect);

    // The last will is the cheapest liveness signal there is: the broker
    // publishes it when this client stops talking, so a monitor learns the
    // unit is gone without polling anything.
    const std::string will = R"({"v":1,"state":"OFFLINE"})";
    const std::string will_topic = topic("health");
    if (mosquitto_will_set(client_.get(), will_topic.c_str(), static_cast<int>(will.size()),
                           will.data(), kQosPeriodic, true) != MOSQ_ERR_SUCCESS) {
      return fail(Code::mqtt_connect, "mosquitto_will_set");
    }

    mosquitto_reconnect_delay_set(client_.get(), 1, 30, true); // 1 s doubling to 30 s
    return {};
  }

  status start() {
    // Asynchronous on purpose: a broker that is down at startup must not stop
    // the service. The loop thread keeps retrying in the background.
    const int rc =
        mosquitto_connect_async(client_.get(), cfg_.host.c_str(), static_cast<int>(cfg_.port), 60);
    if (rc != MOSQ_ERR_SUCCESS && rc != MOSQ_ERR_ERRNO) {
      logger_.log(log::Level::warn, "telemetry", "connect refused, will retry");
    }
    if (mosquitto_loop_start(client_.get()) != MOSQ_ERR_SUCCESS) {
      return fail(Code::mqtt_connect, "mosquitto_loop_start");
    }
    running_ = true;
    return {};
  }

  void stop() noexcept {
    if (!running_)
      return;
    running_ = false;
    // Publish OFFLINE explicitly: a clean shutdown should not have to wait for
    // the broker's keepalive to notice the will.
    publish("health", R"({"v":1,"state":"OFFLINE"})", kQosPeriodic, true);
    mosquitto_disconnect(client_.get());
    mosquitto_loop_stop(client_.get(), false);
  }

  void publish(std::string_view leaf, std::string_view payload, int qos, bool retain) noexcept {
    if (!client_)
      return;
    const std::string t = topic(leaf);
    const int rc = mosquitto_publish(client_.get(), nullptr, t.c_str(),
                                     static_cast<int>(payload.size()), payload.data(), qos, retain);
    if (rc == MOSQ_ERR_SUCCESS) {
      published_.fetch_add(1, std::memory_order_relaxed);
      return;
    }

    failed_.fetch_add(1, std::memory_order_relaxed);
    // Once, not once per sample: an unreachable broker produces one publish
    // per second per topic, and logging each would turn a quiet failure into a
    // noisy one without adding information.
    if (!warned_.exchange(true, std::memory_order_relaxed)) {
      logger_.log(log::Level::warn, "telemetry", "publish failed - broker unreachable",
                  std::array{log::KeyValue::of("rc", static_cast<std::int64_t>(rc))});
    }
  }

  [[nodiscard]] std::string topic(std::string_view leaf) const {
    std::string t = "optronic/";
    t += cfg_.name;
    t += '/';
    t.append(leaf);
    return t;
  }

  [[nodiscard]] Stats stats() const noexcept {
    return {published_.load(std::memory_order_relaxed), failed_.load(std::memory_order_relaxed),
            connected_.load(std::memory_order_relaxed)};
  }

  log::Logger& logger() noexcept { return logger_; }

private:
  static void on_connect(mosquitto*, void* self, int rc) noexcept {
    auto* impl = static_cast<Impl*>(self);
    impl->connected_.store(rc == 0, std::memory_order_relaxed);
    if (rc == 0) {
      impl->warned_.store(false, std::memory_order_relaxed); // arm the warning again
      impl->logger_.log(log::Level::info, "telemetry", "connected to broker");
    }
  }

  static void on_disconnect(mosquitto*, void* self, int) noexcept {
    static_cast<Impl*>(self)->connected_.store(false, std::memory_order_relaxed);
  }

  Config cfg_;
  log::Logger& logger_;
  MosqPtr client_;
  bool running_ = false;
  std::atomic<bool> connected_{false};
  std::atomic<bool> warned_{false};
  std::atomic<std::uint64_t> published_{0};
  std::atomic<std::uint64_t> failed_{0};
};

expected<Publisher> Publisher::create(const Config& cfg, log::Logger& logger) {
  auto impl = std::make_unique<Impl>(cfg, logger);
  if (const status s = impl->construct(); !s)
    return std::unexpected(s.error());
  return Publisher{std::move(impl)};
}

Publisher::Publisher(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Publisher::Publisher(Publisher&&) noexcept = default;
Publisher& Publisher::operator=(Publisher&&) noexcept = default;
Publisher::~Publisher() = default;

status Publisher::start() {
  return impl_->start();
}
void Publisher::stop() noexcept {
  impl_->stop();
}
Stats Publisher::stats() const noexcept {
  return impl_->stats();
}
std::string Publisher::topic(std::string_view leaf) const {
  return impl_->topic(leaf);
}

// Payloads are built with snprintf into a fixed buffer rather than a JSON
// library: the shapes are fixed by SPEC-07 §4, and this way publishing costs
// no allocation and cannot throw on a path that must not throw.
void Publisher::publish_health(const HealthSample& s) noexcept {
  std::array<char, 160> buf{};
  const int n =
      std::snprintf(buf.data(), buf.size(), R"({"v":1,"state":"%.*s","uptime_s":%llu,"faults":%u})",
                    static_cast<int>(s.state.size()), s.state.data(),
                    static_cast<unsigned long long>(s.uptime_s), s.faults);
  if (n > 0)
    impl_->publish("health", {buf.data(), static_cast<std::size_t>(n)}, kQosPeriodic, true);
}

void Publisher::publish_sensor(const SensorSample& s) noexcept {
  std::array<char, 200> buf{};
  const int n = std::snprintf(
      buf.data(), buf.size(), R"({"v":1,"gain":%lld,"nuc":%s,"temp_mc":%lld,"frame_cnt":%llu})",
      static_cast<long long>(s.gain), s.nuc ? "true" : "false", static_cast<long long>(s.temp_mc),
      static_cast<unsigned long long>(s.frame_cnt));
  if (n > 0)
    impl_->publish("sensor", {buf.data(), static_cast<std::size_t>(n)}, kQosPeriodic, false);
}

void Publisher::publish_latency(const LatencySample& s) noexcept {
  std::array<char, 220> buf{};
  const int n = std::snprintf(
      buf.data(), buf.size(),
      R"({"v":1,"cap_p95_us":%llu,"proc_p95_us":%llu,"enc_p95_us":%llu,"fps":%.2f,"dropped":%llu})",
      static_cast<unsigned long long>(s.cap_p95_us), static_cast<unsigned long long>(s.proc_p95_us),
      static_cast<unsigned long long>(s.enc_p95_us), s.fps,
      static_cast<unsigned long long>(s.dropped));
  if (n > 0)
    impl_->publish("latency", {buf.data(), static_cast<std::size_t>(n)}, kQosPeriodic, false);
}

// Built by hand into a fixed buffer for the same reason as the others: this is
// called from the frame path, where an allocation per frame is the thing the
// whole design avoids. A frame with more objects than fit is truncated and
// says so, rather than growing without bound.
void Publisher::publish_detections(std::uint64_t seq,
                                   std::span<const DetectedObject> objects) noexcept {
  std::array<char, 2048> buf{};
  int n = std::snprintf(buf.data(), buf.size(), R"({"v":1,"seq":%llu,"count":%zu,"objects":[)",
                        static_cast<unsigned long long>(seq), objects.size());
  if (n <= 0)
    return;

  std::size_t written = 0;
  for (const DetectedObject& o : objects) {
    // Leave room for the closing brackets whatever happens.
    const int room = static_cast<int>(buf.size()) - n - 16;
    if (room <= 0)
      break;
    const int k =
        std::snprintf(buf.data() + n, static_cast<std::size_t>(room),
                      R"(%s{"label":"%.*s","conf":%.2f,"x":%d,"y":%d,"w":%d,"h":%d})",
                      written ? "," : "", static_cast<int>(o.label.size()), o.label.data(),
                      static_cast<double>(o.confidence), o.x, o.y, o.w, o.h);
    if (k <= 0 || k >= room)
      break;
    n += k;
    ++written;
  }

  const int tail =
      std::snprintf(buf.data() + n, buf.size() - static_cast<std::size_t>(n),
                    R"(],"truncated":%s})", written < objects.size() ? "true" : "false");
  if (tail <= 0)
    return;
  n += tail;

  impl_->publish("detections", {buf.data(), static_cast<std::size_t>(n)}, kQosPeriodic, false);
}

void Publisher::publish_event(std::string_view id, std::uint16_t code,
                              std::string_view text) noexcept {
  std::array<char, 256> buf{};
  const int n =
      std::snprintf(buf.data(), buf.size(), R"({"v":1,"id":"%.*s","code":%u,"text":"%.*s"})",
                    static_cast<int>(id.size()), id.data(), code,
                    static_cast<int>(std::min<std::size_t>(text.size(), 120)), text.data());
  if (n > 0)
    impl_->publish("event", {buf.data(), static_cast<std::size_t>(n)}, kQosEvent, false);
}

} // namespace optronic::telemetry
