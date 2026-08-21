# SPEC-10 Migration Plan — legacy → modern (the "aufbauen" items)

## 1. Starting point (tag `v0-legacy`, deliberately representative)
Flat `Makefile` with hardcoded `g++`/`-I`, C++14, one `main.cpp` (~900 lines) + 5 headers, raw `new/delete`, `pthread_create`, `printf` logging, globals for config, no tests, builds only on the author's machine, no container, no CI.

## 2. Commit series (tag `v1-modern` at the end)
| # | Commit | Why this position | Risk controlled |
|---|---|---|---|
| 1 | build: target-based CMake replacing Makefile | unblocks everything else; zero behaviour change, same flags | binary diff checked |
| 2 | build: CMakePresets + aarch64 toolchain file | host/target from one tree before anyone adds platform ifdefs | `arm64-release` builds |
| 3 | ci: PetaLinux SDK Docker image + pipeline (host tests → arm64-petalinux build → QEMU target tests → artifact) | freeze the environment before refactoring so regressions are visible; target ABI verified from day one | CI green gate from here on |
| 4 | test: GoogleTest scaffold + fake HAL backend + first 10 tests | safety net before touching logic | coverage on touched code |
| 5 | refactor: RAII/unique_ptr, remove new/delete, ASan/UBSan preset | memory safety with tests in place | sanitizer jobs |
| 6 | modernize: C++14 → C++20 | last, because it's the least urgent and most tempting | see §3 |
| 7 | feat: structured logger + MQTT telemetry | first *feature* only after foundations | |
| 8 | docs: this file + NUMBERS.md | | |

## 3. C++20 adoption — what and what not
Adopt now: `concepts` on HAL/Processor interfaces (replace `enable_if`), `std::span` for frame views, `std::jthread`/`stop_token`, `std::optional`/`std::expected` (C++23 via `tl::expected` if toolchain < 23), `std::format` where the toolchain has it, `constexpr`/`consteval` register layout checks, designated initializers for config structs, `[[nodiscard]]`, `<=>` on version types.
Do NOT adopt yet: modules (toolchain/CMake support uneven), coroutines (no async need; debugging story on target poor), ranges in hot paths (codegen verify first).
Rule: every construct must compile on GCC 12 and Clang 16 (SRS-BQ-04).

## 4. Transfer to a real codebase (what changes at the customer)
- Step 1 is the same, but with a *parallel* CMake build until parity is proven (bit-identical binaries or same test results), then remove the old build.
- Step 3 is the same image family; at the customer the SDK is produced from *their* BSP project (their machine, their IMAGE_INSTALL) instead of the reference zcu104/Kria BSP.
- Step 4 starts with characterisation tests around the code you must change, not with full coverage.
- Step 6 is done per-module behind PRs, never as one commit; `clang-tidy modernize-*` proposes, humans decide.
- Add: coding guidelines agreement (SPEC-11) before step 5, otherwise refactors get bike-shedded.

## 5. Effort observed (this repo)
~5 evenings, ~12 h. Numbers per commit in the git log; used as the basis for estimating the customer's migration (×10–20 for a real codebase, dominated by step 1 parity and step 4 characterisation).
