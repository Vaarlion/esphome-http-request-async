# ESPHome HTTP Request Async

A fully asynchronous HTTP request component for ESPHome on **ESP32 (ESP-IDF framework only)**.

## The problem with `http_request`

The built-in `http_request` component blocks the **entire** ESPHome main loop for the duration of each request. With typical LAN latency this means 50–200 ms of freeze per call. During that window, every sensor poll, switch debounce, BLE scan, and display refresh is suspended.

## What this component does differently

Requests run on a **dedicated FreeRTOS worker task**. The main loop is never blocked. Inside an automation, the action pauses the automation at the HTTP step and resumes it transparently when the response arrives — so sequential scripts work exactly as expected, without polling loops or manual state machines.

```
Main loop:    ─────────────── sensors / switches / BLE ──────────────────────────▶
Worker task:  ─ idle ─ [HTTP GET 180ms] ─ idle ─ [HTTP POST 95ms] ─ idle ─────────▶
Automation:              │←── waiting ──→│ continue next step
```

## Requirements

- ESP32 (any variant)
- `framework: type: esp-idf`

```yaml
esp32:
  board: esp32dev
  framework:
    type: esp-idf
```

## Installation

**Recommended — pin to a verified release tag:**

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/your-handle/esphome-http-request-async
      ref: esphome-2026.5    # ← matches your ESPHome version
    components: [http_request_async]
```

Each tag is only published after `make test && make compile` passes against that
ESPHome version. If you are on a different ESPHome version, find the matching tag in
the [releases](https://github.com/your-handle/esphome-http-request-async/releases).

**Edge (latest `main`):**

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/your-handle/esphome-http-request-async
      ref: main
    components: [http_request_async]
```

Use this only if your ESPHome install tracks the latest release and you are
comfortable with untagged code.

## ESPHome version compatibility

| Component tag | ESPHome version | Notes |
|---|---|---|
| `esphome-2026.5` | 2026.5.x | Initial release |

Tags follow the pattern `esphome-YYYY.MM`. A patch release (e.g. `esphome-2026.5.1`)
is published if a bug fix is backported to an already-released tag.

## Hub configuration

```yaml
http_request_async:
  id: http_client               # optional; auto-generated if omitted
  timeout: 4.5s                 # per-request timeout (default: 4.5s)
  verify_ssl: true              # validate TLS certificates (default: true)
  follow_redirects: true        # follow HTTP 3xx (default: true)
  redirect_limit: 3             # max redirects (default: 3)
  buffer_size_rx: 512           # IDF HTTP receive buffer in bytes (default: 512)
  buffer_size_tx: 512           # IDF HTTP transmit buffer in bytes (default: 512)
  task_stack_size: 8192         # worker task stack in bytes (default: 8192)
  task_priority: 5              # FreeRTOS priority 1-24 (default: 5)
  task_count: 1                 # concurrent worker tasks 1-8 (default: 1)
  useragent: "MyDevice/1.0"     # optional; defaults to ESPHome/<version>
  ca_certificate_path: ca.pem   # optional PEM for custom/self-signed TLS
```

## Concurrency and RAM cost

Each worker task processes one request at a time. With `task_count: 1` (the default),
requests are serialised — a 10-second timeout on one request blocks all others for 10 s.
With `task_count: N`, up to N requests run simultaneously.

**RAM cost:** each worker allocates exactly `task_stack_size` bytes of DRAM at boot,
regardless of whether it is idle or busy.

| `task_count` | DRAM (default 8 kB stack) | Concurrent requests | Typical use case |
|:---:|---:|:---:|---|
| 1 (default) | 8 kB | 1 | Single periodic poll |
| 2 | 16 kB | 2 | One slow + one fast request in parallel |
| 3 | 24 kB | 3 | Mixed workloads (REST + telemetry + OTA check) |
| 4 | 32 kB | 4 | Heavily loaded device, multiple independent APIs |

The ESP32 has 320 kB of DRAM, shared with the FreeRTOS kernel, network stack,
BLE, ESPHome components, and application globals. Typical available heap at boot
is 180–250 kB depending on what else is enabled.

**Rule of thumb for a heavily-loaded ESP32 with BLE proxy:**
`task_count: 2` (16 kB overhead) is sufficient for most workloads. Use `task_count: 3`
or `4` only if you regularly fire requests from multiple independent automations that
might overlap. Going beyond 4 is rarely beneficial — HTTP requests are typically
network-bound, not CPU-bound, and a 10 s timeout on N tasks still only costs 10 s.

The pending-request queue holds 8 entries regardless of `task_count`. Requests
beyond that are dropped immediately with `on_error`.

## Actions

### `http_request_async.get`

```yaml
- http_request_async.get:
    url: !lambda 'return "http://" + ip + "/rpc/Light.GetStatus?id=0";'
    capture_response: true
    request_headers:
      Content-Type: application/json
    collect_headers:
      - content-type
    on_response:
      then:
        - lambda: |-
            ESP_LOGI("http", "status=%d body=%s", response->status_code, body.c_str());
    on_error:
      then:
        - logger.log: "Request failed"
```

### `http_request_async.post`

```yaml
- http_request_async.post:
    url: !lambda 'return str_sprintf("http://%s/rpc", ip.c_str());'
    capture_response: true
    request_headers:
      Content-Type: application/json
    json: |-
      root["jsonrpc"] = "2.0";
      root["method"] = "Light.Toggle";
      root["params"]["id"] = to_string(light_id);
    on_response:
      then:
        - logger.log:
            format: "status=%d duration=%ums body=%s"
            args:
              - response->status_code
              - response->duration_ms
              - body.c_str()
    on_error:
      then:
        - logger.log: "Request failed"
```

### `http_request_async.send`

```yaml
- http_request_async.send:
    method: PUT
    url: "http://192.168.1.100/api/endpoint"
    body: '{"key":"value"}'
    on_response:
      then: ...
```

## `on_response` variables

| Variable | Type | Notes |
|---|---|---|
| `response` | `std::shared_ptr<HttpContainer>` | Always available |
| `response->status_code` | `int` | HTTP status code |
| `response->duration_ms` | `uint32_t` | Total wall-clock time for the request |
| `response->content_length` | `size_t` | From Content-Length header; 0 if absent/chunked |
| `response->get_response_header("name")` | `std::string` | Case-insensitive header lookup |
| `body` | `std::string &` | Response body; only when `capture_response: true` |

Script/automation parameters (e.g. `ip`, `light_id`) are available inside `on_response` and `on_error` lambdas — they are captured from the enclosing automation context.

## Migration from `http_request`

The YAML interface is intentionally compatible. Rename the component and actions:

| Before | After |
|---|---|
| `http_request:` | `http_request_async:` |
| `http_request.get:` | `http_request_async.get:` |
| `http_request.post:` | `http_request_async.post:` |
| `http_request.send:` | `http_request_async.send:` |

All config keys carry over unchanged. Add `task_stack_size` / `task_priority` only if the defaults cause issues.

## Testing

See [`tests/README.md`](tests/README.md) for the three-tier testing strategy and how to run native unit tests without hardware.
