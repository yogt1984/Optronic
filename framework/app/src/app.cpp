#include "optronic/app/app.hpp"

#include <csignal>

namespace optronic::app {
namespace {

// A signal handler can touch almost nothing safely, so it touches one atomic
// belonging to the single app instance. Set while the SignalStop guard lives.
std::atomic<App*> g_app{nullptr};

extern "C" void handle_stop_signal(int) noexcept {
  App* app = g_app.load(std::memory_order_acquire);
  if (app != nullptr)
    app->request_stop();
}

} // namespace

App::~App() {
  stop();
}

void App::add(Component& c) {
  components_.push_back(&c);
}

status App::start() {
  for (std::size_t i = 0; i < components_.size(); ++i) {
    if (const status s = components_[i]->init(); !s) {
      // Roll back the ones already initialised, then report the original
      // error - not a second one produced during cleanup.
      stop_through(i);
      return s;
    }
    // Count it as soon as init() succeeds: a component that initialised but
    // failed to start still holds resources and must be stopped.
    started_ = i + 1;

    if (const status s = components_[i]->start(); !s) {
      stop_through(started_);
      return s;
    }
  }
  running_ = true;
  return {};
}

void App::stop() noexcept {
  stop_through(started_);
  running_ = false;
}

// Reverse order, and every component gets its stop() even if an earlier one
// misbehaves - stop() is noexcept precisely so this loop cannot be derailed.
void App::stop_through(std::size_t count) noexcept {
  while (count > 0) {
    --count;
    components_[count]->stop();
  }
  started_ = 0;
}

void App::request_stop() noexcept {
  stop_requested_.store(true, std::memory_order_release);
}

void App::wait_for_stop(std::chrono::milliseconds watchdog_period) {
  std::unique_lock lock{wait_mutex_};
  while (!stop_requested_.load(std::memory_order_acquire)) {
    // Timed wait rather than a plain wait: request_stop() comes from a signal
    // handler, which may not notify a condition variable.
    wait_cv_.wait_for(lock, watchdog_period,
                      [this] { return stop_requested_.load(std::memory_order_acquire); });
    if (stop_requested_.load(std::memory_order_acquire))
      break;

    // Where sd_notify(0, "WATCHDOG=1") goes once the service runs under
    // systemd. Counted here so the period is observable in a test.
    kicks_.fetch_add(1, std::memory_order_relaxed);
  }
}

SignalStop::SignalStop(App& app) {
  g_app.store(&app, std::memory_order_release);
  std::signal(SIGTERM, handle_stop_signal);
  std::signal(SIGINT, handle_stop_signal);
}

SignalStop::~SignalStop() {
  std::signal(SIGTERM, SIG_DFL);
  std::signal(SIGINT, SIG_DFL);
  g_app.store(nullptr, std::memory_order_release);
}

} // namespace optronic::app
