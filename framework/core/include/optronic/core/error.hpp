#pragma once

// SPEC-06 §1 error space. The range encodes the domain, so a bare u16 code
// still says where it came from. Kept POD: an Error crosses thread and C
// callback boundaries where an exception must not.

#include <cstdint>
#include <string_view>

namespace optronic {

enum class Code : std::uint16_t {
  ok = 0x0000,

  invalid_arg = 0x0001,
  timeout = 0x0002,
  not_ready = 0x0003,
  io = 0x0004,
  no_mem = 0x0005,

  cfg_parse = 0x0201,
  cfg_schema = 0x0202,
  cfg_range = 0x0203,

  hal_open = 0x0301,
  hal_map = 0x0302,
  hal_id_mismatch = 0x0303,
  hal_irq_timeout = 0x0304,
  hal_shutter = 0x0305,

  vid_build = 0x0401,
  vid_state = 0x0402,
  vid_encoder_lost = 0x0403,
  vid_stall = 0x0404,
  vid_caps = 0x0405,

  bit_poweron = 0x0501,
  bit_continuous = 0x0502,

  mqtt_connect = 0x0601,
};

struct Error {
  Code code;
  std::string_view where; // literal only - Error outlives no allocation
};

[[nodiscard]] constexpr std::uint16_t domain_of(Code c) noexcept {
  return static_cast<std::uint16_t>(c) & 0xFF00u;
}

} // namespace optronic
