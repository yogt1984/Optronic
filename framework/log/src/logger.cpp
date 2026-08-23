#include "optronic/log/logger.hpp"

#include <algorithm>
#include <cinttypes>
#include <ctime>

namespace optronic::log {

KeyValue KeyValue::of(std::string_view k, std::int64_t v) noexcept {
  KeyValue kv;
  copy_truncated(kv.key, sizeof(kv.key), k);
  kv.kind = ValueKind::integer;
  kv.value = v;
  return kv;
}

KeyValue KeyValue::of(std::string_view k, double v) noexcept {
  KeyValue kv;
  copy_truncated(kv.key, sizeof(kv.key), k);
  kv.kind = ValueKind::real;
  std::memcpy(&kv.value, &v, sizeof(v));
  return kv;
}

double KeyValue::as_real() const noexcept {
  double d = 0.0;
  std::memcpy(&d, &value, sizeof(d));
  return d;
}

void copy_truncated(char* dst, std::size_t dst_size, std::string_view src) noexcept {
  const std::size_t n = std::min(src.size(), dst_size - 1);
  std::memcpy(dst, src.data(), n);
  dst[n] = '\0';
}

namespace {

std::uint64_t mono_ns() noexcept {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
         static_cast<std::uint64_t>(ts.tv_nsec);
}

} // namespace

namespace {
std::atomic<std::uint64_t> g_next_logger_id{1};
}

Logger::Logger(std::FILE* out)
    : id_(g_next_logger_id.fetch_add(1, std::memory_order_relaxed)),
      out_(out != nullptr ? out : stderr) {}

Logger::~Logger() {
  stop();
  drain_now();
  std::fflush(out_);
}

void Logger::start() {
  if (sink_.joinable())
    return;
  last_drop_report_ = std::chrono::steady_clock::now();
  sink_ = std::jthread{[this](std::stop_token stop) { sink_loop(stop); }};
}

void Logger::stop() noexcept {
  if (!sink_.joinable())
    return;
  sink_.request_stop();
  sink_.join();
}

// The first call on a thread registers that thread's ring; every later call
// finds it in a thread_local pointer and touches no shared mutable state.
Logger::Producer& Logger::producer_for_this_thread() noexcept {
  thread_local Producer* mine = nullptr;
  thread_local std::uint64_t owner_id = 0;

  if (mine != nullptr && owner_id == id_)
    return *mine;

  const std::scoped_lock lock{producers_mutex_};
  producers_.push_back(std::make_unique<Producer>());
  mine = producers_.back().get();
  owner_id = id_;
  return *mine;
}

void Logger::log(Level l, std::string_view component, std::string_view msg,
                 std::span<const KeyValue> kv) noexcept {
  if (l < level_.load(std::memory_order_relaxed))
    return;

  Producer& p = producer_for_this_thread();

  Record r;
  r.ts_mono_ns = mono_ns();
  r.seq = seq_.fetch_add(1, std::memory_order_relaxed);
  r.level = l;
  copy_truncated(r.component, sizeof(r.component), component);
  copy_truncated(r.msg, sizeof(r.msg), msg);

  const std::size_t n = std::min(kv.size(), kMaxKeyValues);
  for (std::size_t i = 0; i < n; ++i)
    r.kv[i] = kv[i];
  r.kv_count = static_cast<std::uint8_t>(n);

  // Full ring means the sink cannot keep up. Dropping the newest record and
  // counting it is the only option that keeps this call bounded: blocking
  // would push the stall into the frame path, and overwriting the oldest
  // would mean this thread moving an index the consumer owns.
  if (!p.ring.try_push(r))
    p.dropped.fetch_add(1, std::memory_order_relaxed);
}

void Logger::write_record(const Record& r) {
  const auto secs = static_cast<std::time_t>(r.ts_mono_ns / 1'000'000'000ULL);
  const auto ms = (r.ts_mono_ns % 1'000'000'000ULL) / 1'000'000ULL;

  std::fprintf(out_, "%6lld.%03llu %-5s %-10s %s", static_cast<long long>(secs),
               static_cast<unsigned long long>(ms), level_name(r.level).data(), r.component, r.msg);

  for (std::uint8_t i = 0; i < r.kv_count; ++i) {
    const KeyValue& kv = r.kv[i];
    if (kv.kind == ValueKind::integer) {
      std::fprintf(out_, " %s=%" PRId64, kv.key, kv.value);
    } else if (kv.kind == ValueKind::real) {
      std::fprintf(out_, " %s=%.3f", kv.key, kv.as_real());
    }
  }
  std::fputc('\n', out_);
  written_.fetch_add(1, std::memory_order_relaxed);
}

std::size_t Logger::drain_now() {
  std::size_t n = 0;
  const std::scoped_lock lock{producers_mutex_};
  Record r;
  for (auto& p : producers_) {
    while (p->ring.try_pop(r)) {
      write_record(r);
      ++n;
    }
  }
  return n;
}

// Reported once per second rather than per drop: a ring that is overflowing is
// overflowing thousands of times a second, and logging that would make the
// problem worse.
void Logger::report_drops() {
  std::uint64_t total = 0;
  {
    const std::scoped_lock lock{producers_mutex_};
    for (const auto& p : producers_)
      total += p->dropped.load(std::memory_order_relaxed);
  }
  const std::uint64_t already = reported_drops_.load(std::memory_order_relaxed);
  if (total == already)
    return;

  std::fprintf(out_, "%6s %-5s %-10s log dropped=%llu\n", "", level_name(Level::warn).data(), "log",
               static_cast<unsigned long long>(total - already));
  reported_drops_.store(total, std::memory_order_relaxed);
}

void Logger::sink_loop(std::stop_token stop) {
  using namespace std::chrono_literals;
  while (!stop.stop_requested()) {
    const std::size_t n = drain_now();

    const auto now = std::chrono::steady_clock::now();
    if (now - last_drop_report_ >= 1s) {
      report_drops();
      last_drop_report_ = now;
    }

    // Idle backs off; busy keeps draining. A spin here would burn a core on a
    // system that is meant to be spending it on frames.
    if (n == 0) {
      std::fflush(out_);
      std::this_thread::sleep_for(1ms);
    }
  }
  drain_now();
  report_drops();
  std::fflush(out_);
}

Stats Logger::stats() const noexcept {
  Stats s;
  s.written = written_.load(std::memory_order_relaxed);
  const std::scoped_lock lock{producers_mutex_};
  s.producers = producers_.size();
  for (const auto& p : producers_)
    s.dropped += p->dropped.load(std::memory_order_relaxed);
  return s;
}

} // namespace optronic::log
