# SPEC-05 Configuration Schema v1

File: JSON, UTF-8, validated at startup (SRS-CF-01). Unknown keys → error (strict). `runtime: yes` = settable via ICD SET_PARAM without restart.

| Key | Type | Default | Range / enum | Runtime | Notes |
|---|---|---|---|---|---|
| `service.name` | string | "optronic" | ≤ 32 chars | no | used in logs/MQTT client id |
| `service.state_dir` | path | "/var/lib/optronic" | must be writable | no | SRS-NF-03 |
| `service.watchdog_ms` | uint | 1000 | 100..10000 | no | |
| `log.level` | enum | "info" | trace/debug/info/warn/error | yes | |
| `log.sink` | enum | "stderr" | stderr/journal/file | no | |
| `log.file` | path | — | required if sink=file | no | |
| `log.ring_entries` | uint | 4096 | power of 2, ≥ 256 | no | |
| `hal.backend` | enum | "fake" | fake/uio | no | |
| `hal.uio_device` | path | "/dev/uio0" | — | no | required if backend=uio |
| `hal.expect_id` | hex | 0x49535031 | — | no | BIT check |
| `sensor.gain` | uint | 256 | 0..4095 | yes | Q4.8 |
| `sensor.nuc_enable` | bool | false | — | yes | |
| `sensor.width` / `sensor.height` | uint | 1920 / 1080 | 64..4096 | no | |
| `video.source` | enum | "videotestsrc" | videotestsrc/v4l2src | no | |
| `video.v4l2_device` | path | "/dev/video0" | — | no | |
| `video.encoder` | enum | "x264enc" | x264enc/omxh264enc/vvas_xvcuenc | no | SRS-VP-02 |
| `video.bitrate_kbps` | uint | 4000 | 500..20000 | yes | |
| `video.fps` | uint | 30 | 1..120 | no | |
| `video.leaky_queue` | bool | true | — | yes | drop oldest on overload |
| `video.rtp_host` / `video.rtp_port` | string / uint | "127.0.0.1" / 5004 | — | no | |
| `video.processor` | enum | "passthrough" | passthrough/edge/blur | no | demo processors |
| `ipc.port` | uint | 5600 | 1024..65535 | no | |
| `ipc.bind` | string | "0.0.0.0" | — | no | |
| `health.frame_stall_ms` | uint | 2000 | 500..10000 | no | SRS-HB-04 |
| `health.degraded_fallback` | enum | "raw" | raw/none | no | SRS-HB-02 |
| `telemetry.enable` | bool | true | — | no | |
| `telemetry.broker` | string | "localhost" | — | no | |
| `telemetry.port` | uint | 1883 | — | no | |
| `telemetry.period_ms` | uint | 1000 | 100..60000 | yes | |
| `telemetry.tls` | bool | false | — | no | v1: plain only |

Example `demo.json`:
```json
{ "hal": {"backend": "fake"},
  "video": {"source": "videotestsrc", "encoder": "x264enc", "processor": "edge"},
  "telemetry": {"broker": "localhost"} }
```
Versioning: `"$schema_version": 1` optional; loader refuses a higher major.
