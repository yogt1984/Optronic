// Order is the whole contract: up in registration order, down in reverse, and
// a failure mid-startup leaves nothing running.

#include "optronic/app/app.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <csignal>
#include <string>
#include <thread>
#include <vector>

using namespace optronic;
using namespace optronic::app;
using namespace std::chrono_literals;

namespace {

// Shared log so the interleaving of several components is visible, not just
// each component's own history.
class Recorder final : public Component {
public:
  Recorder(std::string n, std::vector<std::string>& log) : name_(std::move(n)), log_(log) {}

  std::string_view name() const noexcept override { return name_; }

  status init() override {
    log_.push_back(name_ + ":init");
    if (fail_init)
      return fail(Code::not_ready, "Recorder::init");
    return {};
  }

  status start() override {
    log_.push_back(name_ + ":start");
    if (fail_start)
      return fail(Code::io, "Recorder::start");
    return {};
  }

  void stop() noexcept override {
    log_.push_back(name_ + ":stop");
    ++stops;
  }

  bool fail_init = false;
  bool fail_start = false;
  int stops = 0;

private:
  std::string name_;
  std::vector<std::string>& log_;
};

} // namespace

TEST(App, StartsInOrderAndStopsInReverse) {
  std::vector<std::string> log;
  Recorder a{"a", log}, b{"b", log}, c{"c", log};

  App app;
  app.add(a);
  app.add(b);
  app.add(c);

  ASSERT_TRUE(app.start());
  app.stop();

  EXPECT_EQ(log, (std::vector<std::string>{"a:init", "a:start", "b:init", "b:start", "c:init",
                                           "c:start", "c:stop", "b:stop", "a:stop"}));
}

// SRS-LC-03: the failure aborts startup, and the components that did come up
// are torn down rather than left running.
TEST(App, FailedInitAbortsAndRollsBack) {
  std::vector<std::string> log;
  Recorder a{"a", log}, b{"b", log}, c{"c", log};
  b.fail_init = true;

  App app;
  app.add(a);
  app.add(b);
  app.add(c);

  const status s = app.start();
  ASSERT_FALSE(s);
  EXPECT_EQ(s.error().code, Code::not_ready);

  EXPECT_EQ(log, (std::vector<std::string>{"a:init", "a:start", "b:init", "a:stop"}));
  EXPECT_EQ(c.stops, 0) << "a component that never initialised must not be stopped";
}

// A component that initialised but failed to start still holds whatever init()
// acquired, so it must be stopped too - the off-by-one that leaks in real code.
TEST(App, FailedStartStopsTheComponentThatFailed) {
  std::vector<std::string> log;
  Recorder a{"a", log}, b{"b", log};
  b.fail_start = true;

  App app;
  app.add(a);
  app.add(b);

  ASSERT_FALSE(app.start());
  EXPECT_EQ(log, (std::vector<std::string>{"a:init", "a:start", "b:init", "b:start", "b:stop",
                                           "a:stop"}));
}

TEST(App, StopIsIdempotent) {
  std::vector<std::string> log;
  Recorder a{"a", log};

  App app;
  app.add(a);
  ASSERT_TRUE(app.start());

  app.stop();
  app.stop();
  app.stop();

  EXPECT_EQ(a.stops, 1);
}

TEST(App, DestructorStopsWhatIsStillRunning) {
  std::vector<std::string> log;
  Recorder a{"a", log};
  {
    App app;
    app.add(a);
    ASSERT_TRUE(app.start());
  }
  EXPECT_EQ(a.stops, 1);
}

TEST(App, WaitReturnsWhenStopIsRequested) {
  App app;
  std::jthread requester{[&app] {
    std::this_thread::sleep_for(50ms);
    app.request_stop();
  }};

  const auto t0 = std::chrono::steady_clock::now();
  app.wait_for_stop(10ms);
  const auto elapsed = std::chrono::steady_clock::now() - t0;

  EXPECT_TRUE(app.stop_requested());
  EXPECT_LT(elapsed, 2s) << "SRS-LC-02: shutdown must begin well inside two seconds";
}

// SRS-LC-04: the watchdog is kicked while waiting, not only at startup.
TEST(App, WatchdogIsKickedWhileWaiting) {
  App app;
  std::jthread requester{[&app] {
    std::this_thread::sleep_for(120ms);
    app.request_stop();
  }};

  app.wait_for_stop(20ms);
  EXPECT_GE(app.watchdog_kicks(), 2u);
}

// SRS-LC-02: a real SIGTERM, not a direct call, so the handler path is covered.
TEST(App, SigtermRequestsStop) {
  App app;
  const SignalStop guard{app};

  ASSERT_FALSE(app.stop_requested());
  std::raise(SIGTERM);

  for (int i = 0; i < 100 && !app.stop_requested(); ++i)
    std::this_thread::sleep_for(5ms);
  EXPECT_TRUE(app.stop_requested());
}
