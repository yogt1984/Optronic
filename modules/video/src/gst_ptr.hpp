#pragma once

// RAII over the GStreamer C API. Internal to modules/video: this is the only
// header in the tree that includes <gst/gst.h>, which is what keeps the
// dependency an implementation detail rather than a promise in a public header.

#include <gst/gst.h>

#include <cstddef>
#include <memory>
#include <span>

namespace optronic::video {

// Each GStreamer type has its own unref function; picking the wrong one is a
// leak or a double free, so the mapping is stated once here.
template <class T> struct GstUnref;

template <> struct GstUnref<GstElement> {
  void operator()(GstElement* p) const noexcept {
    if (p != nullptr)
      gst_object_unref(p);
  }
};

template <> struct GstUnref<GstBus> {
  void operator()(GstBus* p) const noexcept {
    if (p != nullptr)
      gst_object_unref(p);
  }
};

template <> struct GstUnref<GstSample> {
  void operator()(GstSample* p) const noexcept {
    if (p != nullptr)
      gst_sample_unref(p);
  }
};

template <> struct GstUnref<GstCaps> {
  void operator()(GstCaps* p) const noexcept {
    if (p != nullptr)
      gst_caps_unref(p);
  }
};

template <> struct GstUnref<GstMessage> {
  void operator()(GstMessage* p) const noexcept {
    if (p != nullptr)
      gst_message_unref(p);
  }
};

template <class T> using GstPtr = std::unique_ptr<T, GstUnref<T>>;

// gst_buffer_map has to be paired with gst_buffer_unmap on every path,
// including the early returns a frame callback is full of.
class MapGuard {
public:
  MapGuard(GstBuffer* buffer, GstMapFlags flags) noexcept : buffer_(buffer) {
    mapped_ = buffer_ != nullptr && gst_buffer_map(buffer_, &info_, flags) == TRUE;
  }

  ~MapGuard() noexcept {
    if (mapped_)
      gst_buffer_unmap(buffer_, &info_);
  }

  MapGuard(const MapGuard&) = delete;
  MapGuard& operator=(const MapGuard&) = delete;
  MapGuard(MapGuard&&) = delete;
  MapGuard& operator=(MapGuard&&) = delete;

  [[nodiscard]] bool ok() const noexcept { return mapped_; }

  [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
    if (!mapped_)
      return {};
    return {reinterpret_cast<const std::byte*>(info_.data), info_.size};
  }

  [[nodiscard]] std::span<std::byte> writable_bytes() const noexcept {
    if (!mapped_)
      return {};
    return {reinterpret_cast<std::byte*>(info_.data), info_.size};
  }

private:
  GstBuffer* buffer_ = nullptr;
  GstMapInfo info_{};
  bool mapped_ = false;
};

} // namespace optronic::video
