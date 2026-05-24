# Testing Strategy

ESPHome itself has no C++ unit tests. Its own test suite is pure Python (pytest)
and tests only the config-generation layer. Firmware correctness is validated by
`esphome compile` (checks that generated C++ compiles) and hardware testing.

We add one extra tier on top of that: a native C++ test for the business logic
that doesn't depend on hardware or the full ESPHome build.

---

## Tier 1 — Python tests for `__init__.py` (no hardware, seconds)

Tests the schema validation, defaults, and constraints of the Python codegen layer.
This matches what ESPHome's own unit tests do.

```bash
pip install esphome pytest
pytest tests/python/ -v
```

Covers:
- Hub config: required keys, optional keys, defaults, range validation
- Action schemas: URL validation, method enum, body/json exclusivity
- `on_response` / `on_error` accepted or rejected correctly

---

## Tier 2 — Native C++ tests for component business logic (no hardware, seconds)

Tests the C++ business logic in isolation using:
- A `MockHttpRequestAsyncComponent` that overrides `execute_request_()` with a
  predefined response (no real HTTP calls)
- FreeRTOS shims that replace `xQueueCreate / xQueueSend / xQueueReceive` with
  `std::queue` wrappers
- All `esp_http_client.h` / `esp_timer.h` etc. headers shadowed by empty stubs

These tests cover things ESPHome doesn't test: callback ordering, alive-flag
cancellation, queue-full handling, and the async play_complex/resume mechanism.

```bash
cd tests/native
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
./build/test_runner
```

Requirements: CMake ≥ 3.16, g++ or clang++ with C++17, internet access for the
first build (GoogleTest is fetched automatically by CMake FetchContent).

---

## Tier 3 — ESPHome compilation check (no hardware, 2-5 minutes)

Validates that the Python codegen and generated C++ both compile without errors.
This is the standard ESPHome testing approach.

```bash
esphome compile tests/esphome/test_config.yaml
```

The test config exercises all three actions (get/post/send), both JSON forms
(lambda and dict), all trigger combinations, and all parameter types that appear
in real Shelly scripts (string, int, bool parameters).

---

## Tier 4 — Hardware-in-the-loop (real ESP32 required, minutes)

```bash
# Start a local HTTP echo server
pip install flask httpbin
python -m flask --app httpbin:app run --port 8080

# Flash and watch
esphome run tests/esphome/test_config.yaml
# Edit test_config.yaml to call the scripts on_boot with a local IP
```

---

## Multi-agent TDD workflow (optional, with Claude Code)

This maps naturally to three parallel agents in Claude Code:

```
┌──────────────────────┐   "Write test for     ┌──────────────────────┐
│  Test Writer agent   │ ─── feature X" ──────▶│                      │
│                      │                        │  Code agent          │
│  1. Adds failing     │ ◀─ "implemented" ───── │  Implements until    │
│     test to          │                        │  test passes         │
│     tests/python/    │                        │                      │
│     or tests/native/ │                        └──────────────────────┘
└──────────────────────┘
         │
         │ "run tests"
         ▼
┌──────────────────────┐
│  Validator agent     │
│  Runs pytest +       │
│  cmake test_runner   │
│  Reports pass/fail   │
└──────────────────────┘
```

Practical loop for a new feature (e.g. "add timeout per-request override"):

1. **Test Writer** adds a Python test asserting the new schema key validates correctly,
   and a C++ test asserting the timeout value reaches the PendingRequest.
2. **Code agent** modifies `__init__.py` and `http_request_async.h` until both tests pass.
3. **Validator** runs `pytest tests/python/` and `./build/test_runner`.
4. On green: Code agent runs `esphome compile tests/esphome/test_config.yaml` (Tier 3).

You can invoke the agents with `! pytest tests/python/` and
`! cd tests/native && cmake --build build && ./build/test_runner` inside Claude Code
to run them in the current terminal session.
