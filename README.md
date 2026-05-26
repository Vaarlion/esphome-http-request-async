> [!WARNING]
> **This project was written entirely by AI (Claude), prompted by a non-developer.**
>
> It exists as a functional workaround for a personal project. It is not a proper ESPHome
> implementation and should not be treated as one. The component fills a genuine,
> long-standing gap in ESPHome:
>
> - **[feature-requests#799](https://github.com/esphome/feature-requests/issues/799)** —
>   "Non-blocking http requests" — open since July 2020, no milestone, no assignee.
> - **[PR#7892](https://github.com/esphome/esphome/pull/7892)** —
>   "[http_request] don't block loop with actions" — the only serious upstream attempt,
>   closed stale in February 2026.
>
> If you are a real ESPHome developer reading this: **please do this properly.** The problem
> is real, documented, and affects anyone using `http_request` with automations.
>
> The code here was written as carefully as an AI conversation allows — with tests,
> documented design decisions, and a sensible architecture. It runs in production on real
> hardware. But it was not written by someone who knows the ESPHome internals, and it has
> not been reviewed by anyone who does. Use it at your own risk, and treat it as a reference
> for what a proper implementation might look like — not as the implementation itself.

---

# ESPHome HTTP Request Async

Non-blocking HTTP requests for **ESP32 + ESP-IDF**. The ESPHome main loop never pauses.

---

## The problem

The built-in `http_request` component holds the ESPHome main loop for the full
duration of every request — typically 50–4500 ms per call. During that window,
every sensor poll, BLE scan, switch debounce, and display refresh is suspended.

```
http_request (built-in):
  Main loop:  ──── [████████████ HTTP 380ms ████████████] ──── sensors ──▶
                    everything else stops here ↑

http_request_async (this):
  Main loop:  ──── sensors / BLE / switches / display ──────────────────▶
  Worker:     ──── [HTTP 380ms] ──── idle ─────────────────────────────▶
  Automation:        │←── waiting ──→│ continue next step
```

Requests run on a dedicated FreeRTOS worker task. Inside an automation, the action
pauses the chain at the HTTP step and resumes it when the response arrives — so
sequential scripts (`http post` → `script.execute`) work exactly as written.

---

## Requirements

- ESP32 (any variant)
- `framework: type: esp-idf`

---

## Installation

**Pinned release (recommended):**

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/your-handle/esphome-http-request-async
      ref: esphome-2026.5
    components: [http_request_async]
```

Tags follow `esphome-YYYY.MM`. A tag is published only after `make test && make compile`
passes against that ESPHome version. If a bug is fixed after release, a patch tag
is published (e.g. `esphome-2026.5.1`).

**Latest `main` (tracks the current ESPHome release):**

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/your-handle/esphome-http-request-async
      ref: main
    components: [http_request_async]
```

### Version compatibility

| Tag | ESPHome |
|-----|---------|
| `esphome-2026.5` | 2026.5.x |

---

## Hub configuration

```yaml
http_request_async:
  id: http_client             # optional; auto-generated if omitted
  timeout: 4.5s               # per-request timeout
  verify_ssl: true            # validate TLS certificates
  follow_redirects: true      # follow HTTP 3xx responses
  redirect_limit: 3           # max redirects to follow
  buffer_size_rx: 512         # ESP-IDF receive buffer in bytes
  buffer_size_tx: 512         # ESP-IDF transmit buffer in bytes
  task_count: 1               # concurrent worker tasks (1–8)
  task_stack_size: 8192       # FreeRTOS stack per worker in bytes
  task_priority: 5            # FreeRTOS priority (1–24)
  useragent: "MyDevice/1.0"   # defaults to ESPHome/<version>
  ca_certificate_path: ca.pem # PEM for custom/self-signed TLS
```

| Key | Default | Description |
|-----|---------|-------------|
| `timeout` | `4.5s` | Fires `on_error` when exceeded. |
| `verify_ssl` | `true` | Set `false` for self-signed certs or plain HTTP. |
| `follow_redirects` | `true` | |
| `redirect_limit` | `3` | |
| `buffer_size_rx` | `512` | Increase if server returns large headers. |
| `buffer_size_tx` | `512` | Increase for large request bodies. |
| `task_count` | `1` | See [Concurrency](#concurrency). |
| `task_stack_size` | `8192` | Decrease only if RAM is very tight. |
| `task_priority` | `5` | Higher = preempts lower-priority tasks. |
| `ca_certificate_path` | — | If omitted and `verify_ssl: true`, uses the bundled Mozilla CA store. |

---

## Actions

### GET

```yaml
- http_request_async.get:
    url: !lambda 'return str_sprintf("http://%s/api/status", ip.c_str());'
    capture_response: true
    request_headers:
      Authorization: "Bearer mytoken"
    collect_headers:
      - content-type
    on_response:
      then:
        - lambda: |-
            ESP_LOGI("http", "status=%d  body=%s", response->status_code, body.c_str());
    on_error:
      then:
        - logger.log: "Request failed"
```

If the URL is a static string, the shorthand form works:

```yaml
- http_request_async.get: "http://192.168.1.10/api/ping"
```

### POST

**JSON dict** — for static or templatable key/value pairs:

```yaml
- http_request_async.post:
    url: "http://192.168.1.10/api/report"
    request_headers:
      Content-Type: application/json
    json:
      device: !lambda 'return App.get_name();'
      value: "42"
    on_error:
      then:
        - logger.log: "Post failed"
```

**JSON lambda** — for nested objects, non-string values, or dynamic keys:

```yaml
- http_request_async.post:
    url: !lambda 'return str_sprintf("http://%s/rpc", ip.c_str());'
    capture_response: true
    request_headers:
      Content-Type: application/json
    json: |-
      root["jsonrpc"] = "2.0";
      root["method"] = "Light.Toggle";
      root["params"]["id"] = light_id;
    on_response:
      then:
        - logger.log:
            format: "status=%d  %u ms  %s"
            args: [ response->status_code, response->duration_ms, body.c_str() ]
    on_error:
      then:
        - logger.log: "Post failed"
```

### send (any method)

```yaml
- http_request_async.send:
    method: PUT
    url: "http://192.168.1.10/api/state"
    body: '{"on": true}'
    on_response:
      then:
        - logger.log:
            format: "PUT → %d"
            args: [ response->status_code ]
```

Supported methods: `GET`, `POST`, `PUT`, `DELETE`, `PATCH`.

---

## `on_response` variables

| Variable | Type | Notes |
|----------|------|-------|
| `response` | `std::shared_ptr<HttpContainer>` | Always available |
| `response->status_code` | `int` | HTTP status (200, 404, …) |
| `response->duration_ms` | `uint32_t` | Total wall-clock time for the request |
| `response->content_length` | `size_t` | Bytes in the body. From `Content-Length` header when present; derived from the captured body size for chunked responses; 0 if `capture_response: false`. |
| `response->get_response_header("name")` | `std::string` | Case-insensitive; empty if not collected |
| `body` | `std::string &` | Response body; only when `capture_response: true` |

Script/automation parameters (e.g. `ip`, `light_id`) are available inside
`on_response` and `on_error` lambdas — they are captured from the enclosing
automation context.

---

## Error handling

`on_error` fires when the request cannot be completed successfully:

| Condition | Example |
|-----------|---------|
| Transport failure | DNS error, connection refused, TLS handshake failure |
| Timeout | Server did not respond within `timeout` |
| HTTP 4xx | 404 Not Found, 401 Unauthorized |
| HTTP 5xx | 500 Internal Server Error, 503 Unavailable |
| Not connected | WiFi not associated when the action fires |
| Queue full | More than 8 requests pending simultaneously |

`on_response` fires **only** for 2xx and 3xx responses.

The automation chain always continues after `on_response` or `on_error` — a
failed request does not leave the script stalled.

---

## Collecting response headers

By default, response headers are discarded. List the ones you want to keep in
`collect_headers`:

```yaml
- http_request_async.get:
    url: "http://api.example.com/data"
    capture_response: true
    collect_headers:
      - content-type
      - x-request-id
    on_response:
      then:
        - lambda: |-
            auto ct  = response->get_response_header("content-type");
            auto rid = response->get_response_header("x-request-id");
            ESP_LOGI("http", "ct=%s  rid=%s", ct.c_str(), rid.c_str());
```

Names in `collect_headers` are matched case-insensitively. `get_response_header()`
accepts any casing and returns an empty string for headers that were not collected.

---

## Response body buffering

The response body is buffered in RAM before the `on_response` callback fires.
The default limit is 1 kB. Increase it with `max_response_buffer_size`:

```yaml
- http_request_async.get:
    url: "http://192.168.1.10/api/large"
    capture_response: true
    max_response_buffer_size: 8192   # 8 kB
```

Bodies larger than the limit are silently truncated (a warning is logged). If you
only need the status code or headers, set `capture_response: false` to skip
buffering entirely.

---

## Concurrency

With `task_count: 1` (the default), requests run one at a time. A slow request
— a 10-second timeout, for example — blocks all queued requests for its full
duration.

Increase `task_count` to process multiple requests simultaneously:

```yaml
http_request_async:
  task_count: 2
```

Each worker allocates `task_stack_size` bytes of DRAM at boot, regardless of
whether it is idle:

| `task_count` | DRAM (8 kB stack) | Concurrent | Typical use |
|:---:|---:|:---:|---|
| 1 | 8 kB | 1 | Single periodic poll |
| 2 | 16 kB | 2 | Slow + fast endpoints in parallel |
| 3 | 24 kB | 3 | Mixed workloads (REST + telemetry + OTA check) |
| 4 | 32 kB | 4 | Multiple independent APIs |

The ESP32 has 320 kB of DRAM shared with the kernel, Wi-Fi stack, BLE, and
application state. Available heap at boot is typically 180–250 kB depending on
what else is enabled.

**Rule of thumb:** `task_count: 2` is sufficient for most setups. Use 3–4 only
if you regularly fire requests from independent automations that overlap. The
request queue holds 8 entries — overflow fires `on_error` immediately.

---

## Migration from `http_request`

Rename the hub and action prefixes:

| Before | After |
|--------|-------|
| `http_request:` | `http_request_async:` |
| `http_request.get:` | `http_request_async.get:` |
| `http_request.post:` | `http_request_async.post:` |
| `http_request.send:` | `http_request_async.send:` |

All configuration keys carry over unchanged.

**One behaviour difference:** the built-in component routes all completed
requests — including 4xx/5xx — through `on_response`. This component routes
4xx/5xx through `on_error` instead. Adjust any `on_response` handlers that
currently check `response->status_code >= 400`.

---

## Further reading

- [Development guide](docs/development.md) — first-time setup, making changes, git workflow
- [Testing guide](docs/testing.md) — four-tier test strategy, running tests, hardware testing
- [Shelly dimmer example](example/shelly_dimmer.yaml) — real-world Shelly API scripts
