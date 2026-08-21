# SPEC-19 Interfaces designed around GStreamer

Principle: GStreamer is an implementation detail of `modules/video`. No other component includes `<gst/gst.h>`. The framework sees five narrow interfaces plus a factory.

## 1. `video::Pipeline` — ownership and lifecycle
```cpp
class Pipeline {                       // one per channel/output; non-copyable, movable
 public:
  static expected<Pipeline, Error> create(const PipelineSpec&, FrameSink&, BusSink&);
  expected<void, Error> play();        // NULL→PLAYING, waits for the state change ≤ 3 s
  expected<void, Error> pause();
  expected<void, Error> stop();        // PLAYING→NULL, joins the bus thread
  expected<void, Error> setProperty(std::string_view element, std::string_view prop, PropValue); // hot changes
  PipelineStats stats() const;         // frames, drops, per-stage latency p50/p95
  ~Pipeline();                         // guarantees NULL state and unref
 private:
  GstPtr<GstElement> pipeline_;        // unique_ptr<T, GstUnref>
  std::jthread bus_thread_;
};
```
Rules: every `GstElement/GstBus/GstCaps/GstSample` lives in a `GstPtr`; `gst_buffer_map` only through `MapGuard` (RAII unmap). No exception may cross a GStreamer callback; callbacks return `GstFlowReturn` and post events to the owner.

## 2. `video::FrameSink` / `FrameView` — frames out of the pipeline (appsink)
```cpp
struct FrameView {                     // non-owning; valid only inside onFrame()/process()
  std::span<const std::byte> data; uint32_t width, height, stride; PixelFormat fmt;
  uint64_t hw_ts_ns; uint64_t seq; ChannelId ch;
};
struct FrameSink { virtual FlowResult onFrame(FrameView) noexcept = 0; };  // called on the streaming thread
```
The `appsink` `new-sample` callback pulls the sample, maps read-only via `MapGuard`, builds the `FrameView` (timestamps from `OptronicFrameMeta` or buffer PTS), calls `onFrame`, unmaps, unrefs. `appsink` properties: `sync=false max-buffers=1 drop=true emit-signals=false` (pulled from a dedicated thread when the processor may block).

## 3. `video::FrameSource` — frames into the pipeline (appsrc)
```cpp
class FrameSource {                    // wraps appsrc; used by processors that produce output frames
 public:
  expected<WritableFrame, Error> acquire();                    // buffer from a GstBufferPool, mapped RW via MapGuard
  expected<void, Error> push(WritableFrame&&, uint64_t pts_ns); // sets PTS/duration/meta, gst_app_src_push_buffer
  void endOfStream();
};
```
Buffers come from a `GstBufferPool` sized 3–4, so no per-frame allocation; caps fixed at create (`format=NV12, width, height, framerate`). `is-live=true format=time block=false`; when downstream is full `push` returns `E_VID_BACKPRESSURE` and the processor drops.

## 4. `video::Processor` — the pluggable stage
```cpp
template <class P> concept Processor = requires(P p, FrameView in, WritableFrame& out) {
  { p.process(in, out) } noexcept -> std::same_as<ProcessResult>;   // kept | dropped | error
  { p.budget() } -> std::same_as<std::chrono::microseconds>;
  { P::name } -> std::convertible_to<std::string_view>;
};
```
Implementations: `Passthrough`, `EdgeOverlay` (OpenCV), `YoloBoxes` (OpenCV DNN, optional), `Fusion2` (consumes `std::array<FrameView, 2>` via `FramePairer`). Processors never touch GStreamer types. A runtime registry maps config names to factories. `process()` runs on the streaming thread, or in a worker with a one-slot mailbox when `budget()` exceeds the frame period.

## 5. `video::BusSink` — errors, warnings and state into the framework
```cpp
struct BusSink { virtual void onBus(BusEvent) noexcept = 0; };   // Error{element, code, text} | Warning | Eos | StateChanged | QosDrop | LatencyUpdated
```
A dedicated bus thread (`gst_bus_timed_pop_filtered`) translates `GstMessage` into `BusEvent`. `health::Monitor` subscribes and maps: encoder ERROR → `ENCODER_LOST`, source ERROR → `SOURCE_LOST`, QoS drops → `sync_dropped`, latency messages → `LatencyTracker`.

## 6. `video::PipelineFactory` and `PipelineSpec` — describing graphs without building them
`PipelineSpec{channel, source{kind, device, caps}, processor{name, params}, encoder{kind, bitrate, gop, lowLatency}, outputs[{kind: rtp | kms | file | appsink, …}], queues{leaky, maxBuffers}}`.
`PipelineFactory::launchString(spec)` yields a deterministic `gst-launch` string plus property map (golden-file tested, dry-run on the host); `build(spec)` instantiates via `gst_parse_launch` and fetches named elements (`appsink name=out`, `enc`, `q_live`) for runtime access. Element choice by platform table: `encoder.kind=h264` → `x264enc` (host) / `omxh264enc` / `vvas_xvcuenc` (target), selected by configuration, not `#ifdef`.

## 7. Probes and telemetry hooks
Pad probes on the `appsink` sink pad (t0), after the processor (t1) and the encoder src pad (t2) read PTS and `hw_ts_ns` and feed the `LatencyTracker` (lock-free per stage). `GST_DEBUG_DUMP_DOT_DIR` graph export on `nodectl dump-graph` for documentation and field diagnosis.

## 8. Threading contract
| Thread | Owner | Must not |
|---|---|---|
| GStreamer streaming threads (one per source/queue) | GStreamer | block on framework locks; allocate; throw |
| bus thread | `Pipeline` | change pipeline state synchronously (post to the owner instead) |
| processor worker (optional) | processor runner | hold a `FrameView` across iterations |
| owner/control thread | `ChannelManager` / `ModeController` | be called from streaming threads |

## 9. Inside GStreamer (custom elements) vs outside
Inside (GObject, C, under `modules/video/elements/` if ever needed): hardware-specific zero-copy steps (PL M2M, VCU), or a stage that must sit between two hardware elements without a CPU round-trip. Outside (C++ processors via appsink/appsrc): everything else — overlay, analytics, CPU fusion, anything that needs the framework (config, health, logging). Default is outside; a custom element needs a written justification in latency or copy count.
