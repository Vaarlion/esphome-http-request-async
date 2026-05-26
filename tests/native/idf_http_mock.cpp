/**
 * Mock implementations of esp_http_client_* functions.
 *
 * These replace the real ESP-IDF HTTP client library in the native test build,
 * allowing http_request_async_idf.cpp to compile and run on the host without any
 * real network or FreeRTOS infrastructure.
 *
 * All behaviour is controlled via the g_idf_mock global (see idf_http_mock.h).
 * Tests configure g_idf_mock before calling tick(), then inspect call counts and
 * captured values to assert that execute_request_() made the right IDF calls.
 *
 * Note: esphome_compat.h is force-included before this file via CMake's -include
 * flag, so esp_err_t / ESP_OK / ESP_FAIL are already defined.
 */

#include "idf_http_mock.h"

#include <algorithm>
#include <cstring>

// ── Global mock state ─────────────────────────────────────────────────────────

IdfHttpMockState g_idf_mock;

// ── Internal per-request state ────────────────────────────────────────────────

static http_event_handle_cb s_event_handler   = nullptr;
static void                *s_event_user_data = nullptr;
static int                  s_body_read_pos   = 0;

// ── Mock implementations ──────────────────────────────────────────────────────

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *cfg) {
  s_event_handler   = cfg->event_handler;
  s_event_user_data = cfg->user_data;
  s_body_read_pos   = 0;
  return reinterpret_cast<esp_http_client_handle_t>(0x42);  // non-null sentinel
}

int esp_http_client_cleanup(esp_http_client_handle_t /*client*/) {
  return 0;  // ESP_OK
}

int esp_http_client_close(esp_http_client_handle_t /*client*/) {
  g_idf_mock.close_call_count++;
  return 0;  // ESP_OK
}

int esp_http_client_open(esp_http_client_handle_t /*client*/, int write_len) {
  g_idf_mock.open_call_count++;
  g_idf_mock.last_open_write_len = write_len;
  s_body_read_pos = 0;  // Reset read position for each new open() / redirect hop.
  if (g_idf_mock.open_fails)
    return -1;  // ESP_FAIL
  return 0;     // ESP_OK
}

int esp_http_client_write(esp_http_client_handle_t /*client*/, const char * /*buf*/, int len) {
  return len;  // claim all bytes written
}

int64_t esp_http_client_fetch_headers(esp_http_client_handle_t /*client*/) {
  g_idf_mock.fetch_headers_call_count++;

  // Fire the configured response headers via the event handler.
  // The component clears container->response_headers_ before each redirect hop,
  // so only headers fired during the *last* fetch_headers() call survive into
  // the on_response callback — which is the correct, expected behaviour.
  if (s_event_handler) {
    for (const auto &kv : g_idf_mock.response_headers) {
      esp_http_client_event_t evt{};
      evt.event_id    = HTTP_EVENT_ON_HEADER;
      evt.user_data   = s_event_user_data;
      // header_key / header_value are non-const in the IDF struct; the
      // component's event handler only reads them, so const_cast is safe.
      evt.header_key   = const_cast<char *>(kv.first.c_str());
      evt.header_value = const_cast<char *>(kv.second.c_str());
      s_event_handler(&evt);
    }
  }

  return g_idf_mock.hdr_len;
}

int esp_http_client_get_status_code(esp_http_client_handle_t /*client*/) {
  if (g_idf_mock.status_codes.empty())
    return 200;
  const int code = g_idf_mock.status_codes.front();
  g_idf_mock.status_codes.pop();
  return code;
}

int esp_http_client_set_redirection(esp_http_client_handle_t /*client*/) {
  g_idf_mock.set_redirection_call_count++;
  // The real IDF implementation also switches the method to GET here.
  // We don't track the handle's internal method — we rely on the component
  // calling set_method() explicitly for 307/308 to restore the original method.
  return g_idf_mock.redirection_has_location ? 0 : -1;  // ESP_OK or ESP_FAIL
}

int esp_http_client_set_method(esp_http_client_handle_t /*client*/, esp_http_client_method_t method) {
  g_idf_mock.set_method_call_count++;
  g_idf_mock.last_set_method = method;
  return 0;  // ESP_OK
}

int esp_http_client_set_header(esp_http_client_handle_t /*client*/, const char * /*key*/, const char * /*value*/) {
  return 0;  // ESP_OK
}

int esp_http_client_read(esp_http_client_handle_t /*client*/, char *buffer, int len) {
  const std::string &body      = g_idf_mock.response_body;
  const int          remaining = static_cast<int>(body.size()) - s_body_read_pos;
  if (remaining <= 0)
    return 0;
  const int to_copy = std::min(len, remaining);
  std::memcpy(buffer, body.c_str() + s_body_read_pos, to_copy);
  s_body_read_pos += to_copy;
  return to_copy;
}

int esp_http_client_set_url(esp_http_client_handle_t /*client*/, const char * /*url*/) {
  return 0;  // ESP_OK — URL update is opaque to the mock
}
