#pragma once
/**
 * Mock esp_http_client.h for native (host) unit tests.
 *
 * Defines the IDF types and declares the esp_http_client_* functions used by
 * http_request_async_idf.cpp.  Actual mock implementations live in
 * idf_http_mock.cpp and are controlled via the g_idf_mock global (idf_http_mock.h).
 *
 * esp_err_t / ESP_OK / ESP_FAIL are defined in esphome_compat.h, which is
 * force-included before every translation unit via CMake's -include flag.
 */

// ── Opaque client handle ──────────────────────────────────────────────────────

using esp_http_client_handle_t = void *;

// ── Method enum ───────────────────────────────────────────────────────────────

typedef enum {
  HTTP_METHOD_GET    = 0,
  HTTP_METHOD_POST   = 1,
  HTTP_METHOD_PUT    = 2,
  HTTP_METHOD_PATCH  = 3,
  HTTP_METHOD_DELETE = 4,
  HTTP_METHOD_HEAD   = 5,
} esp_http_client_method_t;

// ── Event types ───────────────────────────────────────────────────────────────

typedef enum {
  HTTP_EVENT_ERROR = 0,
  HTTP_EVENT_ON_CONNECTED,
  HTTP_EVENT_HEADERS_SENT,
  HTTP_EVENT_ON_HEADER,
  HTTP_EVENT_ON_DATA,
  HTTP_EVENT_ON_FINISH,
  HTTP_EVENT_DISCONNECTED,
  HTTP_EVENT_REDIRECT,
} http_event_id_t;

struct esp_http_client_event_t {
  http_event_id_t         event_id;
  esp_http_client_handle_t client{nullptr};
  void                   *event_data{nullptr};
  int                     data_len{0};
  void                   *user_data{nullptr};
  char                   *header_key{nullptr};
  char                   *header_value{nullptr};
};

// ── Event handler callback ────────────────────────────────────────────────────

typedef int (*http_event_handle_cb)(esp_http_client_event_t *evt);

// ── Client configuration ──────────────────────────────────────────────────────

struct esp_http_client_config_t {
  const char              *url{nullptr};
  esp_http_client_method_t method{HTTP_METHOD_GET};
  int                      timeout_ms{0};
  bool                     disable_auto_redirect{false};
  int                      max_redirection_count{0};
  http_event_handle_cb     event_handler{nullptr};
  void                    *user_data{nullptr};
  const char              *cert_pem{nullptr};
  void                   (*crt_bundle_attach)(void *){nullptr};
  const char              *user_agent{nullptr};
  int                      buffer_size{0};
  int                      buffer_size_tx{0};
  bool                     keep_alive_enable{false};
};

// ── Function declarations ─────────────────────────────────────────────────────

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config);
int      esp_http_client_cleanup(esp_http_client_handle_t client);
int      esp_http_client_close(esp_http_client_handle_t client);
int      esp_http_client_open(esp_http_client_handle_t client, int write_len);
int      esp_http_client_write(esp_http_client_handle_t client, const char *buffer, int len);
int64_t  esp_http_client_fetch_headers(esp_http_client_handle_t client);
int      esp_http_client_get_status_code(esp_http_client_handle_t client);
int      esp_http_client_set_redirection(esp_http_client_handle_t client);
int      esp_http_client_set_method(esp_http_client_handle_t client, esp_http_client_method_t method);
int      esp_http_client_set_header(esp_http_client_handle_t client, const char *key, const char *value);
int      esp_http_client_read(esp_http_client_handle_t client, char *buffer, int len);
int      esp_http_client_set_url(esp_http_client_handle_t client, const char *url);
