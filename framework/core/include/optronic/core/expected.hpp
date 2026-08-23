#pragma once

// One alias so the whole framework spells recoverable failure the same way.
// std::expected is C++23; when the toolchain floor moves this file is the only
// thing that changes.

#include "optronic/core/error.hpp"

#include <expected>

namespace optronic {

template <class T> using expected = std::expected<T, Error>;
using status = std::expected<void, Error>;

[[nodiscard]] inline std::unexpected<Error> fail(Code c, std::string_view where) {
  return std::unexpected(Error{c, where});
}

} // namespace optronic
