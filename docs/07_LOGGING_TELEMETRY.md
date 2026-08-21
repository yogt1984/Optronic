# SPEC-07 Logging, Latency Markers, Telemetry

## 1. Log record
`ts_mono_ns u64 · ts_wall (formatted at sink) · level u8 · component char[16] · msg char[96] · kv[0..4]{key char[16], value i64|f64|str32}` — fixed size (256 B) so the ring never allocates (SRS-LT-02). Formatting happens in the sink thread.

Levels: trace, debug, info, warn, error. Runtime changeable (`log.level`). Per-component override via `log.level_overrides` (v1.1).

## 2. Transport
Per producer thread: SPSC ring (`log.ring_entries`, power of 2), acquire/release indices, overwrite-oldest with a dropped counter. Sink thread drains all rings, writes to stderr/journald/file; line format
`2026-08-26T13:02:11.482Z INFO  video      pipeline PLAYING  src=videotestsrc enc=x264enc`.
Dropped records are reported once per second as `WARN log dropped=N`.

## 3. Latency markers
`time::LatencyTracker` with three marks per frame: t0 appsink receive, t1 processor done, t2 encoder output (pad probe). Rolling window 256 frames → p50/p95/max in µs; exposed via STATUS (ICD) and telemetry. Overhead target < 100 ns per mark (two `steady_clock::now()` and an array write).

## 4. MQTT telemetry
Client id = `service.name`. QoS 0 for periodic, QoS 1 for events. Retain on `…/health`.
| Topic | Period | Payload (JSON) |
|---|---|---|
| `optronic/<name>/health` | 1 s, retain | `{"state":"OK","uptime_s":123,"faults":0}` |
| `optronic/<name>/latency` | 1 s | `{"cap_p95_us":1800,"proc_p95_us":900,"enc_p95_us":12000,"fps":29.97,"dropped":0}` |
| `optronic/<name>/bit` | on run | SPEC-06 §4 result |
| `optronic/<name>/event` | on event | `{"id":"ENCODER_LOST","code":1027,"ts":...,"text":"..."}` |
| `optronic/<name>/sensor` | 1 s | `{"gain":256,"nuc":false,"temp_mc":41250,"frame_cnt":9021}` |
Last-will: `health` = `{"state":"OFFLINE"}`. Broker loss → reconnect with backoff 1…30 s, WARN once.

## 5. What is deliberately not done in v1
TLS/auth (config flag reserved), sparse logging to flash with wear awareness, remote log level via MQTT.
