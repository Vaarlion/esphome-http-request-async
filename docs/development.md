# Development Guide

How to set up the project, make changes, and publish a release.

---

## Prerequisites

| Tool | Min version | Install |
|------|-------------|---------|
| `cmake` | 3.16 | `apt install cmake` / `dnf install cmake` |
| `g++` | C++17 capable | `apt install g++` / `dnf install gcc-c++` |
| `python3` | 3.9+ | system package |

Python dependencies (ESPHome, pytest) are managed by `make init` inside a local
virtual environment. Do not install them globally.

---

## First-time setup

```bash
git clone https://github.com/your-handle/esphome-http-request-async
cd esphome-http-request-async

make init       # creates .venv, installs esphome + pytest
make test       # should be all green
```

`make init` is idempotent — rerun it any time `requirements_test.txt` changes.

---

## Project layout

```
components/http_request_async/
  __init__.py               ESPHome YAML schema + Python codegen
  http_request_async.h      C++ types, component class, action template
  http_request_async_idf.cpp  IDF worker task + esp_http_client calls

tests/
  python/test_init.py       Pytest: schema validation, defaults, constraints
  native/main.cpp           GoogleTest: C++ business logic (no hardware needed)
  esphome/test_config.yaml  ESPHome compile target (Tier 3)
  esphome/device_test.yaml  Hardware flash config (Tier 4)

docs/
  development.md            ← you are here
  testing.md                Testing strategy

example/
  shelly_dimmer.yaml        Real-world Shelly API example
  ble_proxy_http_test.yaml  Integration test example
```

---

## Making changes

### New YAML configuration key

1. Add the key to `CONFIG_SCHEMA` in `__init__.py` with validation and a default.
2. Add a `set_<key>()` setter in `http_request_async.h`.
3. Call the setter in `to_code()` in `__init__.py`.
4. Use the stored value in `http_request_async_idf.cpp`.
5. Add a test to `tests/python/test_init.py` covering the default, a valid value,
   and any invalid values.

```bash
make test-python    # fast feedback (~2s)
make test           # full suite
make compile        # verify codegen compiles
```

### New action parameter

Same as above, but also:
- Add the parameter to `HttpRequestAsyncSendAction` in `http_request_async.h`.
- Add it to `_ACTION_BASE_SCHEMA` (or the relevant method schema) in `__init__.py`.
- Wire it up in `_build_action()` in `__init__.py`.
- Add a C++ test in `tests/native/main.cpp` if the parameter affects runtime behaviour.

### C++ business logic change

```bash
make test-native    # fastest: builds and runs C++ tests only (~5s)
make test           # full suite before commit
```

### Files that require tests before merging

| File | Change requires |
|------|----------------|
| `__init__.py` — any `CONFIG_SCHEMA` key | A `TestHubSchema` test |
| `__init__.py` — any action schema key | A `TestGetActionSchema` / `TestPostActionSchema` test |
| `http_request_async.h` — `enqueue_request()` | A test for the changed behaviour |
| `http_request_async.h` — `play_complex()` alive-flag | `AliveGuardPreventsStaleCallbacks` updated |

---

## Make targets

```
make init              create .venv + install Python deps (run once)
make test              C++ unit tests + Python schema tests
make test-native       C++ unit tests only (no ESPHome needed)
make test-python       Python schema tests only
make compile           full ESPHome codegen + C++ compile (~3 min)
make rebuild           clean then test
make upgrade           pip upgrade to latest ESPHome (run after monthly release)
make check-deps        verify cmake, g++, python3, .venv are all present
make clean             remove build artefacts (keeps .venv)
make clean-venv        remove .venv (force fresh install on next make init)
```

---

## Monthly ESPHome release

ESPHome ships a new release every month. Maintenance steps:

```bash
make upgrade           # pip install --upgrade → latest esphome in .venv
make test              # verify schema tests still pass
make compile           # verify codegen + C++ still compiles
# fix any breakage in components/http_request_async/ if needed
git add -A
git commit -m "compat: ESPHome 2026.6"
git tag esphome-2026.6
# push branch + tag only after user approval
```

If `make test && make compile` pass without changes, the component is already
compatible with the new release. The commit and tag are still required — they
mark the verified state.

---

## Git workflow

### Branch model

```
main  ─────────────────────────────────────────────────────▶  (always green)
        │              │              │
   esphome-       esphome-       esphome-
    2026.4         2026.5         2026.6
                       │
              (bug found post-release)
                       │
              maint/esphome-2026.5 ──●── esphome-2026.5.1
                                     │
                             cherry-pick → main
```

- **`main`** is the only permanent branch. It must always pass `make test && make compile`.
- **Tags** (`esphome-YYYY.MM`) are created after tests pass. Users pin to a tag.
- **`maint/` branches** are created only when an older tag needs a patch. Branch
  from the tag, fix, re-tag as `esphome-YYYY.MM.N`, cherry-pick back to `main`
  if the fix applies there too. Delete the `maint/` branch once the tag is pushed.

### What not to do

- Don't tag a commit that fails `make test` or `make compile`.
- Don't push `main` in a broken state.
- Don't create permanent version branches — that's what tags are for.
