#pragma once

// One ring per producing thread, one sink thread draining all of them. The
// producer never locks, never allocates and never formats; everything
// expensive happens on the sink side (SPEC-07 §2).

#include "optronic/log/record.hpp"
#include "optronic/log/ring.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

namespace optronic::log {

inline constexpr std::size_t kRingCapacity = 1024; // 256 KiB per producer thread

using Ring = SpscRing<kRingCapacity>;

struct Stats {
  std::uint64_t written = 0;
  std::uint64_t dropped = 0;
  std::size_t producers = 0;
};

class Logger {
public:
  explicit Logger(std::FILE* out = stderr);
  ~Logger();

  Logger(const Logger&) = delete;
  Logger& operator=(const Logger&) = delete;
  Logger(Logger&&) = delete;
  Logger& operator=(Logger&&) = delete;

  void start();
  void stop() noexcept;

  void set_level(Level l) noexcept { level_.store(l, std::memory_order_relaxed); }
  [[nodiscard]] Level level() const noexcept { return level_.load(std::memory_order_relaxed); }

  // The hot path. noexcept, allocation-free after this thread's first call,
  // and lock-free from the second call onward - the mutex below is taken once
  // per thread to register its ring, never per record.
  void log(Level l, std::string_view component, std::string_view msg,
           std::span<const KeyValue> kv = {}) noexcept;

  [[nodiscard]] Stats stats() const noexcept;

  // Drains every ring once from the calling thread. Exists so a test can be
  // deterministic without sleeping on the sink thread.
  std::size_t drain_now();

private:
  struct Producer {
    Ring ring;
    std::atomic<std::uint64_t> dropped{0};
  };

  Producer& producer_for_this_thread() noexcept;
  void sink_loop(std::stop_token stop);
  void write_record(const Record& r);
  void report_drops();

  // Identity for the thread_local cache below. A pointer will not do: a
  // Logger on the stack can be destroyed and the next one allocated at the
  // same address, which would make a stale cached pointer look valid.
  const std::uint64_t id_;

  std::FILE* out_;
  std::atomic<Level> level_{Level::info};
  std::atomic<std::uint64_t> written_{0};
  std::atomic<std::uint64_t> seq_{0};
  std::atomic<std::uint64_t> reported_drops_{0};
  std::chrono::steady_clock::time_point last_drop_report_{};

  // Registration only. The sink thread reads the vector under the same mutex;
  // producers touch it exactly once each, on their first log call.
  mutable std::mutex producers_mutex_;
  std::vector<std::unique_ptr<Producer>> producers_;

  std::jthread sink_;
};

} // namespace optronic::log
