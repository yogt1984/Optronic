// TST-15 (SRS-LT-01/02): several producers hammering the logger, checked for
// races under TSan and for allocations with a counting operator new.

#include "optronic/log/logger.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using namespace optronic::log;

// AddressSanitizer replaces the allocator, so a global operator new of our own
// both fights it (alloc/dealloc mismatch) and measures ASan rather than this
// code. The allocation check therefore runs in the plain and TSan builds and
// announces itself as skipped under ASan rather than quietly vanishing.
#if defined(__SANITIZE_ADDRESS__)
#define OPTRONIC_COUNTS_ALLOCATIONS 0
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define OPTRONIC_COUNTS_ALLOCATIONS 0
#endif
#endif
#ifndef OPTRONIC_COUNTS_ALLOCATIONS
#define OPTRONIC_COUNTS_ALLOCATIONS 1
#endif

namespace {

// Counts every allocation in the process. Global on purpose: an allocation in
// the hot path is a defect wherever it comes from, including from a library
// this code called without noticing.
std::atomic<std::uint64_t> g_allocations{0};

std::FILE* temp_file() {
  std::FILE* f = std::tmpfile();
  EXPECT_NE(f, nullptr);
  return f;
}

std::string contents_of(std::FILE* f) {
  std::fflush(f);
  std::rewind(f);
  std::string s;
  char buf[4096];
  std::size_t n = 0;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
    s.append(buf, n);
  return s;
}

} // namespace

#if OPTRONIC_COUNTS_ALLOCATIONS
void* operator new(std::size_t n) {
  g_allocations.fetch_add(1, std::memory_order_relaxed);
  if (void* p = std::malloc(n))
    return p;
  throw std::bad_alloc{};
}

void operator delete(void* p) noexcept {
  std::free(p);
}
void operator delete(void* p, std::size_t) noexcept {
  std::free(p);
}
#endif

TEST(Logger, WritesLevelComponentMessageAndPairs) {
  std::FILE* f = temp_file();
  {
    Logger log{f};
    const KeyValue kv[] = {KeyValue::of("src", std::int64_t{7}), KeyValue::of("fps", 29.97)};
    log.log(Level::info, "video", "pipeline PLAYING", kv);
    log.drain_now();
  }

  const std::string out = contents_of(f);
  EXPECT_NE(out.find("INFO"), std::string::npos) << out;
  EXPECT_NE(out.find("video"), std::string::npos) << out;
  EXPECT_NE(out.find("pipeline PLAYING"), std::string::npos) << out;
  EXPECT_NE(out.find("src=7"), std::string::npos) << out;
  EXPECT_NE(out.find("fps=29.970"), std::string::npos) << out;
  std::fclose(f);
}

TEST(Logger, RespectsTheLevel) {
  std::FILE* f = temp_file();
  {
    Logger log{f};
    log.set_level(Level::warn);
    log.log(Level::debug, "a", "suppressed");
    log.log(Level::info, "a", "also suppressed");
    log.log(Level::error, "a", "kept");
    log.drain_now();
  }

  const std::string out = contents_of(f);
  EXPECT_EQ(out.find("suppressed"), std::string::npos) << out;
  EXPECT_NE(out.find("kept"), std::string::npos) << out;
  std::fclose(f);
}

// A message longer than the record loses its tail, not the whole record, and
// must not run past the fixed buffer.
TEST(Logger, TruncatesRatherThanOverflowing) {
  std::FILE* f = temp_file();
  const std::string long_msg(500, 'x');
  {
    Logger log{f};
    log.log(Level::info, "componentnamethatiswaytoolong", long_msg);
    log.drain_now();
  }

  const std::string out = contents_of(f);
  EXPECT_NE(out.find("xxxx"), std::string::npos);
  EXPECT_LT(out.size(), 260u) << "record must be bounded by its fixed size";
  std::fclose(f);
}

// SRS-LT-02: after a thread's first call, producing a record must not allocate.
TEST(Logger, DoesNotAllocateOnTheHotPath) {
#if !OPTRONIC_COUNTS_ALLOCATIONS
  GTEST_SKIP() << "allocation counting is disabled under AddressSanitizer";
#else
  std::FILE* f = temp_file();
  {
    Logger log{f};
    log.log(Level::info, "warmup", "registers this thread's ring"); // may allocate

    const std::uint64_t before = g_allocations.load(std::memory_order_relaxed);
    for (int i = 0; i < 1000; ++i)
      log.log(Level::info, "video", "frame");
    const std::uint64_t after = g_allocations.load(std::memory_order_relaxed);

    EXPECT_EQ(after, before) << "log production allocated " << (after - before) << " times";
    log.drain_now();
  }
  std::fclose(f);
#endif
}

TEST(Logger, CountsDropsInsteadOfBlockingWhenTheRingIsFull) {
  std::FILE* f = temp_file();
  {
    Logger log{f}; // no sink thread: nothing drains, so the ring fills
    for (std::size_t i = 0; i < kRingCapacity + 500; ++i)
      log.log(Level::info, "flood", "x");

    const Stats s = log.stats();
    EXPECT_EQ(s.dropped, 500u);
    EXPECT_EQ(s.producers, 1u);
  }
  std::fclose(f);
}

// TST-15. Scaled to keep a TSan run tolerable; the point is the shape, and
// TSan needs far fewer iterations than a probabilistic race hunt would.
TEST(Logger, ManyProducersOneSinkNoRaces) {
  constexpr int kThreads = 4;
  constexpr int kPerThread = 20'000;

  std::FILE* f = temp_file();
  {
    Logger log{f};
    log.start();

    std::vector<std::jthread> producers;
    producers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
      producers.emplace_back([&log, t] {
        const KeyValue kv[] = {KeyValue::of("thread", static_cast<std::int64_t>(t))};
        for (int i = 0; i < kPerThread; ++i)
          log.log(Level::info, "producer", "record", kv);
      });
    }
    producers.clear(); // joins

    log.stop();
    const Stats s = log.stats();

    // One ring per thread that actually logged - the test thread never does.
    EXPECT_EQ(s.producers, static_cast<std::size_t>(kThreads));
    // Nothing may be lost that was not explicitly counted as dropped.
    EXPECT_EQ(s.written + s.dropped, static_cast<std::uint64_t>(kThreads) * kPerThread);
  }
  std::fclose(f);
}

// A Logger on the stack can be destroyed and the next one built at the same
// address. The per-thread ring cache must notice that it is a different
// logger, or it hands out a pointer into the dead one - which segfaults only
// when several tests share a process, so ctest alone would never see it.
TEST(Logger, SurvivesASecondLoggerAtTheSameAddress) {
  std::uintptr_t first_address = 0;
  {
    std::FILE* f = temp_file();
    Logger log{f};
    first_address = reinterpret_cast<std::uintptr_t>(&log);
    log.log(Level::info, "a", "first");
    log.drain_now();
    std::fclose(f);
  }
  {
    std::FILE* f = temp_file();
    Logger log{f};
    log.log(Level::info, "b", "second");
    log.drain_now();

    const std::string out = contents_of(f);
    EXPECT_NE(out.find("second"), std::string::npos) << out;
    EXPECT_EQ(out.find("first"), std::string::npos)
        << "records must not leak from a destroyed logger";
    EXPECT_EQ(log.stats().producers, 1u);
    if (reinterpret_cast<std::uintptr_t>(&log) != first_address) {
      GTEST_LOG_(INFO) << "addresses differed this run; the guard is still required";
    }
    std::fclose(f);
  }
}
