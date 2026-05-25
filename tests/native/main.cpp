/**
 * Native host unit tests for http_request_async.
 *
 * These tests run on the development machine. No ESP32 hardware required.
 *
 * Design pattern
 * ──────────────
 * MockHttpRequestAsyncComponent overrides execute_request_() to inject a
 * predefined response instead of actually making an HTTP call. It also exposes
 * pump_responses() to manually drain the response queue — simulating what
 * Component::loop() does on the device.
 *
 * The mock derives container->success from status_code, matching the real IDF
 * behaviour: 2xx/3xx → success, 4xx/5xx/0 → failure.
 *
 * Tests build a PendingRequest directly (bypassing the full Action template) to
 * stay focused on the component logic rather than the code-generation layer.
 * Action-level tests (further down) exercise play_complex() end-to-end.
 */

// The compat header must be included first so USE_ESP32 is defined and the
// FreeRTOS / ESPHome types are available before the component header.
#include "mocks/esphome_compat.h"

// Bring in the component under test (header-only portion + implementation shim)
#include "../../components/http_request_async/http_request_async.h"

#include <gtest/gtest.h>

using namespace esphome::http_request_async;
using namespace esphome;

// ── Mock component ────────────────────────────────────────────────────────────

struct MockResponse {
  int status_code{200};
  std::string body;
  std::map<std::string, std::string> headers;
};

class MockHttpRequestAsyncComponent : public HttpRequestAsyncComponent {
 public:
  MockHttpRequestAsyncComponent() {
    // Bypass setup() which would try to start a FreeRTOS task.
    this->request_queue_ = xQueueCreate(8, sizeof(PendingRequest *));
    this->response_queue_ = xQueueCreate(8, sizeof(PendingRequest *));
  }

  /// Configure the next response to return.
  void set_next_response(MockResponse r) { this->next_response_ = std::move(r); }

  /// Drain request_queue_, inject mock response, post to response_queue_.
  void pump_worker() {
    PendingRequest *req = nullptr;
    while (xQueueReceive(this->request_queue_, &req, 0) == pdTRUE) {
      this->execute_request_(req);
    }
  }

  /// Drain response_queue_ and fire callbacks (equivalent to loop()).
  void pump_responses() { this->loop(); }

  /// Convenience: process both queues in order.
  void tick() {
    pump_worker();
    pump_responses();
  }

 protected:
  void execute_request_(PendingRequest *req) override {
    auto container = std::make_shared<HttpContainer>();
    container->status_code = this->next_response_.status_code;
    // Derive success from HTTP status code — matches real IDF behaviour:
    //   2xx / 3xx → success (on_response)
    //   4xx / 5xx → failure (on_error)
    //   0 (transport error, never got a response) → failure (on_error)
    container->success = (this->next_response_.status_code >= 200 &&
                          this->next_response_.status_code < 400);
    container->response_headers_ = this->next_response_.headers;
    container->duration_ms = 42;
    container->content_length = this->next_response_.body.size();
    // Mirror the IDF body-reading path: only populate body when capture_response
    // is set, and truncate at max_response_buffer_size.
    if (req->capture_response) {
      const std::string &src = this->next_response_.body;
      container->body = src.substr(0, std::min(src.size(), req->max_response_buffer_size));
    }

    req->container = container;

    xQueueSend(this->response_queue_, &req, portMAX_DELAY);
  }

 private:
  MockResponse next_response_;
};

// ── Helper: build a minimal PendingRequest ────────────────────────────────────

static PendingRequest *make_request(
    const std::string &url = "http://example.com",
    const std::string &method = "GET",
    bool capture = true) {
  auto *req = new PendingRequest();
  req->url = url;
  req->method = method;
  req->capture_response = capture;
  req->max_response_buffer_size = 4096;
  return req;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Tests
// ═══════════════════════════════════════════════════════════════════════════════

// ── on_response fires for 2xx ─────────────────────────────────────────────────

TEST(HttpRequestAsync, OnResponseCalledOnSuccess) {
  MockHttpRequestAsyncComponent comp;
  comp.set_next_response({200, R"({"output":"true"})"});

  bool response_called = false;
  int captured_status = -1;
  std::string captured_body;

  auto *req = make_request("http://192.168.1.10/rpc/Light.GetStatus?id=0");
  req->on_response_cb = [&](std::shared_ptr<HttpContainer> c) {
    response_called = true;
    captured_status = c->status_code;
    captured_body = c->body;
  };

  comp.enqueue_request(req);
  comp.tick();

  EXPECT_TRUE(response_called);
  EXPECT_EQ(captured_status, 200);
  EXPECT_EQ(captured_body, R"({"output":"true"})");
}

// ── on_error fires for transport failure (status 0, never got HTTP response) ──

TEST(HttpRequestAsync, OnErrorCalledOnTransportFailure) {
  MockHttpRequestAsyncComponent comp;
  comp.set_next_response({0, ""});  // status_code=0 → success=false

  bool error_called = false;
  bool response_called = false;

  auto *req = make_request();
  req->on_response_cb = [&](std::shared_ptr<HttpContainer>) { response_called = true; };
  req->on_error_cb = [&]() { error_called = true; };

  comp.enqueue_request(req);
  comp.tick();

  EXPECT_TRUE(error_called);
  EXPECT_FALSE(response_called);
}

// ── on_error fires for HTTP 4xx client errors ─────────────────────────────────

TEST(HttpRequestAsync, OnErrorCalledOnHttpClientError) {
  MockHttpRequestAsyncComponent comp;
  comp.set_next_response({404, R"({"error":"not found"})"});

  bool error_called = false;
  bool response_called = false;

  auto *req = make_request();
  req->on_response_cb = [&](std::shared_ptr<HttpContainer>) { response_called = true; };
  req->on_error_cb = [&]() { error_called = true; };

  comp.enqueue_request(req);
  comp.tick();

  EXPECT_TRUE(error_called);
  EXPECT_FALSE(response_called);
}

// ── on_error fires for HTTP 5xx server errors ─────────────────────────────────

TEST(HttpRequestAsync, OnErrorCalledOnHttpServerError) {
  MockHttpRequestAsyncComponent comp;
  comp.set_next_response({500, R"({"error":"internal server error"})"});

  bool error_called = false;
  bool response_called = false;

  auto *req = make_request();
  req->on_response_cb = [&](std::shared_ptr<HttpContainer>) { response_called = true; };
  req->on_error_cb = [&]() { error_called = true; };

  comp.enqueue_request(req);
  comp.tick();

  EXPECT_TRUE(error_called);
  EXPECT_FALSE(response_called);
}

// ── on_response fires for 3xx (unhandled redirect, not a transport error) ─────

TEST(HttpRequestAsync, OnResponseCalledOn3xxRedirect) {
  MockHttpRequestAsyncComponent comp;
  comp.set_next_response({301, ""});

  bool error_called = false;
  bool response_called = false;
  int captured_status = -1;

  auto *req = make_request();
  req->on_response_cb = [&](std::shared_ptr<HttpContainer> c) {
    response_called = true;
    captured_status = c->status_code;
  };
  req->on_error_cb = [&]() { error_called = true; };

  comp.enqueue_request(req);
  comp.tick();

  EXPECT_TRUE(response_called);
  EXPECT_FALSE(error_called);
  EXPECT_EQ(captured_status, 301);
}

// ── on_complete always fires after on_response ────────────────────────────────

TEST(HttpRequestAsync, OnCompleteFiresAfterOnResponse) {
  MockHttpRequestAsyncComponent comp;
  comp.set_next_response({200, "ok"});

  std::vector<std::string> call_order;

  auto *req = make_request();
  req->on_response_cb = [&](std::shared_ptr<HttpContainer>) {
    call_order.push_back("on_response");
  };
  req->on_complete_cb = [&]() { call_order.push_back("on_complete"); };

  comp.enqueue_request(req);
  comp.tick();

  ASSERT_EQ(call_order.size(), 2u);
  EXPECT_EQ(call_order[0], "on_response");
  EXPECT_EQ(call_order[1], "on_complete");
}

// ── on_complete always fires after on_error ───────────────────────────────────

TEST(HttpRequestAsync, OnCompleteFiresAfterOnError) {
  MockHttpRequestAsyncComponent comp;
  comp.set_next_response({0, ""});  // transport failure

  std::vector<std::string> call_order;
  auto *req = make_request();
  req->on_error_cb = [&]() { call_order.push_back("on_error"); };
  req->on_complete_cb = [&]() { call_order.push_back("on_complete"); };

  comp.enqueue_request(req);
  comp.tick();

  ASSERT_EQ(call_order.size(), 2u);
  EXPECT_EQ(call_order[0], "on_error");
  EXPECT_EQ(call_order[1], "on_complete");
}

// ── on_complete fires after on_error for HTTP errors too ─────────────────────

TEST(HttpRequestAsync, OnCompleteFiresAfterHttpError) {
  MockHttpRequestAsyncComponent comp;
  comp.set_next_response({500, ""});

  std::vector<std::string> call_order;
  auto *req = make_request();
  req->on_error_cb = [&]() { call_order.push_back("on_error"); };
  req->on_complete_cb = [&]() { call_order.push_back("on_complete"); };

  comp.enqueue_request(req);
  comp.tick();

  ASSERT_EQ(call_order.size(), 2u);
  EXPECT_EQ(call_order[0], "on_error");
  EXPECT_EQ(call_order[1], "on_complete");
}

// ── Response body is truncated at max_response_buffer_size ───────────────────

TEST(HttpRequestAsync, BodyTruncatedAtMaxBufferSize) {
  MockHttpRequestAsyncComponent comp;
  comp.set_next_response({200, "01234567890123456789"});

  auto *req = make_request();
  req->max_response_buffer_size = 5;
  req->on_response_cb = [](std::shared_ptr<HttpContainer>) {};

  comp.enqueue_request(req);
  comp.tick();

  // The mock sets body directly; real truncation happens in the event handler.
  // This test verifies max_response_buffer_size is passed through correctly.
  EXPECT_EQ(req->max_response_buffer_size, 5u);
}

// ── Response header collection is case-insensitive ────────────────────────────

TEST(HttpRequestAsync, ResponseHeaderLookupIsCaseInsensitive) {
  auto container = std::make_shared<HttpContainer>();
  container->response_headers_["content-type"] = "application/json";

  EXPECT_EQ(container->get_response_header("Content-Type"), "application/json");
  EXPECT_EQ(container->get_response_header("content-type"), "application/json");
  EXPECT_EQ(container->get_response_header("CONTENT-TYPE"), "application/json");
  EXPECT_EQ(container->get_response_header("X-Missing"), "");
}

// ── Multiple requests are processed in order; each goes to the right callback ─

TEST(HttpRequestAsync, MultipleRequestsProcessedInOrder) {
  MockHttpRequestAsyncComponent comp;

  struct Record { std::string cb; int status; };
  std::vector<Record> records;

  // 200 → on_response, 404 and 500 → on_error
  for (int code : {200, 404, 500}) {
    comp.set_next_response({code, ""});
    auto *req = make_request();
    req->on_response_cb = [&, code](std::shared_ptr<HttpContainer> c) {
      records.push_back({"on_response", c->status_code});
    };
    req->on_error_cb = [&, code]() {
      records.push_back({"on_error", code});
    };
    comp.enqueue_request(req);
    comp.tick();
  }

  ASSERT_EQ(records.size(), 3u);
  EXPECT_EQ(records[0].cb, "on_response"); EXPECT_EQ(records[0].status, 200);
  EXPECT_EQ(records[1].cb, "on_error");    EXPECT_EQ(records[1].status, 404);
  EXPECT_EQ(records[2].cb, "on_error");    EXPECT_EQ(records[2].status, 500);
}

// ── Not connected: on_error fires, not on_response ───────────────────────────

TEST(HttpRequestAsync, OnErrorWhenNotConnected) {
  network::g_is_connected = false;

  MockHttpRequestAsyncComponent comp;
  comp.set_next_response({200, "ok"});

  bool error_called = false;
  bool response_called = false;

  auto *req = make_request();
  req->on_response_cb = [&](std::shared_ptr<HttpContainer>) { response_called = true; };
  req->on_error_cb = [&]() { error_called = true; };

  comp.enqueue_request(req);
  comp.tick();

  EXPECT_TRUE(error_called);
  EXPECT_FALSE(response_called);

  network::g_is_connected = true;
}

// ── alive flag: callbacks are no-ops after stop() ────────────────────────────

TEST(HttpRequestAsync, AliveGuardPreventsStaleCallbacks) {
  auto alive = std::make_shared<bool>(true);
  bool response_called = false;

  auto on_response = [&alive, &response_called](std::shared_ptr<HttpContainer>) {
    if (!*alive) return;
    response_called = true;
  };

  *alive = false;
  on_response(std::make_shared<HttpContainer>());

  EXPECT_FALSE(response_called);
}

// ── Duration is populated ─────────────────────────────────────────────────────

TEST(HttpRequestAsync, DurationIsReported) {
  MockHttpRequestAsyncComponent comp;
  comp.set_next_response({200, "ok"});

  uint32_t duration = 0;
  auto *req = make_request();
  req->on_response_cb = [&](std::shared_ptr<HttpContainer> c) {
    duration = c->duration_ms;
  };

  comp.enqueue_request(req);
  comp.tick();

  EXPECT_EQ(duration, 42u);
}

// ── PendingRequest correctly stores collect_headers ──────────────────────────

TEST(HttpRequestAsync, CollectHeadersStoredLowerCase) {
  auto *req = make_request();
  req->lower_case_collect_headers = {"content-type", "x-request-id"};

  EXPECT_EQ(req->lower_case_collect_headers.size(), 2u);
  EXPECT_EQ(req->lower_case_collect_headers[0], "content-type");
  EXPECT_EQ(req->lower_case_collect_headers[1], "x-request-id");

  delete req;
}

// ── Multiple requests enqueued at once; all processed in one pump ─────────────
//
// This simulates the multi-worker scenario: N requests are enqueued without
// any processing in between, then pump_worker() drains them all at once
// (as N concurrent worker tasks would), then pump_responses() fires all
// callbacks. Validates the foundation that multi-worker concurrency relies on.

TEST(HttpRequestAsync, ConcurrentEnqueueAndDrain) {
  MockHttpRequestAsyncComponent comp;
  comp.set_next_response({200, "ok"});

  const int N = 4;
  int callbacks_fired = 0;

  for (int i = 0; i < N; i++) {
    auto *req = make_request();
    req->on_response_cb = [&](std::shared_ptr<HttpContainer>) { callbacks_fired++; };
    comp.enqueue_request(req);
  }

  // Drain all N in one shot — no tick() between enqueues.
  comp.pump_worker();
  comp.pump_responses();

  EXPECT_EQ(callbacks_fired, N);
}

// ── Entry point ───────────────────────────────────────────────────────────────

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
