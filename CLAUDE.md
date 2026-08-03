# http_request_async — Project context for AI agents

This file is the single source of truth for any agent (or human) picking up this
project. Read it before touching anything.

---

## What this project is

An ESPHome **external component** that replaces the built-in `http_request` for
**ESP32 + ESP-IDF** targets. The built-in component blocks the entire ESPHome main
loop for the duration of each HTTP request (50–4500 ms). This component runs
requests on a dedicated FreeRTOS worker task instead, keeping the main loop free.

YAML interface intentionally mirrors `http_request` for easy migration.

---

## Key design decisions (do not reverse without discussion)

### Async mechanism

The action overrides `play_complex()` rather than `play()`. It enqueues the
request and returns immediately — the automation chain is paused. When `loop()`
drains the response queue, it:
1. Fires `on_response_cb` or `on_error_cb` (the YAML trigger)
2. Calls `on_complete_cb` which resumes the automation chain

This is identical to how ESPHome's `DelayAction` works. It means sequential
scripts (`http post` → `script.execute`) work correctly — the second step does
not start until the HTTP response arrives.

### Connectivity check in `enqueue_request()`, not `execute_request_()`

`execute_request_()` is virtual and overridable (mocks do override it). The
network connectivity check therefore lives in `enqueue_request()` so it cannot
be bypassed by subclasses. `execute_request_()` has a secondary best-effort check
for the case where WiFi drops between enqueue and execution.

### `alive_` shared flag in the action

`stop()` sets `*alive_ = false`. All three lambdas stored in `PendingRequest`
check `*alive` before doing anything. This prevents stale callbacks from firing
if an automation is aborted mid-flight. A new `shared_ptr<bool>` is created per
`play_complex()` call so concurrent invocations don't interfere.

### ESP-IDF only

No Arduino support. No `#ifdef` maze. The component is guarded with
`#ifdef USE_ESP32` / `#endif`. Attempting to use it on ESP8266 or without
ESP-IDF will fail at the schema validation level (`cv.only_on_esp32`).

---

## Project structure

```
http_request_async/
├── CLAUDE.md                       ← you are here
├── Makefile                        ← ALL dev operations go through here
├── README.md                       ← user-facing documentation
├── .gitignore
├── .claude/
│   └── settings.json               ← pre-authorized Bash commands for agents
│
├── components/
│   └── http_request_async/
│       ├── __init__.py             ← ESPHome Python config schema + codegen
│       ├── http_request_async.h    ← C++ types, component class, action template
│       └── http_request_async_idf.cpp ← IDF worker task + esp_http_client
│
├── tests/
│   ├── README.md                   ← testing strategy (read this)
│   ├── python/
│   │   └── test_init.py            ← pytest: Python schema/codegen tests
│   ├── native/
│   │   ├── CMakeLists.txt          ← GoogleTest build
│   │   ├── main.cpp                ← C++ unit tests
│   │   ├── idf_http_mock.h         ← controllable mock state for esp_http_client_*
│   │   ├── idf_http_mock.cpp       ← mock implementations; compiled with real IDF source
│   │   └── mocks/
│   │       ├── esphome_compat.h    ← all ESPHome + FreeRTOS shims
│   │       ├── esp_http_client.h   ← IDF types + function declarations
│   │       ├── esp_timer.h         ← stub (function defined in esphome_compat.h)
│   │       ├── freertos/*.h        ← empty stubs (types in esphome_compat.h)
│   │       └── esphome/**/*.h      ← empty stubs (types in esphome_compat.h)
│   └── esphome/
│       ├── test_config.yaml        ← Tier 3: ESPHome compile check (no secrets, no flash)
│       ├── hardware_test.yaml      ← Tier 4: self-running hardware test suite (13 tests)
│       ├── secrets.yaml            ← gitignored; copy from secrets.yaml.example
│       └── secrets.yaml.example   ← template (wifi, api, ota)
│
└── example/
    ├── shelly_dimmer.yaml          ← real-world usage example
    └── ble_proxy_http_test.yaml    ← BLE proxy + HTTP integration example
```

---

## Development workflow

**Always use `make` targets** — do not call cmake/pytest/esphome directly. This
ensures consistent flags and output.

```bash
make init              # first-time setup: creates .venv, installs esphome + pytest
make upgrade           # pull latest ESPHome after a monthly release, then test
make check-deps        # verify all tools are present and the venv is ready
make test              # run C++ unit tests + Python schema tests
make test-native       # C++ tests only (no esphome/python/venv needed)
make test-python       # Python tests only — auto-runs `make init` if venv missing
make compile           # full esphome compile check (2-5 min, no hardware)
make flash             # compile + OTA flash hardware test suite + tail logs
make logs              # tail logs from the running device (no reflash)
make clean             # remove build artefacts (keeps .venv)
make clean-venv        # remove .venv (run before make init to force a fresh install)
make rebuild           # clean + test
```

### First-time setup

```bash
make init              # creates .venv, installs latest esphome + pytest
make test              # verify everything works
```

`make init` is idempotent — re-runs automatically whenever `requirements_test.txt`
changes. cmake and g++ are system packages (`apt install cmake g++` / `dnf install
cmake gcc-c++`) — they are not managed by the venv.

### Monthly ESPHome release update

ESPHome ships a new release every month (e.g. `2026.6.0`). This project intentionally
tracks the **latest release with no version pin**. Maintenance after a new release:

```bash
make upgrade                 # pip install --upgrade → latest esphome in .venv
make test                    # verify Python schema tests still pass
make compile                 # verify codegen + C++ still compiles
# fix any breakage in components/http_request_async/ if needed
```

The `upgrade` target is the only thing that needs to run. If `make test` and
`make compile` both pass, the component is compatible with the new release.

### When to run what

| Change | Run |
|---|---|
| Modified `__init__.py` schema or defaults | `make test-python` |
| Modified `http_request_async.h` or action template | `make test-native` |
| Modified `http_request_async_idf.cpp` | `make test-native && make compile` |
| Added a new YAML key end-to-end | `make test && make compile` |
| Before committing | `make test && make compile` |

---

## Four-tier testing strategy

**Tier 1 — Python pytest** (`make test-python`): Tests `__init__.py` schema
validation, defaults, and constraint enforcement. Runs in seconds, no hardware,
no cmake. Follows the same pattern as ESPHome's own unit tests.

**Tier 2 — Native C++ tests** (`make test-native`): Tests business logic in
isolation — callback ordering, alive-flag, queue management, error paths, and the
redirect-following loop. The real `http_request_async_idf.cpp` is compiled into
the test build; all `esp_http_client_*` calls are intercepted by
`idf_http_mock.cpp` whose return values are controlled per-test.
Runs in seconds, no hardware, no ESPHome install needed.

**Tier 3 — ESPHome compile** (`make compile`): Validates that the Python
codegen and generated C++ both compile. Catches type mismatches and missing
imports that can't be caught locally. Requires ESPHome installed.

**Tier 4 — Hardware** (`make flash`): Self-running test suite in
`tests/esphome/hardware_test.yaml`. Flash to an ESP32, watch the logs — all
13 tests start automatically ~1 min after boot. No HA, no dashboard required.
Run before tagging a release. Requires `python3 tools/test_server.py` running
on the dev machine and reachable from the ESP32's network segment.

---

## Git workflow

### Branch model

```
main  ──────────────────────────────────────────────────────▶  (always latest ESPHome, always green)
        │               │               │
   [esphome-       [esphome-       [esphome-
    2026.4]         2026.5]         2026.6]
                        │
               (bug found post-release)
                        │
               maint/esphome-2026.5 ──●──  [esphome-2026.5.1]
                                      │
                              cherry-pick → main
```

- **`main`** is the only permanent branch. It always reflects the latest ESPHome
  release and must always pass `make test && make compile`.
- **Tags** (`esphome-YYYY.MM`) mark verified, releasable commits. A tag is only
  created after both test tiers pass. Users pin to a tag in `external_components`.
- **`maint/` branches** are created **lazily** — only when an older tag needs a
  patch. Branch from the tag, fix, re-tag as `esphome-YYYY.MM.N`, cherry-pick
  back to `main` if the fix is still relevant.

### Monthly release workflow (normal case)

```bash
make upgrade                    # pip install --upgrade → latest esphome in .venv
make test                       # Tier 1 + Tier 2
make compile                    # Tier 3 (codegen + C++ compile)
# fix any breakage in components/http_request_async/ until both pass
git add -A
git commit -m "compat: ESPHome 2026.6"
git tag esphome-2026.6
git push && git push --tags     # push both branch and tag
```

The tag is never created on a broken state. `make test && make compile` is the
gate, not a formality.

### Patching an old release

```bash
git checkout -b maint/esphome-2026.5 esphome-2026.5
# make the fix
make test && make compile       # verify the fix against 2026.5 in the venv
                                # (downgrade if needed: pip install "esphome==2026.5.*")
git commit -m "fix: <description>"
git tag esphome-2026.5.1
git push origin maint/esphome-2026.5 --tags
# cherry-pick to main if the fix applies there too
git checkout main
git cherry-pick <commit-sha>
git push
```

Maintenance branches are deleted once the patched tag exists — they are
short-lived working branches, not permanent version lines.

### What NOT to do

- Do not create a branch per ESPHome version upfront — that's N diverging lines
  to maintain forever.
- Do not tag on `main` if either `make test` or `make compile` is failing.
- Do not push `main` in a broken state — agents and CI depend on `main` being green.

---

## TDD multi-agent workflow

When implementing a new feature, use this loop:

1. **Test Writer agent**: Write the failing test first.
   - For schema changes: add to `tests/python/test_init.py`
   - For C++ logic changes: add to `tests/native/main.cpp`
   - The test should describe the expected behavior, not the implementation.

2. **Code agent**: Implement until `make test` passes.
   - Only modify files in `components/http_request_async/`
   - Run `make test-native` after each change (fastest feedback)
   - Run `make test` before declaring done

3. **Validator**: Run `make test && make compile`.
   - Both must pass before the change is considered complete.

The Validator step is just two shell commands — it does not need a dedicated
agent. Use `! make test` in the Claude Code terminal.

---

## Coding conventions

Follow ESPHome's style exactly (the project is designed to merge cleanly into
ESPHome's codebase eventually):

- **Indentation**: 2 spaces, no tabs
- **Line length**: 120 characters max
- **Class members**: `lower_snake_case_with_trailing_underscore_`
- **Methods**: `lower_snake_case()`
- **Classes**: `UpperCamelCase`
- **Constants**: `UPPER_SNAKE_CASE`
- **No `#define` for constants** — use `const` or `enum class`
- **Always `this->`** for member access
- **`protected` not `private`** for fields that subclasses may need
- **No blocking** in `setup()` / `loop()` / `play_complex()` — no `delay()` calls
- **No heap allocation after `setup()`** except in the worker task's
  `execute_request_()` which creates the `HttpContainer` per request

---

## Files that must NOT be changed without tests

| File | Change requires |
|---|---|
| `http_request_async.h` — `enqueue_request()` | Test for the changed behavior |
| `http_request_async.h` — `play_complex()` alive-flag logic | `AliveGuardPreventsStaleCallbacks` test updated |
| `http_request_async_idf.cpp` — `execute_request_()` | An `Idf*` test in `main.cpp` |
| `__init__.py` — any `CONFIG_SCHEMA` key | A corresponding `TestHubSchema` test |
| `__init__.py` — any action schema key | A corresponding `TestGetActionSchema` etc. test |

---

## Known limitations (v0.1)

- HTTPS requires `verify_ssl: false` or a bundled CA. Per-request CA certs not yet supported.
- No request cancellation after enqueue (the request runs to completion or timeout).
- JSON body via `json:` dict form only supports string values (not nested objects).
  Use the `json: |-` lambda form for nested objects or non-string values.
- Queue depth is fixed at 8 pending requests. Overflow fires `on_error`.
