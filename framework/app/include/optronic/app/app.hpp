#pragma once

// Startup and shutdown order, and nothing else. The App owns no resources of
// its own: it sequences components that do (SPEC-01 §1).

#include "optronic/app/component.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <vector>

namespace optronic::app {

class App {
public:
  App() = default;
  ~App();

  App(const App&) = delete;
  App& operator=(const App&) = delete;
  App(App&&) = delete;
  App& operator=(App&&) = delete;

  // Registration order is dependency order: a component may rely on everything
  // registered before it and on nothing registered after.
  void add(Component& c);

  // init() then start(), both in registration order. A failure at any point
  // stops what is already up, in reverse, and reports the component that
  // failed (SRS-LC-01, SRS-LC-03).
  [[nodiscard]] status start();

  // Reverse registration order. Idempotent, and safe on a partially started
  // system - which is exactly the state a failed start() leaves behind.
  void stop() noexcept;

  // Blocks until request_stop(). The watchdog is kicked once per period while
  // waiting (SRS-LC-04).
  void wait_for_stop(std::chrono::milliseconds watchdog_period = std::chrono::seconds{1});

  // Async-signal-safe: sets a lock-free flag and nothing else. Notifying a
  // condition variable from a signal handler is not allowed, so the waiter
  // polls the flag on a timeout instead.
  void request_stop() noexcept;

  [[nodiscard]] bool stop_requested() const noexcept {
    return stop_requested_.load(std::memory_order_acquire);
  }

  // Number of watchdog kicks performed; lets a test observe SRS-LC-04 without
  // depending on systemd being present.
  [[nodiscard]] std::uint64_t watchdog_kicks() const noexcept {
    return kicks_.load(std::memory_order_relaxed);
  }

private:
  void stop_through(std::size_t count) noexcept;

  std::vector<Component*> components_; // non-owning: the caller owns lifetime
  std::size_t started_ = 0;            // how far up the list start() got
  bool running_ = false;

  static_assert(std::atomic<bool>::is_always_lock_free,
                "request_stop must be callable from a signal handler");
  std::atomic<bool> stop_requested_{false};
  std::atomic<std::uint64_t> kicks_{0};
  std::mutex wait_mutex_;
  std::condition_variable wait_cv_;
};

// Installs SIGTERM/SIGINT handlers that call request_stop() on the app.
// Scoped: the previous handlers are restored on destruction, so a test binary
// does not leave process-wide state behind.
class SignalStop {
public:
  explicit SignalStop(App&);
  ~SignalStop();

  SignalStop(const SignalStop&) = delete;
  SignalStop& operator=(const SignalStop&) = delete;
  SignalStop(SignalStop&&) = delete;
  SignalStop& operator=(SignalStop&&) = delete;
};

} // namespace optronic::app
