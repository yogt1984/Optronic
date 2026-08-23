// Runs a real graph on the host: videotestsrc through the appsink seam and out
// to a discarded sink. Proves the RAII and threading contract, not the picture.

#include "optronic/video/pipeline.hpp"
#include "optronic/video/processor.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <thread>

using namespace optronic::video;
using namespace std::chrono_literals;

namespace {

class CountingSink final : public FrameSink {
public:
  FlowResult on_frame(const FrameView& f) noexcept override {
    frames.fetch_add(1, std::memory_order_relaxed);
    if (f.data.empty())
      empty.fetch_add(1, std::memory_order_relaxed);
    last_width.store(f.width, std::memory_order_relaxed);
    return FlowResult::ok;
  }
  std::atomic<int> frames{0};
  std::atomic<int> empty{0};
  std::atomic<std::uint32_t> last_width{0};
};

class RecordingBus final : public BusSink {
public:
  void on_bus(const BusEvent& e) noexcept override {
    if (e.kind == BusEventKind::error)
      errors.fetch_add(1, std::memory_order_relaxed);
  }
  std::atomic<int> errors{0};
};

PipelineSpec host_spec() {
  PipelineSpec s;
  s.source.width = 320;
  s.source.height = 240;
  s.source.fps = 30;
  s.output.kind = OutputKind::none; // no network in a unit test
  return s;
}

} // namespace

TEST(Pipeline, DeliversFramesToTheSink) {
  CountingSink frames;
  RecordingBus bus;

  auto pipe = Pipeline::create(host_spec(), frames, bus);
  ASSERT_TRUE(pipe) << static_cast<int>(pipe.error().code);
  ASSERT_TRUE(pipe->play());

  for (int i = 0; i < 100 && frames.frames.load() < 5; ++i)
    std::this_thread::sleep_for(20ms);

  EXPECT_GE(frames.frames.load(), 5);
  EXPECT_EQ(frames.empty.load(), 0);
  EXPECT_EQ(frames.last_width.load(), 320u);
  EXPECT_EQ(bus.errors.load(), 0);

  const PipelineStats st = pipe->stats();
  EXPECT_GE(st.frames_in, 5u);

  EXPECT_TRUE(pipe->stop());
}

// The destructor alone must bring the graph down; forgetting stop() is the
// normal way a pipeline leaks in real code.
TEST(Pipeline, DestructorStopsARunningGraph) {
  CountingSink frames;
  RecordingBus bus;
  {
    auto pipe = Pipeline::create(host_spec(), frames, bus);
    ASSERT_TRUE(pipe);
    ASSERT_TRUE(pipe->play());
    for (int i = 0; i < 100 && frames.frames.load() < 2; ++i)
      std::this_thread::sleep_for(20ms);
  }
  const int after_scope = frames.frames.load();
  std::this_thread::sleep_for(100ms);
  EXPECT_EQ(frames.frames.load(), after_scope) << "callback fired after destruction";
}

TEST(Pipeline, RejectsAGraphItCannotBuild) {
  CountingSink frames;
  RecordingBus bus;

  PipelineSpec s = host_spec();
  s.encoder.kind = EncoderKind::h264_vcu; // not present on a laptop

  auto pipe = Pipeline::create(s, frames, bus);
  ASSERT_FALSE(pipe);
  EXPECT_EQ(pipe.error().code, optronic::Code::vid_build);
}

TEST(Processor, PassthroughCopiesTheFrameAndSatisfiesTheConcept) {
  static_assert(Processor<Passthrough>);

  std::array<std::byte, 16> in_bytes{};
  in_bytes[0] = std::byte{0xAB};
  std::array<std::byte, 16> out_bytes{};

  FrameView in{};
  in.data = in_bytes;
  in.width = 4;
  in.height = 4;
  in.fmt = PixelFormat::gray8;

  WritableFrame out{};
  out.data = out_bytes;

  Passthrough p;
  EXPECT_EQ(p.process(in, out), ProcessResult::kept);
  EXPECT_EQ(out_bytes[0], std::byte{0xAB});
  EXPECT_EQ(out.width, 4u);
  EXPECT_GT(p.budget().count(), 0);
}

TEST(Processor, PassthroughRefusesAnUndersizedDestination) {
  std::array<std::byte, 16> in_bytes{};
  std::array<std::byte, 4> out_bytes{};

  FrameView in{};
  in.data = in_bytes;
  WritableFrame out{};
  out.data = out_bytes;

  Passthrough p;
  EXPECT_EQ(p.process(in, out), ProcessResult::error);
}
