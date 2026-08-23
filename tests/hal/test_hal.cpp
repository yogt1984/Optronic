// The register layer, tested against the fake. What this cannot prove is that
// the real block behaves like the model - only the unit can. What it does
// prove is that every line of logic above the backend is correct before that
// scarce bench time is spent (SPEC-09 L1).

#include "optronic/hal/isp_ctrl.hpp"
#include "optronic/hal/mmio.hpp"
#include "optronic/hal/register.hpp"

#include <gtest/gtest.h>

using namespace optronic;
using namespace optronic::hal;

namespace {

struct Isp {
  FakeMmio dev;
  RegisterFile<FakeMmio> regs{dev};

  Isp() { isp::install_isp_model(dev); }
};

} // namespace

TEST(FakeMmio, ReadsBackWhatWasWritten) {
  FakeMmio m;
  m.write32(0x10, 0xDEADBEEF);
  EXPECT_EQ(m.read32(0x10), 0xDEADBEEFu);
  EXPECT_EQ(m.faults(), 0u);
}

// SPEC-04 §3: byte and halfword access is undefined on this bus. The fake
// counts the attempt rather than inventing a result.
TEST(FakeMmio, CountsMisalignedAndOutOfRangeAccess) {
  FakeMmio m;
  m.write32(0x02, 1);
  (void)m.read32(0x06);
  m.write32(FakeMmio::size, 1);
  (void)m.read32(FakeMmio::size + 4);
  EXPECT_EQ(m.faults(), 4u);
}

TEST(FakeMmio, IgnoresWritesToReadOnlyRegisters) {
  FakeMmio m;
  m.set_read_only(0x20);
  m.poke(0x20, 42);
  m.write32(0x20, 99);
  EXPECT_EQ(m.read32(0x20), 42u) << "hardware ignores the write";
  EXPECT_EQ(m.faults(), 1u);
}

TEST(IspModel, HasTheDocumentedResetValues) {
  Isp isp;
  EXPECT_EQ(isp.regs.read(isp::id), isp::kIdMagic);
  EXPECT_EQ(isp.regs.read(isp::version) >> 16, 1u);
  EXPECT_EQ(isp.regs.read(isp::gain), 0x0100u);
  EXPECT_EQ(isp.regs.read(isp::frame_w), 1920u);
  EXPECT_EQ(isp.regs.read(isp::frame_h), 1080u);
}

// The bit clears itself, so code that polls it must terminate. A dumb array
// would leave it set and the poll would spin forever on the bench.
TEST(IspModel, SwResetSelfClearsAndReturnsTheBlockToReset) {
  Isp isp;
  isp.regs.write(isp::gain, 0x0800);
  isp.regs.set_bits(isp::ctrl, isp::ctrl_bits::enable);
  ASSERT_TRUE(isp.regs.any_bit(isp::status, isp::status_bits::running));

  isp.regs.write(isp::ctrl, isp::ctrl_bits::sw_reset);

  EXPECT_EQ(isp.regs.read(isp::ctrl), 0u) << "SW_RESET must clear itself";
  EXPECT_FALSE(isp.regs.any_bit(isp::status, isp::status_bits::running));
  EXPECT_EQ(isp.regs.read(isp::gain), 0x0100u);
}

TEST(IspModel, RunningFollowsEnable) {
  Isp isp;
  EXPECT_FALSE(isp.regs.any_bit(isp::status, isp::status_bits::running));
  isp.regs.set_bits(isp::ctrl, isp::ctrl_bits::enable);
  EXPECT_TRUE(isp.regs.any_bit(isp::status, isp::status_bits::running));
  isp.regs.clear_bits(isp::ctrl, isp::ctrl_bits::enable);
  EXPECT_FALSE(isp.regs.any_bit(isp::status, isp::status_bits::running));
}

TEST(IspModel, IrqClearIsWriteOneToClear) {
  Isp isp;
  isp.dev.poke(isp::status.offset, isp::status_bits::frame_done | isp::status_bits::overflow);
  isp.dev.poke(isp::irq_stat.offset, isp::status_bits::frame_done | isp::status_bits::overflow);

  isp.regs.write(isp::irq_clr, isp::status_bits::frame_done);

  EXPECT_FALSE(isp.regs.any_bit(isp::status, isp::status_bits::frame_done));
  EXPECT_TRUE(isp.regs.any_bit(isp::status, isp::status_bits::overflow))
      << "a bit that was not written must survive";
}

TEST(PowerOnBit, PassesOnAHealthyBlock) {
  Isp isp;
  const status s = isp::power_on_bit(isp.regs);
  EXPECT_TRUE(s) << "code " << (s ? 0 : static_cast<int>(s.error().code));
}

TEST(PowerOnBit, NamesTheCheckThatFailed) {
  {
    Isp isp;
    isp.dev.poke(isp::id.offset, 0x1234);
    const status s = isp::power_on_bit(isp.regs);
    ASSERT_FALSE(s);
    EXPECT_EQ(s.error().code, Code::hal_id_mismatch);
    EXPECT_EQ(s.error().where, "bit: ID");
  }
  {
    Isp isp;
    isp.dev.poke(isp::version.offset, 0x0002'0000);
    const status s = isp::power_on_bit(isp.regs);
    ASSERT_FALSE(s);
    EXPECT_EQ(s.error().where, "bit: VERSION.major");
  }
  {
    Isp isp;
    isp.dev.poke(isp::temp_mc.offset, static_cast<std::uint32_t>(-50'000));
    const status s = isp::power_on_bit(isp.regs);
    ASSERT_FALSE(s);
    EXPECT_EQ(s.error().code, Code::bit_poweron);
    EXPECT_EQ(s.error().where, "bit: TEMP_MC");
  }
}

// TS_HI latches when TS_LO is read. Reading them in the wrong order gives a
// timestamp that is wrong only across a 32-bit wrap - once every 4.3 seconds
// at nanosecond resolution, which is the worst kind of bug to find on a rig.
TEST(Timestamp, CombinesLowAndHighWords) {
  Isp isp;
  isp.dev.poke(isp::ts_lo.offset, 0x89AB'CDEFu);
  isp.dev.poke(isp::ts_hi.offset, 0x0123'4567u);
  EXPECT_EQ(isp::read_timestamp_ns(isp.regs), 0x0123'4567'89AB'CDEFull);
}

TEST(RegisterFile, GainMaskMatchesTheDocumentedField) {
  Isp isp;
  isp.regs.write(isp::gain, 0x0ABC & isp::kGainMask);
  EXPECT_EQ(isp.regs.read(isp::gain), 0x0ABCu);
}

// Access rights are part of the type, so these must not compile. Kept as a
// static check of the concepts rather than as a commented-out line that
// nobody ever tries again.
static_assert(Readable<ro_t> && !Writable<ro_t>);
static_assert(Writable<wo_t> && !Readable<wo_t>);
static_assert(Readable<rw_t> && Writable<rw_t>);
template <class F, class R>
concept CanRead = requires(F f, R r) { f.read(r); };
template <class F, class R>
concept CanWrite = requires(F f, R r) { f.write(r, std::uint32_t{}); };

using Regs = RegisterFile<FakeMmio>;

static_assert(!CanWrite<Regs, Reg<0x000, ro_t>>, "writing a read-only register must not compile");
static_assert(!CanRead<Regs, Reg<0x02C, wo_t>>, "reading a write-only register must not compile");
static_assert(CanRead<Regs, Reg<0x000, ro_t>>);
static_assert(CanWrite<Regs, Reg<0x02C, wo_t>>);
static_assert(CanRead<Regs, Reg<0x010, rw_t>> && CanWrite<Regs, Reg<0x010, rw_t>>);

// The map is checked against the document by pointer-to-member as well, which
// is the form SPEC-04 §4 shows.
namespace {
struct Bank {
  Reg<0x010, rw_t> gain;
};
static_assert(offset_of(&Bank::gain) == 0x010);
} // namespace
