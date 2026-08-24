// The detector against a known image. Not a test of YOLO's accuracy - that is
// the model's business - but of the parts this repository owns: that a frame
// goes in, detections come out, and the boxes are written into the buffer the
// caller handed over.

#include "optronic/detect/yolo.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

using namespace optronic;

namespace {

// The default model, not whichever one happens to be lying around: checking
// for a different file than the one create() loads is how a test skips on one
// machine and fails on another.
bool model_present() {
  namespace fs = std::filesystem;
  return fs::exists(detect::YoloConfig{}.weights) && fs::exists(detect::YoloConfig{}.names);
}

// A synthetic scene the detector will not find anything in, used to check the
// no-detection path leaves the frame untouched.
std::vector<std::byte> flat_frame(std::uint32_t w, std::uint32_t h, std::uint8_t value) {
  return std::vector<std::byte>(static_cast<std::size_t>(w) * h, static_cast<std::byte>(value));
}

video::FrameView view_of(const std::vector<std::byte>& buf, std::uint32_t w, std::uint32_t h) {
  video::FrameView v{};
  v.data = buf;
  v.width = w;
  v.height = h;
  v.stride = w;
  v.fmt = video::PixelFormat::nv12;
  return v;
}

} // namespace

TEST(Yolo, ReportsAMissingModelRatherThanCrashing) {
  detect::YoloConfig cfg;
  cfg.weights = "models/does-not-exist.weights";
  cfg.config = "models/does-not-exist.cfg";
  cfg.names = "models/does-not-exist.names";

  const auto y = detect::Yolo::create(cfg);
  ASSERT_FALSE(y);
  EXPECT_EQ(y.error().code, Code::io);
}

TEST(Yolo, SatisfiesTheProcessorConceptAndDeclaresABudget) {
  static_assert(video::Processor<detect::Yolo>);
  if (!model_present())
    GTEST_SKIP() << "no model - run tools/get_model.sh";

  const auto y = detect::Yolo::create();
  ASSERT_TRUE(y);
  // Honest, not aspirational: tiny-YOLO on a CPU is tens of milliseconds.
  EXPECT_GT(y->budget().count(), 1000);
}

// An empty scene must leave the buffer exactly as it was: a detector that
// scribbles when it finds nothing is worse than one that finds nothing.
TEST(Yolo, LeavesTheFrameAloneWhenThereIsNothingToFind) {
  if (!model_present())
    GTEST_SKIP() << "no model - run tools/get_model.sh";

  auto y = detect::Yolo::create();
  ASSERT_TRUE(y);

  constexpr std::uint32_t w = 320, h = 240;
  const auto original = flat_frame(w, h, 0x40);
  auto working = original;

  const video::FrameView in = view_of(working, w, h);
  video::WritableFrame out{};
  out.data = working;
  out.width = w;
  out.height = h;
  out.stride = w;

  for (int i = 0; i < 6; ++i)
    EXPECT_EQ(y->process(in, out), video::ProcessResult::kept);

  EXPECT_EQ(working, original) << "a flat frame produced marks in the buffer";
  EXPECT_TRUE(y->last().empty());
}

TEST(Yolo, RefusesAnEmptyFrame) {
  if (!model_present())
    GTEST_SKIP() << "no model - run tools/get_model.sh";

  auto y = detect::Yolo::create();
  ASSERT_TRUE(y);

  video::FrameView in{};
  video::WritableFrame out{};
  EXPECT_EQ(y->process(in, out), video::ProcessResult::error);
}

// The one that matters: a real image, real detections, and pixels changed in
// the caller's buffer. Reads the reference image as raw luma so the test needs
// no image decoder of its own.
TEST(Yolo, DrawsBoxesIntoTheCallersBuffer) {
  if (!model_present())
    GTEST_SKIP() << "no model - run tools/get_model.sh";
  if (!std::filesystem::exists("tests/detect/scene.gray"))
    GTEST_SKIP() << "no reference scene - see tests/detect/README.md";

  std::ifstream f{"tests/detect/scene.gray", std::ios::binary};
  ASSERT_TRUE(f);
  const std::vector<char> raw{std::istreambuf_iterator<char>{f}, {}};
  std::vector<std::byte> pixels(raw.size());
  std::transform(raw.begin(), raw.end(), pixels.begin(),
                 [](char c) { return static_cast<std::byte>(static_cast<unsigned char>(c)); });

  constexpr std::uint32_t w = 768, h = 576; // the reference image geometry
  ASSERT_EQ(pixels.size(), static_cast<std::size_t>(w) * h);

  // Synchronous on purpose: this test is about the drawing, and inference on
  // a worker would make "has it finished yet" part of the assertion.
  detect::YoloConfig cfg;
  cfg.async = false;
  cfg.detect_every = 1;
  auto y = detect::Yolo::create(cfg);
  ASSERT_TRUE(y);

  const auto before = pixels;
  const video::FrameView in = view_of(pixels, w, h);
  video::WritableFrame out{};
  out.data = pixels;
  out.width = w;
  out.height = h;
  out.stride = w;

  // Two frames: the first detects, the second draws what it found.
  for (int i = 0; i < 2; ++i)
    EXPECT_EQ(y->process(in, out), video::ProcessResult::kept);

  EXPECT_FALSE(y->last().empty()) << "the reference scene should contain objects";
  EXPECT_NE(pixels, before) << "detections were found but nothing was drawn";

  const auto changed = static_cast<std::size_t>(
      std::count_if(pixels.begin(), pixels.end(),
                    [&, i = std::size_t{0}](std::byte b) mutable { return b != before[i++]; }));
  EXPECT_GT(changed, 100u) << "only " << changed << " pixels changed - boxes look wrong";
}

// The async path is the one the service actually uses: the streaming thread
// must return promptly and pick the boxes up on a later frame.
TEST(Yolo, AsyncInferenceDoesNotBlockTheCaller) {
  if (!model_present())
    GTEST_SKIP() << "no model - run tools/get_model.sh";
  if (!std::filesystem::exists("tests/detect/scene.gray"))
    GTEST_SKIP() << "no reference scene - see tests/detect/README.md";

  std::ifstream f{"tests/detect/scene.gray", std::ios::binary};
  ASSERT_TRUE(f);
  const std::vector<char> raw{std::istreambuf_iterator<char>{f}, {}};
  std::vector<std::byte> pixels(raw.size());
  std::transform(raw.begin(), raw.end(), pixels.begin(),
                 [](char c) { return static_cast<std::byte>(static_cast<unsigned char>(c)); });

  constexpr std::uint32_t w = 768, h = 576;
  detect::YoloConfig cfg;
  cfg.async = true;
  auto y = detect::Yolo::create(cfg);
  ASSERT_TRUE(y);

  const video::FrameView in = view_of(pixels, w, h);
  video::WritableFrame out{};
  out.data = pixels;
  out.width = w;
  out.height = h;
  out.stride = w;

  // Each call must cost far less than an inference - that is the whole point.
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < 5; ++i)
    EXPECT_EQ(y->process(in, out), video::ProcessResult::kept);
  const auto per_call =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0) /
      5;
  EXPECT_LT(per_call.count(), 50) << "the streaming thread waited on inference";

  // The worker gets there in its own time.
  for (int i = 0; i < 100 && y->last().empty(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    (void)y->process(in, out);
  }
  EXPECT_FALSE(y->last().empty()) << "the worker never produced a result";
}
