# Testing Guide

Four tiers of tests. Tiers 1–3 run locally in seconds to minutes with no
hardware. Tier 4 requires a physical ESP32 and is the only way to cover the
real IDF execution path.

| Tier | What it tests | Speed | Hardware |
|------|---------------|-------|----------|
| 1 — Python | Schema validation, defaults, constraints | ~2 s | No |
| 2 — Native C++ | Callback routing, queue logic, action lifecycle | ~5 s | No |
| 3 — ESPHome compile | Codegen correctness, C++ type safety | 2–5 min | No |
| 4 — Hardware | Real HTTP, real TLS, real chunked encoding, real WiFi | Minutes | Yes |

**Quick rule:** run `make test` (Tiers 1+2) after every change. Run `make compile`
before committing. Flash Tier 4 before tagging a release.

---

## What can and cannot be tested locally

The component has a hard split between testable and non-testable logic:

**Testable locally (Tiers 1–3)**
- Python schema validation, defaults, mutual exclusions, codegen options
- Callback dispatch: which callback fires for which status code
- Callback ordering: response/error → complete
- Queue overflow: on_error fires immediately, automation does not stall
- Alive-flag guard: callbacks suppressed after stop()
- `is_running()` lifecycle: true while in flight, false after complete/stop
- `is_running()` chain propagation: true while a chained action executes

**Cannot be tested without hardware (Tier 4 only)**
- The entire `execute_request_()` IDF path:
  - HTTP method → IDF enum mapping
  - Request headers reaching the server
  - Response body read loop (Content-Length and chunked encoding)
  - `content_length` updated from body for chunked responses
  - Truncation: body capped at `max_response_buffer_size`, warning logged
  - `collect_headers` filter: un-requested headers discarded at the event handler
  - TLS handshake, CA bundle validation, `verify_ssl: false` behaviour
  - Redirect following
  - Real timing: `duration_ms` reflects actual wall-clock HTTP time

---

## Tier 1 — Python schema tests

```bash
make test-python
```

Tests `__init__.py`: schema keys, defaults, type validation, mutual exclusions,
and the sdkconfig codegen for TLS options. Mirrors the approach ESPHome uses
for its own component unit tests.

What's covered:
- Hub config keys — required, optional, defaults, int ranges
- Action schemas — URL validation, method enum, `body`/`json` exclusivity
- `capture_response` default and trigger signature
- `task_count`, `task_stack_size`, `task_priority` range enforcement
- `buffer_size_rx/tx` uint16_t overflow rejection
- `verify_ssl: true` → `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE` enabled, insecure flags off
- `verify_ssl: false` → insecure flags on, CA bundle NOT attached

When to run: any change to `__init__.py`.

---

## Tier 2 — Native C++ tests

```bash
make test-native
```

Tests C++ component logic in isolation — no ESP32, no ESPHome build, no real
HTTP. Runs in a few seconds on the development machine.

### How it works

`MockHttpRequestAsyncComponent` overrides `execute_request_()` to inject a
predefined response. FreeRTOS queues are replaced by `std::queue` wrappers in
`tests/native/mocks/esphome_compat.h`.

```cpp
MockHttpRequestAsyncComponent comp;
comp.set_next_response({200, R"({"output":"true"})"});

auto *req = make_request("http://192.168.1.10/api");
req->on_response_cb = [&](std::shared_ptr<HttpContainer> c) { /* ... */ };

comp.enqueue_request(req);
comp.tick();   // pump_worker() then pump_responses() — simulates one loop cycle
```

### What's covered

| Test | What it verifies |
|------|-----------------|
| `OnResponseCalledOnSuccess` | on_response fires for 2xx |
| `OnErrorCalledOnTransportFailure` | on_error fires when status=0 |
| `OnErrorCalledOnHttpClientError` | on_error fires for 4xx |
| `OnErrorCalledOnServerError` | on_error fires for 5xx |
| `OnResponseCalledOnRedirect` | on_response fires for 3xx |
| `OnCompleteAlwaysFires` | on_complete fires after success |
| `OnCompleteAlwaysFiresOnError` | on_complete fires after error |
| `NetworkDownCallsOnError` | on_error fires immediately when WiFi is down |
| `BodyTruncatedAtMaxBufferSize` | captured body capped at max_response_buffer_size |
| `NoCaptureResponseLeavesBodyEmpty` | body stays empty when capture_response=false |
| `AliveGuardPreventsStaleCallbacks` | callbacks suppressed after stop() clears alive_ |
| `DurationIsReported` | duration_ms passed through to callback |
| `CollectedResponseHeaderAccessibleInCallback` | get_response_header() round-trip |
| `QueueFullDropsRequestWithOnError` | 9th enqueue fires on_error immediately |
| `ConcurrentEnqueueAndDrain` | N requests enqueued and drained in one pass |
| `NullCallbacksDoNotCrashLoop` | no crash when on_response/on_error are null |
| `NullCallbacksDoNotCrashOnNetworkDown` | no crash when callbacks null + network down |
| `ContentLengthMatchesResponseBodySize` | content_length == body.size() when captured |
| `IsRunningFollowsRequestLifecycle` | is_running() true during flight, false after complete |
| `IsRunningPropagatesIntoChainedActions` | is_running() true while chained action runs (mode:single) |
| `StopClearsIsRunningAndSuppressesCallbacks` | stop_complex() clears is_running(), blocks callbacks |

When to run: any change to `http_request_async.h` or logic in `loop()` / `enqueue_request()`.

---

## Tier 3 — ESPHome compile check

```bash
make compile
```

Validates that the Python codegen and generated C++ compile cleanly. Catches
type mismatches, missing includes, and codegen bugs that unit tests cannot detect.

The test config (`tests/esphome/test_config.yaml`) exercises all three actions,
both JSON body forms, both trigger combinations, script parameters of multiple
types, `collect_headers`, and `request_headers`. Takes 2–5 minutes.

Run before every commit that touches `.h`, `.cpp`, or `__init__.py`.

---

## Tier 4 — Hardware testing

Flash a real ESP32 and trigger scripts to verify the IDF execution path with
real HTTP calls. Required before tagging a release.

### Test config

`tests/esphome/hardware_test.yaml` — purpose-built for runtime edge-case testing.
It has two hub instances (`http_client` with `verify_ssl: false` for HTTP/insecure
HTTPS, `https_client` with `verify_ssl: true` for CA-validated HTTPS) and one
script per scenario.

### Setup

**1. Run httpbin locally** (most tests use it):

```bash
docker run -p 8000:80 kennethreitz/httpbin
```

**2. Set `httpbin_host`** in the YAML to your machine's LAN IP:

```yaml
globals:
  - id: httpbin_host
    initial_value: '"192.168.1.42:8000"'
```

**3. Create `tests/esphome/secrets.yaml`** (gitignored):

```yaml
wifi_ssid: "YourSSID"
wifi_password: "YourPassword"
```

**4. Flash and tail logs:**

```bash
esphome run tests/esphome/hardware_test.yaml
```

### Hardware test scenarios

Trigger each script manually (HA service call or ESPHome dashboard).
Look for `[PASS]` / `[FAIL]` in the logs.

| Script | What it verifies | Log prefix |
|--------|-----------------|------------|
| `test_truncation` | Body capped at 64 B; LOGW emitted; `content_length` still reflects full response | `[TRUNCATION]` |
| `test_chunked_content_length` | Chunked response: `content_length == body.size()` (not 0) | `[CHUNKED]` |
| `test_collect_headers_filter` | Requested header present; un-requested header absent | `[HEADERS FILTER]` |
| `test_request_header_injection` | Custom request header echoed by server | `[HDR INJECT]` |
| `test_delete_method` | DELETE returns 200 | `[DELETE]` |
| `test_patch_method` | PATCH returns 200; body echoed | `[PATCH]` |
| `test_redirect` | 3 redirects followed; final status 200 | `[REDIRECT]` |
| `test_tls_verify_on` | HTTPS with CA bundle validation (needs internet) | `[TLS VERIFY ON]` |
| `test_tls_verify_off` | HTTPS without CA validation | `[TLS VERIFY OFF]` |
| `test_on_error_4xx` | 404 routes to on_error, not on_response | `[4XX ON_ERROR]` |
| `test_concurrent_requests` | Two requests in flight simultaneously; both complete | `[CONCURRENT]` |

The `test_truncation` test also validates the "break early" fix — if the worker
is blocked reading the full 4096-byte response for the 10 s timeout, something
regressed. The response should complete well under 1 s.

---

## When to run what

| Change made | Run |
|-------------|-----|
| Modified `__init__.py` schema or defaults | `make test-python` |
| Modified `http_request_async.h` | `make test-native` |
| Modified `http_request_async_idf.cpp` | `make test-native && make compile` |
| Added a new YAML key end-to-end | `make test && make compile` |
| Before committing | `make test && make compile` |
| Before tagging a release | All of the above + Tier 4 hardware |
