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

**Testable via IDF mock (Tier 2, `IdfMockHttpRequestAsyncComponent`)**
- Redirect-following loop: hop count, limit enforcement, 302/307 method handling,
  missing-Location-header behaviour, intermediate header clearing

**Cannot be tested without hardware (Tier 4 only)**
- The real network path in `execute_request_()`:
  - HTTP method → IDF enum mapping reaching an actual server
  - Request headers transmitted over the wire
  - Response body read loop (Content-Length and chunked encoding from a real server)
  - `content_length` updated from body for chunked responses
  - Truncation: body capped at `max_response_buffer_size`, warning logged
  - `collect_headers` filter: un-requested headers discarded at the event handler
  - TLS handshake, CA bundle validation, `verify_ssl: false` behaviour
  - Real redirect chains (real Location headers, real TCP connections per hop)
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

Two test component classes are available:

**`MockHttpRequestAsyncComponent`** — overrides `execute_request_()` to inject a
predefined response directly. FreeRTOS queues are replaced by `std::queue`
wrappers. Use this for testing callback routing, lifecycle, and queue behaviour.

```cpp
MockHttpRequestAsyncComponent comp;
comp.set_next_response({200, R"({"output":"true"})"});

auto *req = make_request("http://192.168.1.10/api");
req->on_response_cb = [&](std::shared_ptr<HttpContainer> c) { /* ... */ };

comp.enqueue_request(req);
comp.tick();   // pump_worker() then pump_responses() — simulates one loop cycle
```

**`IdfMockHttpRequestAsyncComponent`** — does NOT override `execute_request_()`.
The real IDF implementation runs; all `esp_http_client_*` calls are intercepted by
`idf_http_mock.cpp`. Control the response sequence and inspect call counts via
`g_idf_mock`. Use this for testing IDF-level logic (redirect loop, method
switching, header clearing).

```cpp
g_idf_mock.reset();
g_idf_mock.push_statuses({302, 302, 200});  // simulate two redirect hops

IdfMockHttpRequestAsyncComponent comp;
comp.enqueue_request(req);
comp.tick();

EXPECT_EQ(g_idf_mock.set_redirection_call_count, 2);
```

### What's covered

**Callback routing / lifecycle (MockHttpRequestAsyncComponent)**

| Test | What it verifies |
|------|-----------------|
| `OnResponseCalledOnSuccess` | on_response fires for 2xx |
| `OnErrorCalledOnTransportFailure` | on_error fires when status=0 |
| `OnErrorCalledOnHttpClientError` | on_error fires for 4xx |
| `OnErrorCalledOnHttpServerError` | on_error fires for 5xx |
| `OnResponseCalledOn3xxRedirect` | on_response fires for 3xx (unhandled redirect) |
| `OnCompleteFiresAfterOnResponse` | on_complete fires after on_response |
| `OnCompleteFiresAfterOnError` | on_complete fires after on_error |
| `OnCompleteFiresAfterHttpError` | on_complete fires after HTTP 4xx/5xx |
| `OnErrorWhenNotConnected` | on_error fires immediately when WiFi is down |
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

**IDF redirect loop (IdfMockHttpRequestAsyncComponent)**

| Test | What it verifies |
|------|-----------------|
| `IdfRedirectChainFollowed` | 302×3 → 200: on_response fires with final status; correct hop count |
| `IdfRedirectLimitEnforced` | limit=3 with 5×302: stops at 4th hop, surfaces 302 via on_response |
| `IdfRedirectNotFollowedWhenDisabled` | follow_redirects=false: 302 returned as-is, set_redirection not called |
| `IdfRedirectStopsOnMissingLocation` | set_redirection() returns ESP_FAIL: response treated as final |
| `IdfRedirect302SwitchesToGetDropsBody` | POST + 302 → GET: second open() has write_len=0; set_method not called |
| `IdfRedirect307PreservesMethod` | POST + 307: set_method() called to restore POST; body resent |
| `IdfRedirectClearsIntermediateHeaders` | headers from redirect hops cleared; final-hop headers accessible |

When to run: any change to `http_request_async.h`, `http_request_async_idf.cpp`,
or logic in `loop()` / `enqueue_request()`.

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

Flash a real ESP32 and watch it run the full test suite automatically.
No Home Assistant, no dashboard interaction required.

### Setup

**1. Start the test server** on your dev machine:

```bash
python3 tools/test_server.py          # listens on port 8765
```

**2. Create `tests/esphome/secrets.yaml`** (gitignored):

```bash
cp tests/esphome/secrets.yaml.example tests/esphome/secrets.yaml
# fill in: wifi_ssid, wifi_password, api_key, ota_password
# set test_server to your dev machine's LAN IP:8765
```

**3. Flash and watch:**

```bash
make flash                  # compile + OTA flash + tail logs
# or, if the device is already running:
make logs                   # just tail the logs
```

Tests start automatically ~1 minute after boot (gives WiFi and the API time to
stabilise), then repeat every 10 minutes.

**Note:** the ESP32 must be able to reach the test server host on port 8765.
If it is on an isolated IoT VLAN, open TCP 8765 from that subnet both at the
router/firewall and on the host OS.

### What runs

All 12 tests execute sequentially. Look for `[PASS]` / `[FAIL]` per test,
and `=== RESULTS: N passed, M failed ===` at the end.

| Test | Endpoint | What it verifies |
|------|----------|-----------------|
| 1/12 | `GET /slow?delay=2` | GET completes; `duration_ms ≥ 2000` |
| 2/12 | `POST /echo` | POST body echoed back by server |
| 3/12 | `GET /bytes?size=4096` (max=64 B) | Body capped at 64; `content_length` = 4096 |
| 4/12 | `GET /stream?lines=5` | Chunked: `content_length == body.size()` (not 0) |
| 5/12 | `GET /fast` | `x-server` header collected; un-requested header absent |
| 6/12 | `GET /headers` | Custom request header echoed by server |
| 7/12 | `DELETE /delete` | DELETE returns 200 |
| 8/12 | `PATCH /patch` | PATCH body echoed; status 200 |
| 9/12 | `GET /redirect?n=3` | 3 redirects followed; final status 200 |
| 10/12 | `GET /fast` × 2 | Sequential requests: second fires only after first `on_complete` |
| 11/12 | `GET /status?code=404` | 404 routes to `on_error`, not `on_response` |
| 12/12 | `GET /status?code=500` | 500 routes to `on_error`, not `on_response` |

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
