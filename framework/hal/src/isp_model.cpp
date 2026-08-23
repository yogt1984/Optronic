#include "optronic/hal/isp_ctrl.hpp"

#include <chrono>
#include <memory>
#include <vector>

// The behaviour of the PL block, as far as software can observe it. Keeping it
// next to the register map rather than in the tests means every test sees the
// same device, and a disagreement about how the hardware behaves is settled in
// one place.

namespace optronic::hal::isp {

void install_isp_model(FakeMmio& m, const IspModel& model) {
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

  // Commanding the shutter raises BUSY; it stays raised for a few polls, which
  // is what makes the caller's timeout reachable in a test.
  using clock = std::chrono::steady_clock;
  auto shutter_ready = std::make_shared<clock::time_point>();
  m.on_write(shutter.offset, [shutter_ready, model](FakeMmio& dev, std::uint32_t v) {
    *shutter_ready = clock::now() + model.shutter_move;
    dev.poke(shutter.offset, (v & shutter_bits::close) | shutter_bits::busy);
  });
  m.on_read(shutter.offset, [shutter_ready, model](FakeMmio& dev) {
    if (model.shutter_jams)
      return;
    if (clock::now() >= *shutter_ready)
      dev.poke(shutter.offset, dev.peek(shutter.offset) & ~shutter_bits::busy);
  });

  // Likewise the accumulation: START now, DONE a few polls later.
  auto accum_ready = std::make_shared<clock::time_point>();
  m.on_write(nuc_acc_ctrl.offset, [accum_ready, model](FakeMmio& dev, std::uint32_t v) {
    if ((v & nuc_bits::start) != 0u) {
      *accum_ready = clock::now() + model.nuc_accumulate;
      dev.poke(nuc_acc_ctrl.offset, v & ~nuc_bits::done);
    }
  });
  m.on_read(nuc_acc_ctrl.offset, [accum_ready, model](FakeMmio& dev) {
    const std::uint32_t v = dev.peek(nuc_acc_ctrl.offset);
    if ((v & nuc_bits::start) == 0u)
      return;
    if (clock::now() >= *accum_ready)
      dev.poke(nuc_acc_ctrl.offset, v | nuc_bits::done);
  });

  // The coefficient window: writing ADDR selects an entry, and reading or
  // writing DATA moves to the next one, so a table transfer is a loop over
  // DATA rather than an address computation per element.
  auto table = std::make_shared<std::vector<std::uint32_t>>(1024, 0);
  m.on_write(nuc_table_data.offset, [table](FakeMmio& dev, std::uint32_t v) {
    const std::uint32_t idx = dev.peek(nuc_table_addr.offset) & 0xFFFFu;
    if (idx < table->size())
      (*table)[idx] = v & 0xFFFFu;
    dev.poke(nuc_table_addr.offset, idx + 1);
  });
  m.on_read(nuc_table_data.offset, [table](FakeMmio& dev) {
    const std::uint32_t idx = dev.peek(nuc_table_addr.offset) & 0xFFFFu;
    dev.poke(nuc_table_data.offset, idx < table->size() ? (*table)[idx] : 0u);
    dev.poke(nuc_table_addr.offset, idx + 1);
  });
}

} // namespace optronic::hal::isp
