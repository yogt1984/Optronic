# SPEC-18 Configuration, calibration and persistence versioning

## 1. Three data classes
| Class | Examples | Written by | Location | Integrity |
|---|---|---|---|---|
| Factory / calibration | NUC base tables, bad-pixel map, lens distortion, boresight offsets, detector serial | production / calibration rig | `/factory` (read-only partition) | signed (SPEC-15), never modified in the field |
| Device configuration | channels, network, ICD port, mode defaults, thresholds | integrator / service | `/etc/optronic/config.json` (rootfs, A/B) | schema-validated; part of the signed image or a signed overlay |
| Runtime state | last mode, user gain, last NUC tables, counters, logs, recordings | the service | `/var/lib/optronic` (state partition) | best effort; checksummed; loss tolerated |

## 2. Versioning
- Every file carries `"$schema": "optronic/<class>/<major>.<minor>"`. The loader accepts equal major and ≤ minor; a higher minor is read with unknown keys ignored **only** for runtime state (strict for configuration).
- Migrations: `config::Migrator` chain `v1→v2→…` as pure functions `(json) -> expected<json>`, unit-tested with fixtures; run at startup before validation; the original is kept once as `*.bak.<ver>`.
- Calibration tables carry detector serial and timestamp; loading a table for a different serial fails power-on BIT.

## 3. Write discipline
- Atomic writes (`tmp` → `fsync` → `rename`); never a partial file after power loss.
- Runtime-state writes rate-limited (≤ 1 per 10 s) and batched; NUC tables written only after a successful sequence (SPEC-04 §3.1).
- Flash wear: logs go to a RAM ring and are flushed on WARN+ and at shutdown; recordings are size-capped with oldest-first deletion.

## 4. Interfaces
- `config::Store` — `load(path)`, `get<T>(key)`, `set(key, value) -> expected<void>` (runtime keys only), `subscribe(key, cb)`; `Verifier` hook (SPEC-15).
- `calib::Store` — `table(ChannelId, Kind) -> span<const uint16_t>`, `serial()`, `validateAgainst(detector_serial)`.
- `state::Store` — `save(key, bytes)`, `load(key)`; atomic, checksummed.

## 5. Requirements
- SRS-CV-01 Schema versions shall be enforced as above; mismatches name file, found and expected version.
- SRS-CV-02 All persistent writes shall be atomic.
- SRS-CV-03 Calibration for a foreign detector serial shall fail power-on BIT.
