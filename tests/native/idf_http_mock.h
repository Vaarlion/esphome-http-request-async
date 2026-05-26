#pragma once
/**
 * Controllable state for the esp_http_client mock.
 *
 * Set up g_idf_mock before calling tick(), then inspect it afterwards to verify
 * that execute_request_() made the right IDF calls.  Always call reset() at the
 * start of each test that uses IdfMockHttpRequestAsyncComponent.
 *
 * Design rules:
 *   - status_codes is a queue: one code consumed per get_status_code() call,
 *     which corresponds to one fetch_headers() / redirect hop.
 *   - response_headers are fired on every fetch_headers() call (including
 *     redirect hops).  The component clears container->response_headers_ before
 *     each redirect, so only headers from the *last* hop are visible in on_response.
 *   - response_body is readable via esp_http_client_read() on the final hop only
 *     (the mock resets the read position on each open()).
 */

#include <initializer_list>
#include <map>
#include <queue>
#include <string>

#include "mocks/esp_http_client.h"  // for esp_http_client_method_t

struct IdfHttpMockState {
  // ── Response sequence ─────────────────────────────────────────────────────
  std::queue<int>  status_codes;  ///< One code consumed per get_status_code() call.
  int64_t          hdr_len{0};    ///< Value returned by fetch_headers() (Content-Length; -1=chunked).
  std::string      response_body; ///< Body returned by read() on the final hop.

  /// Headers fired by the event handler on every fetch_headers() call.
  /// key: lower-case header name (matches what collect_headers expects).
  std::map<std::string, std::string> response_headers;

  // ── Redirect control ──────────────────────────────────────────────────────
  bool redirection_has_location{true}; ///< false → set_redirection() returns ESP_FAIL.
  bool open_fails{false};              ///< true  → open() returns ESP_FAIL.

  // ── Call tracking ─────────────────────────────────────────────────────────
  int  open_call_count{0};
  int  close_call_count{0};
  int  fetch_headers_call_count{0};
  int  set_redirection_call_count{0};
  int  set_method_call_count{0};
  esp_http_client_method_t last_set_method{HTTP_METHOD_GET};
  int  last_open_write_len{0}; ///< write_len arg passed to the most recent open().

  // ── Helpers ───────────────────────────────────────────────────────────────

  void reset() {
    while (!status_codes.empty())
      status_codes.pop();
    hdr_len = 0;
    response_body.clear();
    response_headers.clear();
    redirection_has_location = true;
    open_fails = false;
    open_call_count = 0;
    close_call_count = 0;
    fetch_headers_call_count = 0;
    set_redirection_call_count = 0;
    set_method_call_count = 0;
    last_set_method = HTTP_METHOD_GET;
    last_open_write_len = 0;
  }

  void push_statuses(std::initializer_list<int> codes) {
    for (int c : codes)
      status_codes.push(c);
  }
};

/// Singleton — defined in idf_http_mock.cpp; include this header to access it.
extern IdfHttpMockState g_idf_mock;
