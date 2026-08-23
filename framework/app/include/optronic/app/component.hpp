#pragma once

// What the lifecycle needs from anything it owns. Deliberately four functions:
// a component that needs more than this is really two components.

#include "optronic/core/expected.hpp"

#include <string_view>

namespace optronic::app {

class Component {
public:
  virtual ~Component() = default;

  // Named in the log when startup aborts, so a failure says which component
  // (SRS-LC-03). Must be a literal - it is read during teardown.
  [[nodiscard]] virtual std::string_view name() const noexcept = 0;

  // Acquire resources. May fail; failing here aborts startup and everything
  // already initialised is torn down in reverse.
  [[nodiscard]] virtual status init() = 0;

  // Begin doing work: threads start here, not in init(), so that a half-built
  // system never has threads running against uninitialised peers.
  [[nodiscard]] virtual status start() = 0;

  // Must be safe to call on a component that was initialised but never
  // started, and safe to call twice. Teardown is not allowed to fail: there
  // is nothing sensible a caller could do with the error.
  virtual void stop() noexcept = 0;

protected:
  Component() = default;
  Component(const Component&) = default;
  Component& operator=(const Component&) = default;
};

} // namespace optronic::app
