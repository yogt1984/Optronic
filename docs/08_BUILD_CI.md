# SPEC-08 Build, Toolchains, Containers, CI

## 1. CMake layout
Top-level `CMakeLists.txt` declares the project (C++20) and adds `framework/`, `modules/`, `tools/`, `tests/`. One library target per component (`optronic::log`, `optronic::hal`, …), `target_link_libraries` with PRIVATE/PUBLIC discipline; GStreamer linked PRIVATE into `optronic::video` only; mosquitto PRIVATE into `optronic::telemetry` only. No `include_directories()`, no `file(GLOB)`, no `CMAKE_CXX_FLAGS` edits — warnings via an `optronic::warnings` INTERFACE target.

Options: `OPTRONIC_BUILD_TESTS` (ON host / OFF target), `OPTRONIC_FAULT_INJECT` (ON debug), `OPTRONIC_WITH_MQTT` (ON), `OPTRONIC_WITH_GST` (ON; OFF builds framework only).

## 2. Presets (`CMakePresets.json`)
| Preset | Purpose |
|---|---|
| `host-debug` | Ninja, Debug, tests, fault-inject |
| `host-asan` | + `-fsanitize=address,undefined` |
| `host-tsan` | + `-fsanitize=thread` |
| `host-release` | RelWithDebInfo, tests |
| `arm64-petalinux` | **primary target preset** — PetaLinux SDK toolchain + sysroot from the build image (`environment-setup-cortexa72-cortexa53-xilinx-linux`), RelWithDebInfo, tests built for on-target/QEMU run, `-march=armv8-a+crc`, LTO |
| `arm64-debian` | fallback for reviewers without the PetaLinux installer: `crossbuild-essential-arm64`, same toolchain file, no sysroot guarantees |

## 3. Toolchain file
`cmake/toolchain-aarch64.cmake`: `CMAKE_SYSTEM_NAME Linux`, `CMAKE_SYSTEM_PROCESSOR aarch64`. If `OECORE_TARGET_SYSROOT` is set (PetaLinux/Yocto SDK sourced) it takes `CC/CXX/SYSROOT` from the environment and sets `CMAKE_FIND_ROOT_PATH_MODE_*` ONLY; otherwise it falls back to `aarch64-linux-gnu-g++` (Debian). One file, two worlds, no `if(CUSTOMER)` branches.

## 4. Docker — PetaLinux-based build and test image (primary)
Docker runs on build/CI hosts only, never on the target. The image freezes the AMD-supported host environment so laptops and runners build identically.

### 4.1 `docker/Dockerfile.petalinux` (base, built once per tool release)
- `FROM ubuntu:22.04` (on AMD's supported list for PetaLinux 2023.2/2024.1); `SHELL ["/bin/bash","-c"]`; locale `en_US.UTF-8`.
- Host packages per UG1144 (gawk, chrpath, texinfo, diffstat, xterm-less set, zlib1g:i386 via `dpkg --add-architecture i386`, python3, cpio, rsync, libtinfo5, …).
- Non-root user `dev` (PetaLinux refuses root); `/opt/petalinux` owned by `dev`.
- Installer is **not in the repo** (login-gated, non-redistributable): it is passed as a build secret / bind mount: `docker build --secret id=plnx,src=petalinux-v2024.1-final-installer.run …` → `RUN --mount=type=secret,id=plnx … ./installer -y -d /opt/petalinux`.
- `ENV PETALINUX=/opt/petalinux`; `source $PETALINUX/settings.sh` in the entrypoint.
- Volumes: `/sstate` and `/downloads` (Yocto caches, tens of GB) and `/work` (project). Without them every image build is hours.
- Tag: `optronic/plnx:2024.1`. Reproducibility = tool version in the tag + pinned base image digest.

### 4.2 `docker/Dockerfile.sdk` (derived, what developers and app-CI use)
- `FROM optronic/plnx:2024.1`; builds the BSP project for the chosen machine (`zcu104`/`kria-k26`/SoM vendor BSP) and runs `petalinux-build --sdk && petalinux-package --sysroot` once; installs the resulting `sdk.sh` into `/opt/sdk` and **discards the BSP tree** (multi-stage) → a 2–3 GB image containing: `aarch64-xilinx-linux-g++`, `gdb` + `gdbserver`, the **target sysroot** (glibc, glib, GStreamer 1.x + xlnx plugins, libdrm, libv4l, OpenAMP, mosquitto — whatever `IMAGE_INSTALL` had), `pkg-config` files, `environment-setup-*`, `qemu-system-aarch64` from PetaLinux, and `BOOT.BIN`/`Image`/`rootfs.cpio` of the matching QEMU machine for L3 tests.
- Also installs host tooling: cmake ≥ 3.25, ninja, gcc-12/clang-16 (host tests), googletest, clang-format/tidy, ccache.
- Entrypoint runs the full sequence: `cmake --preset host-debug && ctest` → `cmake --preset arm64-petalinux && cmake --build` → `ctest --preset arm64-qemu` (see §6).
- Tag `optronic/sdk:2024.1`. This is the image the CI jobs use.

### 4.3 `docker/Dockerfile.debian` (fallback)
Debian bookworm + `crossbuild-essential-arm64` + multiarch GStreamer/mosquitto dev packages. Used only when the PetaLinux installer is unavailable (public reviewers). Produces a binary that runs on a generic aarch64 Debian, **not** guaranteed against the PetaLinux sysroot. README states this plainly.

### 4.4 `docker/Dockerfile.run`
Runtime image for host demos (amd64): runtime libs only, `USER nobody`, read-only FS compatible. Not used on the target — the target runs the Yocto image.

One-liner for a machine that has the installer: `make docker-sdk && docker run --rm -v $PWD:/work optronic/sdk:2024.1`.

## 5. CI (GitHub Actions self-hosted runner / GitLab CI / Jenkins — same stages)
```
image  : build optronic/plnx + optronic/sdk when docker/ or tool version changes (manual/weekly; needs the installer secret on the runner)
lint   : clang-format --dry-run, dependency-rule grep (SPEC-02 §1), docs regen check (register map)
host   : host-debug + ctest; host-asan + ctest; host-tsan + ctest (gcc-12 / clang-16 matrix)     — in optronic/sdk
cross  : cmake --preset arm64-petalinux → build → strip → artifact optronic-aarch64.tar.gz        — in optronic/sdk
qemu   : boot PetaLinux QEMU (zcu104 machine) from the SDK image, scp binary + tests, run L3 smoke + on-target unit tests via ssh, collect junit — in optronic/sdk
board  : (optional, lab runner tagged `zynq`) same as qemu against a real board over ssh; nightly
```
All stages share the ccache volume keyed on compiler + preset. Artifacts: arm64 tarball, junit for host/qemu/board, NUMBERS.md diff.

## 6. QEMU test preset
`ctest --preset arm64-qemu` wraps: `petalinux-boot --qemu --kernel` (or direct `qemu-system-aarch64 -M xlnx-zcu102 …` with the images from the SDK stage), waits for ssh, copies `build-arm64/bin/*` and `build-arm64/tests/*`, runs the GoogleTest binaries with `--gtest_output=xml`, fetches results. UIO tests run against a `uio_pdrv_genirq` device declared in the QEMU device tree overlay (a plain memory region; no PL behaviour — validates mapping/irq plumbing only). GStreamer tests use `videotestsrc` + `x264enc` (software) in QEMU; VCU paths are skipped with a reason.

## 7. Yocto hook
`recipes-optronic/optronic/optronic_git.bb`: `inherit cmake pkgconfig systemd`, `DEPENDS = "gstreamer1.0 gstreamer1.0-plugins-base mosquitto nlohmann-json"`, `EXTRA_OECMAKE = "--preset arm64-petalinux -DOPTRONIC_BUILD_TESTS=OFF"`, installs `optronic.service` with `WatchdogSec=3`. Added to the PetaLinux project as `project-spec/meta-user` bbappend or, preferably, its own `meta-optronic` layer.

## 8. Versioning
SemVer from git tag via `git describe`, baked into `version.hpp` at configure; exposed in HELLO_ACK and `health` topic. The PetaLinux tool version used for the build is recorded alongside (`OPTRONIC_SDK_VERSION`) and printed at startup.

## 9. Why PetaLinux-in-Docker and what it costs
- PetaLinux ≥ 2019 is a CLI over Yocto (meta-xilinx, meta-xilinx-tools, meta-petalinux); the image is therefore also a plain-Yocto image — switching to kas/bitbake later changes the base stage only.
- Pro: identical toolchain and sysroot to the product image → ABI-safe binaries, no "works on Debian, crashes on target"; QEMU from the same release for free; AMD answer-record fixes apply directly.
- Cost: installer download (~4 GB) and licence click-through per developer/runner, 50–100 GB caches, first image build ~1–2 h. Mitigated by building the base image once and sharing the SDK image via the company registry.
- Installer cannot be committed or published; the repo documents the build and ships the Debian fallback so the pipeline shape is still reviewable without AMD credentials.
