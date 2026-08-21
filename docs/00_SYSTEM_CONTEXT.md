# SPEC-00 System Context

## 1. Purpose
`Optronic` is a Linux user-space service that owns the video pipeline and the cross-cutting functions of an EO/IR sensor node: lifecycle, configuration, logging, control interface, hardware abstraction, health/BIT, telemetry.

## 2. Context diagram
```
 Operator / C2 station ──TCP control (ICD, SPEC-03)──►┐
                      ◄──RTP/H.264 video──────────────┤
 Fleet / test rig     ◄──MQTT telemetry (SPEC-07)─────┤  Optronic (APU, Linux)
                                                      │   ├─ framework (lifecycle, config, log, ipc, hal, health, time)
 PL "ISP" block ◄──AXI4-Lite regs via UIO (SPEC-04)───┤   └─ modules (video, sensor, telemetry)
 V4L2 / videotestsrc ──frames (dma-buf / sysmem)─────►┘
 R5 / RPU  ◄──RPMsg (NOT in scope, interface reserved)
```

## 3. Assumptions about the target (unconfirmed — from JD inference)
- SoC: Xilinx Zynq UltraScale+ MPSoC (EV, VCU available) on a vendor SoM on a custom carrier. Alternatives (Zynq-7000, Versal, Kria) change nothing in this software except the encoder element.
- OS: Yocto/PetaLinux Linux, aarch64, GCC ≥ 12, GStreamer 1.18+ with Xilinx plugins.
- PL exposes processing blocks as AXI4-Lite register banks reachable via UIO.
- Control link is Ethernet; video leaves via RTP; telemetry via MQTT to a broker on the LAN.

## 4. Scope
In: everything in the framework/ and modules/ directories; host build with fakes; aarch64 cross build with the PetaLinux SDK inside Docker; target tests in PetaLinux QEMU; CI; docs.
Out: PL design, kernel drivers, R5 firmware, secure boot, OTA, operator UI, real detector support, CUDA.

## 5. Glossary
APU application processing unit (A53) · RPU real-time unit (R5) · PL programmable logic · VCU video codec unit · UIO userspace I/O · ICD interface control document · BIT built-in test · NUC non-uniformity correction · SoM system on module · HIL hardware-in-the-loop.
