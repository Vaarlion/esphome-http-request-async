# Testing Guide

Four tiers of tests, each catching a different class of problem.

| Tier | What it tests | Speed | Hardware needed |
|------|---------------|-------|-----------------|
| 1 — Python | Schema validation, defaults, constraints | ~2 s | No |
| 2 — Native C++ | Callback ordering, queue logic, error routing | ~5 s | No |
| 3 — ESPHome compile | Codegen correctness, C++ type safety | 2–5 min | No |
| 4 — Hardware | Real HTTP, real WiFi, real timing | Minutes | Yes |

**Quick rule:** run `make test` (Tiers 1+2) after every change. Run `make compile`
before committing. Flash hardware (Tier 4) before tagging a release.

---

## Tier 1 — Python schema tests

Tests `__init__.py`: schema keys, defaults, type validation, mutual exclusions.
This mirrors the approach ESPHome uses for its own component tests.

```bash
make test-python
```

What's covered:
- Hub config keys — required, optional, defaults, int ranges
- Action schemas — URL validation, method enum, `body`/`json` exclusivity
- `capture_response` default and effect on trigger signature
- `ca_certificate_path` validation
- `task_count` range (1–8)

Tests live in `tests/python/test_init.py`. Each test class covers one schema
(`TestHubSchema`, `TestGetActionSchema`, `TestPostActionSchema`, etc.).

When to run: any change to `__init__.py`.

---

## Tier 2 — Native C++ tests

Tests the C++ component logic in isolation — no ESP32, no ESPHome build, no
real HTTP. Runs in a few seconds on the development machine.

```bash
make test-native
```

### How it works

`MockHttpRequestAsyncComponent` overrides `execute_request_()` to inject a
predefined response instead of making a real HTTP call. FreeRTOS queues are
replaced by `std::queue` wrappers (in `tests/native/mocks/esphome_compat.h`).

```cpp
MockHttpRequestAsyncComponent comp;
comp.set_next_response({200, R"({"output":"true"})", true, {}});

auto *req = make_request("http://192.168.1.10/api");
req->on_response_cb = [&](std::shared_ptr<HttpContainer> c) { /* ... */ };

comp.enqueue_request(req);
comp.tick();   // pump_worker() then pump_responses() — simulates one loop cycle
```

The mock derives `success` from `status_code` the same way the real implementation
does: 2xx/3xx → `success = true`, everything else → `success = false`.

### What's covered

- `on_response` fires for 2xx and 3xx responses
- `on_error` fires for transport failures (success=false), 4xx, 5xx
- `on_complete` always fires after either `on_response` or `on_error`
- Callback ordering: response/error → complete
- `alive_` guard: callbacks become no-ops after `stop()`
- `response->duration_ms` is populated
- `response->get_response_header()` is case-insensitive
- `max_response_buffer_size` is passed through to the request descriptor
- `collect_headers` stores header names correctly
- Multiple requests processed in order
- `on_error` fires immediately when network is not connected
- Concurrent enqueue and drain

When to run: any change to `http_request_async.h` or logic in `loop()` /
`enqueue_request()`.

---

## Tier 3 — ESPHome compile check

Validates that the Python codegen and generated C++ compile cleanly. Catches
type mismatches, missing includes, and invalid generated code that unit tests
can't detect.

```bash
make compile
```

The test config (`tests/esphome/test_config.yaml`) exercises:
- All three actions: `get`, `post`, `send`
- Both JSON body forms: lambda and dict
- Both trigger combinations: with and without `capture_response`
- Script parameters of multiple types (string, int, bool)
- `collect_headers` and `request_headers`

Takes 2–5 minutes. Run before every commit that touches `.h`, `.cpp`, or
`__init__.py`.

---

## Tier 4 — Hardware testing

Flash a real device and observe behaviour with a real HTTP server. Required before
tagging a release.

### Setup

**1. Start the test server** (on your development machine):

```bash
python3 tools/test_server.py        # listens on port 8765
```

Endpoints:
- `GET /slow?delay=N` — responds after N seconds
- `POST /echo` — echoes the request body after 1s
- `GET /fast` — immediate 200
- `GET /error` — always returns 500
- `GET /unreachable` — never responds (tests the timeout)

**2. Create `tests/esphome/secrets.yaml`** (gitignored):

```bash
cp tests/esphome/secrets.yaml.example tests/esphome/secrets.yaml
# fill in wifi_ssid, wifi_password, api_key, ota_password, test_server
```

`test_server` is the LAN IP and port of the machine running `test_server.py`,
e.g. `192.168.1.42:8765`.

**3. Flash and tail logs:**

```bash
make flash                          # compile + OTA flash + tail logs
make flash DEVICE=192.168.1.26     # specify device by IP
make logs                           # tail logs without flashing
make logs DEVICE=192.168.1.26
```

### What to verify

| Test | Expected result |
|------|----------------|
| `GET /slow?delay=3` | BLE scanning / sensor logs keep appearing during the 3s wait |
| `POST /echo` | Body echoed back in `on_response` |
| `GET /error` (HTTP 500) | `on_error` fires, NOT `on_response` |
| `GET /unreachable` (timeout) | `on_error` fires after `timeout` ms |
| Two parallel scripts with `task_count: 2` | Both resolve independently without blocking each other |

### Compile only (no flash)

```bash
make compile-device    # compile device_test.yaml without flashing
```

Useful to verify the local component compiles correctly before flashing.

---

## When to run what

| Change made | Run |
|-------------|-----|
| Modified `__init__.py` schema or defaults | `make test-python` |
| Modified `http_request_async.h` | `make test-native` |
| Modified `http_request_async_idf.cpp` | `make test-native && make compile` |
| Added a new YAML key end-to-end | `make test && make compile` |
| Before committing | `make test && make compile` |
| Before tagging a release | All of the above + hardware Tier 4 |
