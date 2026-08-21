# SPEC-17 Mode management and live reconfiguration

## 1. Operator modes (per output)
`DAY`, `THERMAL_WHITE_HOT`, `THERMAL_BLACK_HOT`, `FUSED` (day + thermal blend), `PIP` (thermal inset on day); orthogonal: `ZOOM {1x, 2x, 4x digital}`, `OVERLAY {off, reticle, full symbology}`, `RECORD {off, on}`, `FREEZE`.
A `ModeSpec` is a small value type `{view, zoom, overlay, record, freeze}`, validated against channel capabilities (`hasThermal`, `hasDay`, `maxZoom`).

## 2. Transition rules
- Mode changes arrive from the ICD (`SET_MODE`, type 0x0008, payload `ModeSpec`) or from local controls via the R5.
- Two classes. **Hot**: no pipeline rebuild — zoom, overlay, freeze, white/black-hot LUT are property changes on running elements or register writes. **Cold**: rebuild — switching the source channel, enabling fusion, toggling the record branch.
- Cold transitions build the new pipeline **beside** the old one up to PAUSED, swap the output (`input-selector` in front of the sink, or pad re-link), then tear the old one down. Operator gap ≤ 500 ms, last frame held during the swap, never black.
- One transition in flight per output; further requests coalesce, the latest wins.
- A failed transition rolls back to the previous `ModeSpec` and reports `E_VID_STATE` with the failing element.

## 3. State exposure
`STATUS` gains `current_mode` per output and `transition_in_progress`. Telemetry topic `…/mode`, retained.

## 4. Interfaces
- `video::ModeController` — `request(output_id, ModeSpec) -> expected<TicketId>`, `current(output_id)`; events `ModeChanged{ticket, spec, duration_ms}` / `ModeFailed{ticket, error}`.
- `video::PipelineFactory` — `build(ChannelSpec, ModeSpec) -> expected<unique_ptr<Pipeline>>`; a pure function of the specs, so it is testable without hardware (dry-run returns the launch string and element property map).

## 5. Requirements
- SRS-MM-01 Hot mode changes shall apply within one frame period without frame loss.
- SRS-MM-02 Cold mode changes shall complete within 500 ms with the last frame held; never a black frame.
- SRS-MM-03 Failed transitions shall roll back and report the cause.
- SRS-MM-04 `PipelineFactory` shall be testable in dry-run on the host against golden files.
