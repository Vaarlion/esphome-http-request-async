#include "http_request_async.h"

#ifdef USE_ESP32

#include "esp_http_client.h"
#include "esp_timer.h"

#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif

#include "esphome/components/network/util.h"
#include "esphome/core/application.h"
#include "esphome/core/log.h"

namespace esphome {
namespace http_request_async {

// ── Event handler context ─────────────────────────────────────────────────────

/// Passed as user_data to esp_http_client_config_t.event_handler.
/// Lives on the stack of execute_request_(); valid for the duration of
/// esp_http_client_perform().
struct EventHandlerCtx {
  const PendingRequest *req;
  HttpContainer *container;
};

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
  auto *ctx = static_cast<EventHandlerCtx *>(evt->user_data);

  switch (evt->event_id) {
    case HTTP_EVENT_ON_HEADER: {
      // Collect only the response headers the action asked for.
      const std::string lower_key = str_lower_case(evt->header_key);
      for (const auto &name : ctx->req->lower_case_collect_headers) {
        if (name == lower_key) {
          ctx->container->response_headers_[lower_key] = evt->header_value;
          break;
        }
      }
      break;
    }

    case HTTP_EVENT_ON_DATA:
      if (ctx->req->capture_response && evt->data_len > 0) {
        const size_t remaining =
            ctx->req->max_response_buffer_size - ctx->container->body.size();
        if (remaining > 0) {
          const size_t to_copy =
              std::min(static_cast<size_t>(evt->data_len), remaining);
          ctx->container->body.append(static_cast<char *>(evt->data), to_copy);
          if (to_copy < static_cast<size_t>(evt->data_len)) {
            ESP_LOGW(TAG, "Response body truncated at %u bytes (max_response_buffer_size)",
                     ctx->req->max_response_buffer_size);
          }
        }
      }
      break;

    default:
      break;
  }
  return ESP_OK;
}

// ── Component lifecycle ───────────────────────────────────────────────────────

void HttpRequestAsyncComponent::setup() {
  this->request_queue_ =
      xQueueCreate(QUEUE_DEPTH, sizeof(PendingRequest *));
  this->response_queue_ =
      xQueueCreate(QUEUE_DEPTH, sizeof(PendingRequest *));

  if (this->request_queue_ == nullptr || this->response_queue_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create FreeRTOS queues — out of heap");
    this->mark_failed();
    return;
  }

  this->worker_task_handles_.resize(this->task_count_);
  for (uint8_t i = 0; i < this->task_count_; i++) {
    char task_name[16];
    snprintf(task_name, sizeof(task_name), "http_async_w_%u", i);

    const BaseType_t ret = xTaskCreate(
        HttpRequestAsyncComponent::worker_task_fn,
        task_name,
        this->task_stack_size_,
        this,
        this->task_priority_,
        &this->worker_task_handles_[i]);

    if (ret != pdPASS) {
      ESP_LOGE(TAG, "Failed to create worker task %u/%u (stack=%u, priority=%u)",
               i + 1, this->task_count_, this->task_stack_size_, this->task_priority_);
      this->mark_failed();
      return;
    }
  }

  ESP_LOGD(TAG, "Started %u worker task(s) (stack=%u each, priority=%u)",
           this->task_count_, this->task_stack_size_, this->task_priority_);
}

void HttpRequestAsyncComponent::loop() {
  PendingRequest *req = nullptr;

  // Drain completed requests — fire their callbacks from the main loop task.
  while (xQueueReceive(this->response_queue_, &req, 0) == pdTRUE) {
    if (req->container != nullptr && req->container->success) {
      if (req->on_response_cb) {
        req->on_response_cb(req->container);
      }
    } else {
      if (req->on_error_cb) {
        req->on_error_cb();
      }
    }
    // Resume the automation chain (next step after the HTTP action).
    if (req->on_complete_cb) {
      req->on_complete_cb();
    }
    delete req;  // NOLINT(cppcoreguidelines-owning-memory)
  }
}

void HttpRequestAsyncComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "HTTP Request Async:");
  ESP_LOGCONFIG(TAG, "  Timeout:          %u ms", this->timeout_ms_);
  ESP_LOGCONFIG(TAG, "  Follow redirects: %s",
                this->follow_redirects_ ? "true" : "false");
  ESP_LOGCONFIG(TAG, "  Redirect limit:   %u", this->redirect_limit_);
  ESP_LOGCONFIG(TAG, "  Verify SSL:       %s",
                this->verify_ssl_ ? "true" : "false");
  ESP_LOGCONFIG(TAG, "  Custom CA cert:   %s",
                this->ca_certificate_.empty() ? "no" : "yes");
  ESP_LOGCONFIG(TAG, "  RX buffer:        %u bytes", this->buffer_size_rx_);
  ESP_LOGCONFIG(TAG, "  TX buffer:        %u bytes", this->buffer_size_tx_);
  ESP_LOGCONFIG(TAG, "  Worker tasks:     %u", this->task_count_);
  ESP_LOGCONFIG(TAG, "  Worker stack:     %u bytes each (~%u kB total)",
                this->task_stack_size_,
                (this->task_count_ * this->task_stack_size_ + 512) / 1024);
  ESP_LOGCONFIG(TAG, "  Worker priority:  %u", this->task_priority_);
  ESP_LOGCONFIG(TAG, "  User-Agent:       %s", this->useragent_.c_str());
}

// ── Request queueing ──────────────────────────────────────────────────────────

void HttpRequestAsyncComponent::enqueue_request(PendingRequest *req) {
  // Check connectivity before touching the queue so that on_error fires
  // predictably even when execute_request_() is overridden by a subclass.
  if (!network::is_connected()) {
    ESP_LOGW(TAG, "Not connected — dropping request to %s", req->url.c_str());
    if (req->on_error_cb)
      req->on_error_cb();
    if (req->on_complete_cb)
      req->on_complete_cb();
    delete req;  // NOLINT
    return;
  }
  // Non-blocking send: if the queue is full the request is dropped.
  if (xQueueSend(this->request_queue_, &req, 0) != pdTRUE) {
    ESP_LOGW(TAG,
             "Request queue full (depth=%u) — dropping request to %s. "
             "Increase task_stack_size or reduce request rate.",
             QUEUE_DEPTH, req->url.c_str());
    if (req->on_error_cb) {
      req->on_error_cb();
    }
    if (req->on_complete_cb) {
      req->on_complete_cb();
    }
    delete req;  // NOLINT
  }
}

// ── Worker task ───────────────────────────────────────────────────────────────

void HttpRequestAsyncComponent::worker_task_fn(void *param) {
  auto *self = static_cast<HttpRequestAsyncComponent *>(param);
  PendingRequest *req = nullptr;

  while (true) {
    // Block indefinitely until a request arrives.
    if (xQueueReceive(self->request_queue_, &req, portMAX_DELAY) == pdTRUE) {
      self->execute_request_(req);
      // execute_request_() is responsible for posting req to response_queue_.
    }
  }
}

// ── HTTP execution ────────────────────────────────────────────────────────────

void HttpRequestAsyncComponent::execute_request_(PendingRequest *req) {
  auto container = std::make_shared<HttpContainer>();
  req->container = container;

  const uint64_t start_us = esp_timer_get_time();

  // Secondary connectivity guard: the connection may have dropped between
  // enqueue_request() and now (e.g. WiFi reassociation). enqueue_request()
  // already checked; this is just a best-effort defensive check.
  if (!network::is_connected()) {
    ESP_LOGW(TAG, "Network lost before request started: %s", req->url.c_str());
    container->success = false;
    container->duration_ms =
        static_cast<uint32_t>((esp_timer_get_time() - start_us) / 1000ULL);
    xQueueSend(this->response_queue_, &req, portMAX_DELAY);
    return;
  }

  // ── Map method string to ESP-IDF enum ─────────────────────────────────
  esp_http_client_method_t idf_method = HTTP_METHOD_GET;
  if (req->method == "POST") {
    idf_method = HTTP_METHOD_POST;
  } else if (req->method == "PUT") {
    idf_method = HTTP_METHOD_PUT;
  } else if (req->method == "DELETE") {
    idf_method = HTTP_METHOD_DELETE;
  } else if (req->method == "PATCH") {
    idf_method = HTTP_METHOD_PATCH;
  }

  // ── Build ESP-IDF client config ────────────────────────────────────────
  EventHandlerCtx ctx{req, container.get()};

  esp_http_client_config_t cfg = {};
  cfg.url = req->url.c_str();
  cfg.method = idf_method;
  cfg.timeout_ms = static_cast<int>(this->timeout_ms_);
  cfg.disable_auto_redirect = !this->follow_redirects_;
  cfg.max_redirection_count = this->redirect_limit_;
  cfg.user_agent = this->useragent_.c_str();
  cfg.buffer_size = this->buffer_size_rx_;
  cfg.buffer_size_tx = this->buffer_size_tx_;
  cfg.keep_alive_enable = false;
  cfg.event_handler = http_event_handler;
  cfg.user_data = &ctx;

  const bool is_https = req->url.rfind("https:", 0) == 0;
  if (is_https) {
    if (!this->ca_certificate_.empty()) {
      cfg.cert_pem = this->ca_certificate_.c_str();
    } else if (this->verify_ssl_) {
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
      cfg.crt_bundle_attach = esp_crt_bundle_attach;
#endif
    } else {
      cfg.skip_cert_common_name_check = true;
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
      cfg.crt_bundle_attach = esp_crt_bundle_attach;
#endif
    }
  }

  // ── Open connection ────────────────────────────────────────────────────
  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (client == nullptr) {
    ESP_LOGE(TAG, "esp_http_client_init() failed for %s", req->url.c_str());
    container->success = false;
    container->duration_ms =
        static_cast<uint32_t>((esp_timer_get_time() - start_us) / 1000ULL);
    xQueueSend(this->response_queue_, &req, portMAX_DELAY);
    return;
  }

  // ── Set request headers ────────────────────────────────────────────────
  for (const auto &hdr : req->request_headers) {
    esp_http_client_set_header(client, hdr.name.c_str(), hdr.value.c_str());
  }

  // ── Open and write body ────────────────────────────────────────────────
  const int body_len = static_cast<int>(req->body.size());
  esp_err_t err = esp_http_client_open(client, body_len);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_http_client_open() failed: %s", esp_err_to_name(err));
    esp_http_client_cleanup(client);
    container->success = false;
    container->duration_ms =
        static_cast<uint32_t>((esp_timer_get_time() - start_us) / 1000ULL);
    xQueueSend(this->response_queue_, &req, portMAX_DELAY);
    return;
  }

  if (body_len > 0) {
    int remaining = body_len;
    int offset = 0;
    while (remaining > 0) {
      const int written =
          esp_http_client_write(client, req->body.c_str() + offset, remaining);
      if (written < 0) {
        err = ESP_FAIL;
        break;
      }
      offset += written;
      remaining -= written;
    }
  }

  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to write request body: %s", esp_err_to_name(err));
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    container->success = false;
    container->duration_ms =
        static_cast<uint32_t>((esp_timer_get_time() - start_us) / 1000ULL);
    xQueueSend(this->response_queue_, &req, portMAX_DELAY);
    return;
  }

  // ── Fetch response headers ─────────────────────────────────────────────
  const int64_t hdr_len = esp_http_client_fetch_headers(client);
  container->status_code = esp_http_client_get_status_code(client);

  // status_code <= 0 means we never received an HTTP response:
  // the socket timed out, DNS failed, TLS handshake failed, connection was reset,
  // or the server hung up. hdr_len can be -1 for both "chunked response" (success)
  // and "transport error", so status_code is the reliable discriminator.
  if (container->status_code <= 0) {
    ESP_LOGW(TAG, "HTTP transport error (status=%d) for %s",
             container->status_code, req->url.c_str());
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    container->success = false;
    container->duration_ms =
        static_cast<uint32_t>((esp_timer_get_time() - start_us) / 1000ULL);
    if (xQueueSend(this->response_queue_, &req, pdMS_TO_TICKS(100)) != pdTRUE) {
      ESP_LOGE(TAG, "Response queue full — request to %s leaked", req->url.c_str());
      delete req;  // NOLINT
    }
    return;
  }
  // hdr_len is -1 for chunked responses (no Content-Length), >= 0 for known sizes.
  container->content_length = hdr_len >= 0 ? static_cast<size_t>(hdr_len) : 0;

  // ── Read response body (if requested) ─────────────────────────────────
  // The event handler already collected body chunks into container->body via
  // HTTP_EVENT_ON_DATA during esp_http_client_fetch_headers. For chunked
  // responses we need to drain the remaining data.
  if (req->capture_response && esp_http_client_is_chunked_response(client)) {
    char buf[256];
    int read_len = 0;
    while ((read_len = esp_http_client_read(client, buf, sizeof(buf))) > 0) {
      const size_t remaining_cap =
          req->max_response_buffer_size - container->body.size();
      if (remaining_cap > 0) {
        container->body.append(buf, std::min(static_cast<size_t>(read_len),
                                             remaining_cap));
      }
    }
  }

  container->duration_ms =
      static_cast<uint32_t>((esp_timer_get_time() - start_us) / 1000ULL);
  // 2xx / 3xx → success (on_response fires)
  // 4xx / 5xx → failure (on_error fires)
  // Transport failure is handled by the early-return paths above (success stays false).
  container->success = (container->status_code >= 200 && container->status_code < 400);

  ESP_LOGD(TAG, "%s %s → %d (%u ms, %u bytes)", req->method.c_str(),
           req->url.c_str(), container->status_code, container->duration_ms,
           container->body.size());

  esp_http_client_close(client);
  esp_http_client_cleanup(client);

  // Hand the completed request to the main loop task.
  if (xQueueSend(this->response_queue_, &req, pdMS_TO_TICKS(100)) != pdTRUE) {
    // Response queue full — this should not happen in normal operation.
    // If it does, both memory and the automation will leak.
    ESP_LOGE(TAG, "Response queue full — request to %s leaked", req->url.c_str());
    delete req;  // NOLINT: prevent heap leak; automation will stall
  }
}

}  // namespace http_request_async
}  // namespace esphome

#endif  // USE_ESP32
