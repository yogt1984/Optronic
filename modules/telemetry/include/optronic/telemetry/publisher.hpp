#pragma once

// Telemetry observes; it never influences. A broker that is down, slow or
// absent must be invisible to the pipeline and must not move the health state
// - the worst outcome of losing telemetry is that nobody is watching, and the
// second worst is a unit that degrades itself because nobody is watching
// (SRS-LT-04).
//
// No mosquitto type appears in this header: the dependency is confined to this
// module and tools/check_deps.sh fails the build if it escapes.

#include "optronic/core/expected.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace optronic::log {
class Logger;
}

namespace optronic::telemetry {

struct Config {
  std::string name = "node1"; // client id and topic prefix: optronic/<name>/...
  std::string host = "127.0.0.1";
  std::uint16_t port = 1883;
  std::chrono::milliseconds period{1000};
};

struct HealthSample {
  std::string_view state = "OK"; // INIT | OK | DEGRADED | FAULT
  std::uint64_t uptime_s = 0;
  std::uint32_t faults = 0;
};

struct SensorSample {
  std::int64_t gain = 0;
  bool nuc = false;
  std::int64_t temp_mc = 0;
  std::uint64_t frame_cnt = 0;
};

struct LatencySample {
  std::uint64_t cap_p95_us = 0;
  std::uint64_t proc_p95_us = 0;
  std::uint64_t enc_p95_us = 0;
  double fps = 0.0;
  std::uint64_t dropped = 0;
};

struct Stats {
  std::uint64_t published = 0;
  std::uint64_t failed = 0;
  bool connected = false;
};

class Publisher {
public:
  // Constructing does not talk to the network: a broker that is unreachable at
  // startup must not stop the service from starting.
  [[nodiscard]] static expected<Publisher> create(const Config&, log::Logger&);

  Publisher(Publisher&&) noexcept;
  Publisher& operator=(Publisher&&) noexcept;
  Publisher(const Publisher&) = delete;
  Publisher& operator=(const Publisher&) = delete;
  ~Publisher();

  // Starts the client's own network thread and reconnect loop. Returns ok even
  // when the broker is down; the loop keeps trying with backoff.
  [[nodiscard]] status start();
  void stop() noexcept;

  // All noexcept and non-blocking. A failed publish increments a counter and
  // logs at WARN once, and that is the entire consequence.
  void publish_health(const HealthSample&) noexcept; // retained, QoS 0
  void publish_sensor(const SensorSample&) noexcept; // QoS 0
  void publish_latency(const LatencySample&) noexcept;
  void publish_event(std::string_view id, std::uint16_t code,
                     std::string_view text) noexcept; // QoS 1

  [[nodiscard]] Stats stats() const noexcept;
  [[nodiscard]] std::string topic(std::string_view leaf) const;

private:
  class Impl;
  explicit Publisher(std::unique_ptr<Impl>) noexcept;
  std::unique_ptr<Impl> impl_;
};

} // namespace optronic::telemetry
