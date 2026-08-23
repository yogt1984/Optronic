#pragma once

// One log record is a fixed 256 bytes with no indirection, so producing one
// costs a memcpy into a ring slot and nothing else - no allocation, no
// std::string, no formatting (SRS-LT-02). Formatting happens in the sink
// thread, where the cost does not sit in the frame path.

#include <cstdint>
#include <cstring>
#include <string_view>

namespace optronic::log {

enum class Level : std::uint8_t { trace = 0, debug = 1, info = 2, warn = 3, error = 4 };

[[nodiscard]] constexpr std::string_view level_name(Level l) noexcept {
  switch (l) {
  case Level::trace:
    return "TRACE";
  case Level::debug:
    return "DEBUG";
  case Level::info:
    return "INFO";
  case Level::warn:
    return "WARN";
  case Level::error:
    return "ERROR";
  }
  return "?????";
}

enum class ValueKind : std::uint8_t { none = 0, integer = 1, real = 2 };

struct KeyValue { // 24 bytes
  char key[15]{};
  ValueKind kind{ValueKind::none};
  std::int64_t value{}; // a double is stored here bit-for-bit; kind says which

  static KeyValue of(std::string_view k, std::int64_t v) noexcept;
  static KeyValue of(std::string_view k, double v) noexcept;
  [[nodiscard]] double as_real() const noexcept;
};

inline constexpr std::size_t kMaxKeyValues = 4;
inline constexpr std::size_t kRecordSize = 256;

struct Record {
  std::uint64_t ts_mono_ns{}; //   0
  std::uint64_t seq{};        //   8
  char component[16]{};       //  16
  char msg[96]{};             //  32
  KeyValue kv[kMaxKeyValues]; // 128
  Level level{Level::info};   // 224
  std::uint8_t kv_count{};    // 225

  // Room for the fields v1.1 adds - channel id, thread id - without changing
  // the record size, which would change the ring layout and every reader.
  char reserved[30]{}; // 226
};

static_assert(sizeof(Record) == kRecordSize, "the ring geometry depends on this");
static_assert(std::is_trivially_copyable_v<Record>, "records are memcpy'd into the ring");

// Truncating on purpose: a log call must never fail, allocate or block, so an
// over-long message loses its tail rather than its whole record.
void copy_truncated(char* dst, std::size_t dst_size, std::string_view src) noexcept;

} // namespace optronic::log
