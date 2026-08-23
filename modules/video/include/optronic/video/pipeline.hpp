#pragma once

// Owns one graph. The GStreamer types live behind a pimpl so that nothing
// outside this module needs <gst/gst.h> to include this header - the module
// boundary of SPEC-19 §1 enforced by the compiler rather than by convention.

#include "optronic/core/expected.hpp"
#include "optronic/video/frame.hpp"
#include "optronic/video/spec.hpp"

#include <cstdint>
#include <memory>

namespace optronic::video {

struct PipelineStats {
  std::uint64_t frames_in = 0;
  std::uint64_t frames_out = 0;
  std::uint64_t drops = 0;
};

class Pipeline {
public:
  // The sinks are references, not owners: they outlive the pipeline by
  // contract, which the lifecycle enforces by stopping video first (SPEC-01).
  [[nodiscard]] static expected<Pipeline> create(const PipelineSpec&, FrameSink&, BusSink&);

  Pipeline(Pipeline&&) noexcept;
  Pipeline& operator=(Pipeline&&) noexcept;
  Pipeline(const Pipeline&) = delete;
  Pipeline& operator=(const Pipeline&) = delete;

  // Destruction alone is enough: it brings the graph to NULL and joins the bus
  // thread, so a scope exit cannot leave a pipeline running.
  ~Pipeline();

  [[nodiscard]] status play();
  [[nodiscard]] status stop();

  [[nodiscard]] PipelineStats stats() const noexcept;

private:
  class Impl;
  explicit Pipeline(std::unique_ptr<Impl>) noexcept;
  std::unique_ptr<Impl> impl_;
};

} // namespace optronic::video
