# SPEC-11 Coding Guidelines (C++20 subset for embedded Linux services)

1. **Standard**: C++20, GCC ≥ 12 / Clang ≥ 16; `-Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion`.
2. **Ownership**: RAII everywhere; `unique_ptr` default, `shared_ptr` only for genuinely shared lifetime (document why); raw pointers = non-owning, never `delete`. C handles wrapped with `unique_ptr<T, Deleter>` (`GstPtr`, `MosqPtr`).
3. **Errors**: `std::expected<T, Error>` for recoverable; exceptions only at startup (config) and for programmer errors; **never** across C callbacks, threads, or destructors. `[[nodiscard]]` on every `expected`.
4. **Concurrency**: `std::jthread` + `stop_token`; no detached threads; mutex scope = function scope via `scoped_lock`; condition variables always with predicate; atomics with explicit memory order and a comment on the pairing; lock order documented in SPEC-02 §7.
5. **Hot path** (frame callbacks, log producers): no allocation, no locks, no exceptions, no `std::string` construction; `span`/`string_view` in, fixed-size records out.
6. **Hardware access**: only via `hal::Register`; no `volatile` outside `hal/`; barriers explicit.
7. **Interfaces**: concepts for compile-time polymorphism (`Processor`, `MmioBackend`); virtual interfaces only where runtime swapping is needed (HAL backend selected by config).
8. **Types**: `enum class`; `std::chrono` for all durations; fixed-width ints at interfaces; `<=>` for version/ordering types.
9. **Layout**: one component per directory with `include/optronic/<comp>/…` + `src/`; tests next to component in `tests/<comp>/`.
10. **Style**: clang-format (LLVM base, 100 cols), `snake_case` functions/vars, `PascalCase` types, `k` prefix for constants off; no `using namespace` in headers.
11. **Docs**: every public header has a 3-line purpose comment; every design decision with a trade-off goes to `docs/DECISIONS.md` (ADR-style, 10 lines max).
12. **Commits**: conventional commits (`build:`, `ci:`, `test:`, `refactor:`, `feat:`, `docs:`), one concern per commit, build green at every commit.
