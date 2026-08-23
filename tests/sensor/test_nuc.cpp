// The NUC sequence against the modelled block. None of this needs hardware,
// which is the point: the sequence, its timeouts and its failure recovery are
// all settled before anyone stands at a bench with a thermal camera.

#include "optronic/hal/mmio.hpp"
#include "optronic/sensor/nuc.hpp"

#include <gtest/gtest.h>

using namespace optronic;
using namespace optronic::sensor;

namespace {

struct Rig {
  hal::FakeMmio dev;
  hal::RegisterFile<hal::FakeMmio> regs{dev};

  explicit Rig(const hal::isp::IspModel& m = {}) { hal::isp::install_isp_model(dev, m); }

  [[nodiscard]] bool shutter_closed() {
    return regs.any_bit(hal::isp::shutter, hal::isp::shutter_bits::close);
  }
};

NucConfig quick() {
  NucConfig c;
  c.table_entries = 8;
  return c;
}

} // namespace

TEST(Nuc, CompletesAndLeavesTheShutterOpen) {
  Rig rig;
  const auto r = run_nuc(rig.regs, quick());

  ASSERT_TRUE(r) << std::string{r ? "" : r.error().where};
  EXPECT_EQ(r->entries, 8u);
  EXPECT_FALSE(rig.shutter_closed()) << "the channel must be able to see again";
}

// SPEC-04 §3.1: the whole sequence is budgeted at 600 ms.
TEST(Nuc, StaysInsideTheBudget) {
  Rig rig;
  const auto r = run_nuc(rig.regs, quick());
  ASSERT_TRUE(r);
  EXPECT_LT(r->duration.count(), 600) << "sequence took " << r->duration.count() << " ms";
}

TEST(Nuc, RequestsTheConfiguredFrameCount) {
  Rig rig;
  NucConfig cfg = quick();
  cfg.log2_frames = 5; // 32 frames

  ASSERT_TRUE(run_nuc(rig.regs, cfg));

  const std::uint32_t acc = rig.dev.peek(hal::isp::nuc_acc_ctrl.offset);
  EXPECT_EQ((acc & hal::isp::nuc_bits::log2n_mask) >> hal::isp::nuc_bits::log2n_shift, 5u);
}

// A shutter that never finishes moving is the failure that matters: ending a
// failed NUC with the shutter shut leaves the unit blind, which is worse than
// an uncorrected image.
TEST(Nuc, AJammedShutterFailsButLeavesTheChannelSeeing) {
  hal::isp::IspModel jam;
  jam.shutter_jams = true;
  Rig rig{jam};

  NucConfig cfg = quick();
  cfg.shutter_timeout = std::chrono::milliseconds{20};

  const auto r = run_nuc(rig.regs, cfg);

  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().code, Code::hal_shutter);
  EXPECT_FALSE(rig.shutter_closed()) << "a failed NUC must not leave the unit blind";
}

TEST(Nuc, RefusesWhenTheShutterAlreadyReportsAFault) {
  Rig rig;
  rig.dev.poke(hal::isp::shutter.offset, hal::isp::shutter_bits::fault);

  const auto r = run_nuc(rig.regs, quick());
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().code, Code::hal_shutter);
  EXPECT_EQ(r.error().where, "nuc: shutter reports FAULT");
}

TEST(Nuc, AnAccumulationThatNeverFinishesTimesOutAndReopens) {
  hal::isp::IspModel slow;
  slow.nuc_accumulate = std::chrono::seconds{60}; // never within the budget
  Rig rig{slow};

  NucConfig cfg = quick();
  cfg.accumulate_timeout = std::chrono::milliseconds{20};

  const auto r = run_nuc(rig.regs, cfg);

  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().code, Code::timeout);
  EXPECT_FALSE(rig.shutter_closed());
}

// The coefficient window auto-increments ADDR on every DATA access. Reading it
// without resetting ADDR first returns the tail of the table, which is the
// kind of bug that shows up as a faint gradient on one channel.
TEST(Nuc, ReadsTheTableFromTheBeginning) {
  Rig rig;

  rig.regs.write(hal::isp::nuc_table_addr, 0);
  for (std::uint32_t i = 0; i < 8; ++i)
    rig.regs.write(hal::isp::nuc_table_data, 100 + i);

  NucConfig cfg = quick();
  const auto r = run_nuc(rig.regs, cfg);

  ASSERT_TRUE(r);
  // 100..107 average to 103, and only if the read started at index 0.
  EXPECT_EQ(r->mean_offset, 103);
}
