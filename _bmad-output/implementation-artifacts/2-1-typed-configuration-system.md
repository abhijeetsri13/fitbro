# Story 2.1: Typed configuration system

Status: ready-for-dev

## Story

As an operator,
I want all behavior driven by typed config (file + env),
so that the same binary runs across dev/paper/live by configuration only. (FR-34)

## Acceptance Criteria

1. **Given** a TOML config (toml++) + env overrides **When** the app loads **Then** config parses into a
   validated typed struct; invalid config fails fast with a clear, field-named message.
2. **And** secrets are read only from env/secret-provider, never from the TOML (a token-shaped or
   secret-named key appearing in the TOML is rejected at load).
3. **And** the same source runs dev/paper/live-small/live-prod with only config changed (a `mode`/profile
   selector + per-environment overrides, no recompilation).

## Tasks / Subtasks

- [ ] Task 1: Add `tomlplusplus` to `conanfile.py` requirements; wire `find_package(tomlplusplus)` in root CMake. (AC: 1)
- [ ] Task 2: Define the typed config schema `include/broker_exec/config/config.hpp` (AC: 1, 3)
  - [ ] Immutable value struct(s): top-level `Config` composed of nested sections
        (e.g. `engine`/`broker`/`risk`/`paths`/`logging`) — only the fields needed now; extensible later.
  - [ ] A `TradingMode`-style selector reusing the existing `domain/enums.hpp` enum if present
        (live/paper/dry-run/...); otherwise a local `RunProfile` enum. Do NOT duplicate an existing enum.
- [ ] Task 3: Loader `src/config/config.cpp` — `load(toml_path, env_lookup)` returning `std::expected<Config, Error>` (AC: 1, 2, 3)
  - [ ] Layer order: built-in defaults → TOML file values → env overrides (env wins).
  - [ ] Env override convention: `BROKER_EXEC_<SECTION>_<FIELD>` (uppercase), documented in code.
  - [ ] Validation: required fields present, enums parse, numeric ranges sane, paths well-formed.
        On failure return a typed `Error` (taxonomy) naming the offending field — never throw to caller.
  - [ ] Secret rejection: if the TOML contains any key matching the secret denylist
        (e.g. `*token*`, `*secret*`, `*password*`, `*api_key*`, `mpin`, `totp`) → fail fast with a clear error.
        Secrets are sourced via the injected `env_lookup` / future `SecretProvider`, never the TOML.
- [ ] Task 4: Inject the environment as a seam (`std::function<std::optional<std::string>(std::string_view)>`
      or a small `EnvSource` interface) so tests are deterministic and no `std::getenv` is called in the hot path
      outside that seam. (AC: 1, 2)
- [ ] Task 5: `config/CMakeLists.txt` — `broker_exec_config` target + alias, link `tomlplusplus::tomlplusplus`,
      `broker_exec::errors`, `broker_exec::domain` (PRIVATE warnings+sanitizers); `broker_exec_config_tests`. (AC: all)
- [ ] Task 6: Tests `src/config/config_test.cpp` (Catch2) (AC: 1, 2, 3)
  - [ ] Valid TOML + env override → fields resolve, env wins over file.
  - [ ] Invalid value (bad enum / out-of-range / missing required) → fail-fast Error naming the field.
  - [ ] A secret-shaped key in TOML → rejected.
  - [ ] Same TOML + two different env profile sets → two different validated Configs (one-binary proof).
- [ ] Task 7: Add a non-secret `config/example.toml` sample under the repo (documents the surface; secrets via env).

## Dev Notes

- **Module location:** `src/config/` + `include/broker_exec/config/`. Mirrors architecture source tree
  (`config/settings.py` → C++ `config/`). [Source: architecture.md#Source tree]
- **Library:** toml++ (Conan `tomlplusplus`), typed config struct, env via the injected seam (not raw getenv).
  [Source: architecture.md#Library choices — "TOML config: toml++ + a typed config struct; env via std::getenv"]
- **DA-5:** Layered defaults → TOML file → env; typed/validated; **secrets only via env/secret-provider, never in the TOML.**
  [Source: architecture.md#Data Architecture DA-5]
- **Errors:** return `std::expected<Config, Error>`; use the existing taxonomy (`include/broker_exec/errors/error.hpp`,
  `expected.hpp`). Pick the closest existing `SuggestedAction` (e.g. block-strategy / do-not-retry — fail-closed config).
  Never throw across the boundary. [Source: docs/conventions.md#Errors]
- **Cross-platform:** no `#ifdef`/POSIX outside `src/platform/`; use `std::filesystem` for paths; the only env access
  is through the seam. [Source: docs/conventions.md#Cross-platform]
- **Immutability:** config value types immutable after load (value structs, const accessors). [Source: architecture.md#Patterns]
- **Reuse:** check `include/broker_exec/domain/enums.hpp` for an existing TradingMode/mode enum and reuse it rather
  than defining a new one. Check `errors/error.hpp` for the `Error`/`SuggestedAction` shape before inventing.

### Project Structure Notes

- New module `config` is additive; append one `add_subdirectory(src/config)` to root `CMakeLists.txt` in the
  Wave-2/Epic-2 region. Self-contained module CMake so it doesn't collide with other modules.
- `domain` and `ports` must not gain a dependency on `config`; `config` may depend on `domain`+`errors` (inward only).

### References

- [Source: _bmad-output/planning-artifacts/epics.md#Story 2.1: Typed configuration system]
- [Source: _bmad-output/planning-artifacts/architecture.md#DA-5] [#Library choices] [#Source tree]
- [Source: docs/conventions.md]

## Dev Agent Record

### Agent Model Used

Opus 4.8 (1M context) — BMAD dev agent.

### Debug Log References

Build not run by the dev agent (orchestrator owns the build).

### Completion Notes List

- New module `broker_exec::config`. Immutable typed `Config` with nested sections
  (engine/broker/risk/paths/logging). No existing mode enum in `domain/enums.hpp`,
  so a local `RunProfile { DryRun, Paper, Live, Replay }` enum owns the selector
  (DryRun is the safe default).
- Loader `load(toml_path, env)` -> `Result<Config>`, layering defaults -> TOML
  (toml++) -> env (env wins). Env convention `BROKER_EXEC_<SECTION>_<FIELD>`.
- Validation returns a Validation/DoNotRetry `Error` naming the offending field;
  nothing throws (toml++ parse errors are caught internally).
- Secret denylist scan (token/secret/password/api_key/mpin/totp, case-insensitive,
  recursive incl. arrays-of-tables) runs before any value is read.
- Env seam is an injected `std::function`; `default_env_lookup()` is the only
  `std::getenv` call site. No `#ifdef`/POSIX; `std::filesystem` for paths.

### File List

- include/broker_exec/config/config.hpp (new)
- src/config/config.cpp (new)
- src/config/config_test.cpp (new)
- src/config/CMakeLists.txt (new)
- config/example.toml (new)
- CMakeLists.txt (modified: find_package(tomlplusplus), add_subdirectory(src/config))
- conanfile.py (modified: tomlplusplus/3.4.0 requirement)
