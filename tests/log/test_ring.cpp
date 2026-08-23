// The ring is the part where a memory-ordering mistake produces a bug that
// shows up once a week on the target and never on a laptop, so it is tested
// on its own before the logger that uses it.

#include "optronic/log/ring.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

using namespace optronic::log;

namespace {

Record make(std::uint64_t seq) {
  Record r;
  r.seq = seq;
  return r;
}

} // namespace

TEST(SpscRing, PopsInPushOrder) {
  SpscRing<4> ring;
  Record out;

  EXPECT_FALSE(ring.try_pop(out)) << "empty ring must not yield a record";

  ASSERT_TRUE(ring.try_push(make(1)));
  ASSERT_TRUE(ring.try_push(make(2)));

  ASSERT_TRUE(ring.try_pop(out));
  EXPECT_EQ(out.seq, 1u);
  ASSERT_TRUE(ring.try_pop(out));
  EXPECT_EQ(out.seq, 2u);
  EXPECT_FALSE(ring.try_pop(out));
}

TEST(SpscRing, RefusesWhenFullRatherThanOverwriting) {
  SpscRing<4> ring;
  for (std::uint64_t i = 0; i < 4; ++i)
    ASSERT_TRUE(ring.try_push(make(i)));

  EXPECT_FALSE(ring.try_push(make(99)));
  EXPECT_EQ(ring.size_approx(), 4u);

  // The oldest record must still be intact: a refused push may not corrupt
  // what the consumer has not read yet.
  Record out;
  ASSERT_TRUE(ring.try_pop(out));
  EXPECT_EQ(out.seq, 0u);

  EXPECT_TRUE(ring.try_push(make(99))) << "a freed slot must be reusable";
}

// The indices are free-running and only masked on use, so the wrap is where an
// off-by-one hides. Pushing many times through a small ring exercises it.
TEST(SpscRing, SurvivesManyWraps) {
  SpscRing<4> ring;
  Record out;
  for (std::uint64_t i = 0; i < 10'000; ++i) {
    ASSERT_TRUE(ring.try_push(make(i)));
    ASSERT_TRUE(ring.try_pop(out));
    ASSERT_EQ(out.seq, i);
  }
  EXPECT_EQ(ring.size_approx(), 0u);
}

// One producer, one consumer, no locks: every record must arrive exactly once
// and in order. Under TSan this is also the check that the acquire/release
// pairing is real and not accidental.
TEST(SpscRing, ConcurrentProducerAndConsumerLoseNothing) {
  constexpr std::uint64_t kCount = 200'000;
  SpscRing<1024> ring;
  std::atomic<bool> producer_done{false};

  std::jthread producer{[&] {
    for (std::uint64_t i = 0; i < kCount;) {
      if (ring.try_push(make(i)))
        ++i;
    }
    producer_done.store(true, std::memory_order_release);
  }};

  std::uint64_t expected = 0;
  Record out;
  while (expected < kCount) {
    if (ring.try_pop(out)) {
      ASSERT_EQ(out.seq, expected) << "records must arrive in order and without gaps";
      ++expected;
    } else if (producer_done.load(std::memory_order_acquire) && ring.size_approx() == 0) {
      break;
    }
  }

  EXPECT_EQ(expected, kCount);
}
