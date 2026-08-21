# SPEC-12 Multi-channel video and synchronization

## 1. Channels
A node has N video channels (PERI/ATTICA-class sight: 2 — thermal + day; SETAS module: 1–2; central unit: 6–8). Each channel = one source, one processing chain, one or more outputs. The service owns **one pipeline per channel** plus optional **derived channels** (fusion, picture-in-picture) that consume two or more channels.
Config: `video.channels[]` with `id`, `source`, `format`, `fps`, `processor`, `outputs[]`. The demo builds two: `thermal` (videotestsrc pattern=ball, GRAY16 → GRAY8) and `day` (videotestsrc or v4l2src, NV12).

## 2. Time base
- One monotonic clock per node (`CLOCK_MONOTONIC_RAW`, mirrored into the GStreamer pipeline clock), disciplined to PTP when a grandmaster exists on the vehicle network, else free-running with the offset reported in telemetry.
- Every frame carries a **capture timestamp** from hardware (SPEC-04 `TS_LO/HI`) as `GstBuffer` PTS plus a `GstMeta` (`OptronicFrameMeta{channel_id, hw_ts_ns, frame_seq, exposure_us, gain}`). Software sources stamp at `appsink` arrival with a `SW_TS` flag.

## 3. Synchronization rules
- Derived channels pair frames by nearest `hw_ts_ns` within `sync_window_ms` (default ½ frame period); unmatched frames are dropped from the faster side and counted (`sync_dropped`).
- Channels are **not** frame-locked at the source (different detectors, different fps); alignment happens at the consumer. If the PL provides a common frame trigger, the window shrinks to jitter only.
- Latency is measured per channel; glass-to-glass (SPEC-13) is measured per output.

## 4. Interfaces
- `video::ChannelManager` — owns `Pipeline` instances, exposes `channel(id)`, `startAll()/stopAll()` honoring dependency (derived after sources), `rebuild(id, ModeSpec)` (SPEC-17).
- `video::FramePairer<N>` — template over N inputs, one lock-free single-slot mailbox per input, emits `std::array<FrameView, N>` to a `Processor`.
- Telemetry: `optronic/<name>/channel/<id>/latency`, `…/sync` (`{"paired":…, "dropped":…, "skew_ms_p95":…}`).

## 5. Requirements
- SRS-MC-01 N channels shall run independently; failure of one shall not stop another.
- SRS-MC-02 Derived channels shall pair frames within `sync_window_ms` and report skew p95.
- SRS-MC-03 Each frame shall carry capture timestamp and sequence number end-to-end, including into RTP (header extension or H.264 SEI) so the HMI can compute its own latency.
