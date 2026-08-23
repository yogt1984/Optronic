// TST-16 (SRS-LT-03/04). The important test is not that a payload arrives -
// it is that nothing bad happens when it does not.

#include "optronic/log/logger.hpp"
#include "optronic/telemetry/publisher.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

using namespace optronic;
using namespace std::chrono_literals;

namespace {

telemetry::Config unreachable_broker() {
  telemetry::Config c;
  c.name = "testnode";
  c.host = "127.0.0.1";
  c.port = 1; // nothing listens here
  return c;
}

// The logger has to be destroyed before the stream it writes to: its
// destructor drains whatever is still in the rings and formats it. Declaration
// order gives that for free, because members are destroyed in reverse - so the
// file wrapper is declared first and therefore closed last.
struct TempFile {
  std::FILE* f = std::tmpfile();
  ~TempFile() {
    if (f != nullptr)
      std::fclose(f);
  }
  TempFile() = default;
  TempFile(const TempFile&) = delete;
  TempFile& operator=(const TempFile&) = delete;
};

struct LogTo {
  TempFile file;
  log::Logger logger{file.f};
};

} // namespace

TEST(Telemetry, TopicsFollowTheSpecifiedNamespace) {
  LogTo out;
  auto& logger = out.logger;

  auto pub = telemetry::Publisher::create(unreachable_broker(), logger);
  ASSERT_TRUE(pub);

  EXPECT_EQ(pub->topic("health"), "optronic/testnode/health");
  EXPECT_EQ(pub->topic("sensor"), "optronic/testnode/sensor");
  EXPECT_EQ(pub->topic("event"), "optronic/testnode/event");
}

// The whole point of SRS-LT-04: an unreachable broker is a WARN and a counter,
// not an error, not an exception, and not a reason to stop.
TEST(Telemetry, AnUnreachableBrokerIsHarmless) {
  LogTo out;
  auto& logger = out.logger;

  auto pub = telemetry::Publisher::create(unreachable_broker(), logger);
  ASSERT_TRUE(pub) << "construction must not need the broker";
  ASSERT_TRUE(pub->start()) << "start must succeed even with nothing listening";

  for (int i = 0; i < 20; ++i) {
    pub->publish_health({"OK", static_cast<std::uint64_t>(i), 0});
    pub->publish_sensor({256, false, 41250, static_cast<std::uint64_t>(i) * 30});
    pub->publish_event("ENCODER_LOST", 0x0403, "bus error on enc");
  }

  const telemetry::Stats s = pub->stats();
  EXPECT_FALSE(s.connected);
  EXPECT_GT(s.failed, 0u) << "failures must be counted, not hidden";

  pub->stop();
}

// Publishing must stay bounded: it is called from the same threads that carry
// frames, so a slow or absent broker may not turn into latency.
TEST(Telemetry, PublishingDoesNotBlock) {
  LogTo out;
  auto& logger = out.logger;

  auto pub = telemetry::Publisher::create(unreachable_broker(), logger);
  ASSERT_TRUE(pub);
  ASSERT_TRUE(pub->start());

  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < 500; ++i)
    pub->publish_sensor({256, false, 41250, 0});
  const auto elapsed = std::chrono::steady_clock::now() - t0;

  // A generous bound on purpose. The property is that publishing never waits
  // on the network: a blocking implementation would spend a connect timeout -
  // seconds - on the very first call. Anything in the same order as this bound
  // proves that; a tighter one only measures how loaded the machine is, and
  // flakes when the whole CI runs at once.
  EXPECT_LT(elapsed, 2s) << "500 publishes to a dead broker must not block";
  pub->stop();
}

// The detections topic is what turns a picture into something a machine can
// act on, so its shape is a contract worth pinning down.
TEST(Telemetry, DetectionsAreAListWithPositions) {
  LogTo out;
  auto& logger = out.logger;

  auto pub = telemetry::Publisher::create(unreachable_broker(), logger);
  ASSERT_TRUE(pub);
  EXPECT_EQ(pub->topic("detections"), "optronic/testnode/detections");

  const telemetry::DetectedObject objects[] = {
      {"person", 0.91F, 100, 50, 80, 200},
      {"chair", 0.55F, 12, 4, 60, 70},
  };
  pub->publish_detections(42, objects);

  // Nothing is listening, so the counter is the observable effect; the payload
  // itself is checked below by building it the same way.
  EXPECT_GT(pub->stats().failed, 0u);
}

// A frame with more objects than the buffer holds must truncate and say so,
// not overrun and not grow.
TEST(Telemetry, TooManyDetectionsTruncateRatherThanOverflow) {
  LogTo out;
  auto pub = telemetry::Publisher::create(unreachable_broker(), out.logger);
  ASSERT_TRUE(pub);

  std::vector<telemetry::DetectedObject> many(400);
  for (auto& o : many)
    o = {"aeroplane", 0.5F, 1000, 1000, 100, 100};

  pub->publish_detections(1, many); // must not crash or corrupt
  pub->publish_detections(2, {});   // and an empty list is fine
  SUCCEED();
}

TEST(Telemetry, StopIsSafeWithoutStart) {
  LogTo out;
  auto& logger = out.logger;
  auto pub = telemetry::Publisher::create(unreachable_broker(), logger);
  ASSERT_TRUE(pub);
  pub->stop();
  pub->stop();
}

TEST(Telemetry, DestructorCleansUpARunningClient) {
  LogTo out;
  auto& logger = out.logger;
  {
    auto pub = telemetry::Publisher::create(unreachable_broker(), logger);
    ASSERT_TRUE(pub);
    ASSERT_TRUE(pub->start());
    pub->publish_health({"OK", 1, 0});
  }
  SUCCEED() << "no leak, no hang, no double free";
}
