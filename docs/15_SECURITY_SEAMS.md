# SPEC-15 Security seams (v1 plain; where each protection attaches)

A deployed sight runs secure boot, signed software and authenticated links. The demo keeps v1 plain but makes the seams explicit so each protection is a bounded change, not a redesign.

| Asset | v1 | Seam / planned mechanism |
|---|---|---|
| Boot chain | PetaLinux default, unsigned | CSU RSA-4096 authentication of FSBL/PMU-FW/ATF/U-Boot, AES-256 encrypted bitstream, keys in eFuse; `bootgen` BIF kept under `docker/`; FIT image signature verified by U-Boot |
| Application binaries / rootfs | plain eMMC | read-only root with dm-verity; A/B slots; update bundle signed (ed25519) and verified by the updater before the slot switch |
| Configuration and calibration | JSON, unsigned | `config::Store` loads through a `Verifier` interface (v1: no-op); detached signatures for factory/calibration data; runtime parameters are never persisted without signature |
| Control link (ICD) | plain TCP, single client | `ipc::Transport` interface; v2: mTLS (mbedTLS/OpenSSL) or link-layer authentication per vehicle architecture; sequence number + HMAC per message as the non-TLS fallback |
| Telemetry (MQTT) | plain port 1883 | `telemetry.tls=true` → TLS 1.2+, client certificates, topic ACLs on the broker |
| Debug interfaces | `INJECT_FAULT`, `nodectl` | compiled out in release (`OPTRONIC_FAULT_INJECT=OFF`); UART console disabled in the production device tree; JTAG disabled via eFuse |
| Logs | plain text on eMMC | no secrets in logs (lint rule); export only over the authenticated control link |
| Supply chain | Debian / PetaLinux packages | SBOM from Yocto (`create-spdx`), pinned layer revisions, reproducible-build check in CI |

Rules: no cryptography inside the framework itself — it consumes interfaces (`Verifier`, `Transport`) so a certified implementation can be dropped in. Threat model and STRIDE table are customer documents; the repo only marks where each control lives.
