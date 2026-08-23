#include "optronic/hal/isp_ctrl.hpp"

// The behaviour of the PL block, as far as software can observe it. Keeping it
// next to the register map rather than in the tests means every test sees the
// same device, and a disagreement about how the hardware behaves is settled in
// one place.

namespace optronic::hal::isp {

void install_isp_model(FakeMmio& m) {
  // Reset values, SPEC-04 §2.
  m.poke(id.offset, kIdMagic);
  m.poke(version.offset, 0x0001'0000u);
  m.poke(gain.offset, 0x0100u);
  m.poke(frame_w.offset, 1920u);
  m.poke(frame_h.offset, 1080u);
  m.poke(temp_mc.offset, static_cast<std::uint32_t>(41'250)); // a plausible die temperature

  for (std::uint32_t off : {id.offset, version.offset, status.offset, frame_cnt.offset,
                            irq_stat.offset, temp_mc.offset, ts_lo.offset, ts_hi.offset}) {
    m.set_read_only(off);
  }

  // CTRL.SW_RESET clears itself and returns the block to reset state; software
  // that waits for the bit to clear must not spin forever.
  m.on_write(ctrl.offset, [](FakeMmio& dev, std::uint32_t v) {
    if ((v & ctrl_bits::sw_reset) != 0u) {
      dev.poke(ctrl.offset, 0u);
      dev.poke(status.offset, 0u);
      dev.poke(frame_cnt.offset, 0u);
      dev.poke(gain.offset, 0x0100u);
      return;
    }
    // RUNNING follows ENABLE. On the real block it does so at the next frame
    // boundary; here it is immediate, which is the one simplification a test
    // has to keep in mind.
    const std::uint32_t st = dev.peek(status.offset);
    dev.poke(status.offset, (v & ctrl_bits::enable) != 0u ? (st | status_bits::running)
                                                          : (st & ~status_bits::running));
  });

  // IRQ_CLR is write-1-to-clear against the sticky bits in STATUS/IRQ_STAT.
  m.on_write(irq_clr.offset, [](FakeMmio& dev, std::uint32_t v) {
    dev.poke(status.offset, dev.peek(status.offset) & ~v);
    dev.poke(irq_stat.offset, dev.peek(irq_stat.offset) & ~v);
    dev.poke(irq_clr.offset, 0u); // write-only: it does not read back
  });

  // The shutter takes time to move; BUSY is what the NUC sequence polls.
  m.on_write(shutter.offset, [](FakeMmio& dev, std::uint32_t v) {
    dev.poke(shutter.offset, (v & shutter_bits::close) | shutter_bits::busy);
  });

  // Starting an accumulation eventually sets DONE. Immediate here; a test that
  // wants the slow path clears DONE and drives it by hand.
  m.on_write(nuc_acc_ctrl.offset, [](FakeMmio& dev, std::uint32_t v) {
    if ((v & nuc_bits::start) != 0u)
      dev.poke(nuc_acc_ctrl.offset, v | nuc_bits::done);
  });
}

} // namespace optronic::hal::isp
