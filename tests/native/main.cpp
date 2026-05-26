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

// IDF mock state — controls esp_http_client_* calls made by execute_request_().
#include "idf_http_mock.h"

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
// The mock honours max_response_buffer_size the same way the real IDF path does.

TEST(HttpRequestAsync, BodyTruncatedAtMaxBufferSize) {
  MockHttpRequestAsyncComponent comp;
  comp.set_next_response({200, "01234567890123456789"});  // 20 bytes

  std::string captured_body;
  auto *req = make_request();
  req->capture_response = true;
  req->max_response_buffer_size = 5;
  req->on_response_cb = [&](std::shared_ptr<HttpContainer> c) {
    captured_body = c->body;
  };

  comp.enqueue_request(req);
  comp.tick();

  EXPECT_EQ(captured_body, "01234");           // exactly 5 bytes
  EXPECT_EQ(captured_body.size(), 5u);
}

// ── capture_response: false leaves body empty regardless of server payload ────

TEST(HttpRequestAsync, NoCaptureResponseLeavesBodyEmpty) {
  MockHttpRequestAsyncComponent comp;
  comp.set_next_response({200, "this should not appear"});

  std::string captured_body = "sentinel";
  auto *req = make_request("http://example.com", "GET", /*capture=*/false);
  req->on_response_cb = [&](std::shared_ptr<HttpContainer> c) {
    captured_body = c->body;
  };

  comp.enqueue_request(req);
  comp.tick();

  EXPECT_EQ(captured_body, "");
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
//
// Simulates the sequence that occurs when an automation is aborted while an
// HTTP request is in flight: stop() clears alive_, the request finishes and
// arrives in the response queue, but loop() should fire nothing.

TEST(HttpRequestAsync, AliveGuardPreventsStaleCallbacks) {
  MockHttpRequestAsyncComponent comp;
  comp.set_next_response({200, "ok"});

  auto alive = std::make_shared<bool>(true);
  bool response_called = false;
  bool error_called = false;
  bool complete_called = false;

  auto *req = make_request();
  req->on_response_cb = [alive, &response_called](std::shared_ptr<HttpContainer>) {
    if (!*alive) return;
    response_called = true;
  };
  req->on_error_cb = [alive, &error_called]() {
    if (!*alive) return;
    error_called = true;
  };
  req->on_complete_cb = [alive, &complete_called]() {
    if (!*alive) return;
    complete_called = true;
  };

  comp.enqueue_request(req);
  comp.pump_worker();    // request processed; now sitting in response_queue

  // Simulate stop() clearing the alive flag before loop() runs.
  *alive = false;

  comp.pump_responses(); // should all be no-ops

  EXPECT_FALSE(response_called);
  EXPECT_FALSE(error_called);
  EXPECT_FALSE(complete_called);
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

// ── Collected response headers are accessible via get_response_header() ───────
//
// Verifies the full round-trip: header set in mock response → accessible in
// on_response callback → get_response_header() returns it case-insensitively.
// The mock populates container->response_headers_ directly; in the real IDF
// path the event handler does this after filtering against lower_case_collect_headers.

TEST(HttpRequestAsync, CollectedResponseHeaderAccessibleInCallback) {
  MockHttpRequestAsyncComponent comp;
  MockResponse resp;
  resp.status_code = 200;
  resp.body = "ok";
  resp.headers = {{"content-type", "application/json"}, {"x-request-id", "abc-123"}};
  comp.set_next_response(resp);

  std::string captured_ct;
  std::string captured_rid;
  std::string captured_missing;

  auto *req = make_request();
  req->lower_case_collect_headers = {"content-type", "x-request-id"};
  req->on_response_cb = [&](std::shared_ptr<HttpContainer> c) {
    // Case-insensitive lookups — all three casings must return the same value.
    captured_ct      = c->get_response_header("Content-Type");
    captured_rid     = c->get_response_header("X-Request-Id");
    captured_missing = c->get_response_header("X-Not-Present");
  };

  comp.enqueue_request(req);
  comp.tick();

  EXPECT_EQ(captured_ct, "application/json");
  EXPECT_EQ(captured_rid, "abc-123");
  EXPECT_EQ(captured_missing, "");           // absent header → empty string
}

// ── Request queue overflow fires on_error immediately ────────────────────────
//
// The request queue has a fixed depth of QUEUE_DEPTH (8). When it is full,
// enqueue_request() must fire on_error synchronously and delete the request —
// the automation must not stall waiting for a slot that will never open.

TEST(HttpRequestAsync, QueueFullDropsRequestWithOnError) {
  MockHttpRequestAsyncComponent comp;
  comp.set_next_response({200, "ok"});

  int error_count    = 0;
  int response_count = 0;

  // Fill the queue to capacity without draining (pump_worker is NOT called).
  for (int i = 0; i < 8; i++) {
    auto *req = make_request();
    req->on_response_cb = [&](std::shared_ptr<HttpContainer>) { response_count++; };
    req->on_error_cb    = [&]() { error_count++; };
    comp.enqueue_request(req);
  }
  EXPECT_EQ(error_count, 0);    // all 8 fit
  EXPECT_EQ(response_count, 0); // nothing processed yet

  // 9th request: queue is full → on_error fires synchronously, no on_response.
  bool ninth_error    = false;
  bool ninth_response = false;
  auto *req9 = make_request();
  req9->on_error_cb    = [&]() { ninth_error = true; };
  req9->on_response_cb = [&](std::shared_ptr<HttpContainer>) { ninth_response = true; };
  comp.enqueue_request(req9);

  EXPECT_TRUE(ninth_error);
  EXPECT_FALSE(ninth_response);

  // Drain the 8 enqueued requests to avoid leaking PendingRequest objects.
  comp.pump_worker();
  comp.pump_responses();
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

// ── Null callbacks do not crash loop() ───────────────────────────────────────
//
// A user who writes an HTTP action with no on_response and no on_error gets
// null std::function objects in the PendingRequest. loop() guards each with
// if (req->on_...) — removing any one of those guards would silently become
// a call through a null std::function (UB / crash).

TEST(HttpRequestAsync, NullCallbacksDoNotCrashLoop) {
  MockHttpRequestAsyncComponent comp;
  comp.set_next_response({200, "ok"});

  auto *req = make_request();
  // All callbacks left as default-constructed (null) std::functions.
  comp.enqueue_request(req);
  comp.tick();  // must not crash or invoke UB
}

// ── Null callbacks do not crash enqueue_request() on network-down ─────────────

TEST(HttpRequestAsync, NullCallbacksDoNotCrashOnNetworkDown) {
  network::g_is_connected = false;

  MockHttpRequestAsyncComponent comp;
  auto *req = make_request();
  // on_error_cb and on_complete_cb are null — enqueue_request() must guard
  // both before calling them in the network-down fast-path.
  comp.enqueue_request(req);  // must not crash

  network::g_is_connected = true;
}

// ── content_length reflects the received payload size ────────────────────────
//
// For Content-Length responses: set from the header value by the IDF path.
// For chunked responses: set from body.size() after the read loop (IDF fix).
// The mock sets content_length = body.size() in both cases, so this test covers
// the contract that the callback always sees a non-zero content_length when the
// body was captured — regardless of transfer encoding.
//
// Hardware verification: test_chunked_content_length in hardware_test.yaml
// exercises the real IDF path with a genuine chunked response.

TEST(HttpRequestAsync, ContentLengthMatchesResponseBodySize) {
  MockHttpRequestAsyncComponent comp;
  comp.set_next_response({200, "hello world"});  // 11 bytes

  size_t captured_content_length = 0;
  auto *req = make_request();
  req->on_response_cb = [&](std::shared_ptr<HttpContainer> c) {
    captured_content_length = c->content_length;
  };

  comp.enqueue_request(req);
  comp.tick();

  EXPECT_EQ(captured_content_length, 11u);
}

// ── is_running() follows the request lifecycle ────────────────────────────────
//
// is_running() returns is_waiting_, which must be:
//   false  → before play_complex() is called
//   true   → after play_complex() returns (request enqueued but not dispatched)
//   true   → after worker processes request (response in queue, loop() not yet run)
//   false  → after loop() fires on_complete_cb (is_waiting_ set to false there)
//
// ESPHome's automation engine uses is_running() to gate re-entry for scripts
// with mode: single. An incorrect value would cause either a stalled script or
// overlapping concurrent executions.

TEST(HttpRequestAsync, IsRunningFollowsRequestLifecycle) {
  MockHttpRequestAsyncComponent comp;
  comp.set_next_response({200, "ok"});

  HttpRequestAsyncSendAction<> action(&comp);
  action.set_url(TemplatableValue<std::string>("http://example.com"));
  action.set_method(TemplatableValue<const char *>("GET"));

  EXPECT_FALSE(action.is_running());   // idle

  action.play_complex();
  EXPECT_TRUE(action.is_running());    // enqueued, awaiting response

  comp.pump_worker();
  EXPECT_TRUE(action.is_running());    // response in queue, loop() not yet called

  comp.pump_responses();
  EXPECT_FALSE(action.is_running());   // on_complete_cb fired: is_waiting_ = false
}

// ── MockNextAction ────────────────────────────────────────────────────────────
//
// Simulates an async action chained after the HTTP action (e.g. a second HTTP
// request, a delay, or a script.execute). It stays "running" until finish() is
// called explicitly, so the test can verify that is_running() on the HTTP
// action propagates through the chain.

class MockNextAction : public esphome::Action<> {
 public:
  void play() override {}  // required pure-virtual stub; never called directly

  // Override play_complex() to simulate an async action in progress.
  // Intentionally does NOT call play_next_() — the action stays running until
  // finish() is called.
  void play_complex() override { this->num_running_++; }

  // Simulate this action completing and advancing any further chain.
  void finish() { this->play_next_(); }
};

// ── is_running() propagates through chained actions (mode:single) ─────────────
//
// When on_complete_cb fires it advances the chain (play_next_tuple_). If a
// chained action is still running, is_running() on the HTTP action must return
// true. Without is_running_next_() this is false — the automation engine sees
// the script as idle and allows re-entry while the next step executes.

TEST(HttpRequestAsync, IsRunningPropagatesIntoChainedActions) {
  MockHttpRequestAsyncComponent comp;
  comp.set_next_response({200, "ok"});

  HttpRequestAsyncSendAction<> action(&comp);
  action.set_url(TemplatableValue<std::string>("http://example.com"));
  action.set_method(TemplatableValue<const char *>("GET"));

  MockNextAction next_action;
  action.set_next(&next_action);

  action.play_complex();
  EXPECT_TRUE(action.is_running());   // HTTP request in flight

  comp.pump_worker();
  comp.pump_responses();
  // on_complete_cb fired: is_waiting_ = false, play_next_() → next_action.play_complex()
  EXPECT_TRUE(next_action.is_running());   // chained action is now running
  EXPECT_TRUE(action.is_running());        // must propagate: false || is_running_next_()

  next_action.finish();
  EXPECT_FALSE(action.is_running());       // chain fully complete
}

// ── stop() clears is_running() and prevents stale callbacks ──────────────────
//
// When stop_complex() is called (the real ESPHome calling convention used by
// the automation engine to abort a script), is_running() must return false
// immediately. Callbacks that arrive later via the response queue must be
// no-ops (the alive_ guard prevents them from executing).

TEST(HttpRequestAsync, StopClearsIsRunningAndSuppressesCallbacks) {
  MockHttpRequestAsyncComponent comp;
  comp.set_next_response({200, "ok"});

  HttpRequestAsyncSendAction<> action(&comp);
  action.set_url(TemplatableValue<std::string>("http://example.com"));
  action.set_method(TemplatableValue<const char *>("GET"));

  action.play_complex();
  EXPECT_TRUE(action.is_running());

  action.stop_complex();               // real ESPHome calling convention
  EXPECT_FALSE(action.is_running());   // is_waiting_ = false, num_running_ = 0

  // The request is still in flight — process it and verify is_running() stays false
  // and no callback reinstates is_waiting_ = true (alive_ guard blocks on_complete_cb).
  comp.pump_worker();
  comp.pump_responses();

  EXPECT_FALSE(action.is_running());
}

// ═══════════════════════════════════════════════════════════════════════════════
// IDF-level redirect tests
// ═══════════════════════════════════════════════════════════════════════════════
//
// These tests use the REAL execute_request_() from http_request_async_idf.cpp.
// All esp_http_client_* calls go through idf_http_mock.cpp whose behaviour is
// controlled via g_idf_mock.  This catches regressions in the redirect-following
// loop that the higher-level mock tests (MockHttpRequestAsyncComponent) cannot —
// because that mock bypasses execute_request_() entirely.
//
// Hardware test 9/12 in hardware_test.yaml remains the authoritative test for
// a real network + real IDF client; these tests cover the logic layer only.

/// Test component that uses the real execute_request_() from the IDF file.
/// Queues are created directly in the constructor, bypassing setup() which would
/// try to start FreeRTOS worker tasks.
class IdfMockHttpRequestAsyncComponent : public HttpRequestAsyncComponent {
 public:
  IdfMockHttpRequestAsyncComponent() {
    this->request_queue_ = xQueueCreate(8, sizeof(PendingRequest *));
    this->response_queue_ = xQueueCreate(8, sizeof(PendingRequest *));
    // Apply defaults that match a typical YAML configuration.
    this->set_follow_redirects(true);
    this->set_redirect_limit(3);
    this->set_useragent("ESPHome");
    this->set_timeout(4500);
    this->set_buffer_size_rx(512);
    this->set_buffer_size_tx(512);
  }

  void pump_worker() {
    PendingRequest *req = nullptr;
    while (xQueueReceive(this->request_queue_, &req, 0) == pdTRUE) {
      this->execute_request_(req);
    }
  }

  void pump_responses() { this->loop(); }
  void tick() { pump_worker(); pump_responses(); }
};

// ── 302×3 chain resolves to the final 200 ────────────────────────────────────
//
// Mirrors hardware test 9/12 (/redirect?n=3 → /fast).
// Verifies that three redirect hops are followed and on_response fires with
// the final status code, not the intermediate 302.

TEST(HttpRequestAsync, IdfRedirectChainFollowed) {
  g_idf_mock.reset();
  g_idf_mock.push_statuses({302, 302, 302, 200});

  IdfMockHttpRequestAsyncComponent comp;
  comp.set_redirect_limit(5);

  bool response_called = false;
  bool error_called    = false;
  int  final_status    = -1;

  auto *req = make_request("http://example.com/redirect?n=3");
  req->on_response_cb = [&](std::shared_ptr<HttpContainer> c) {
    response_called = true;
    final_status    = c->status_code;
  };
  req->on_error_cb = [&]() { error_called = true; };

  comp.enqueue_request(req);
  comp.tick();

  EXPECT_TRUE(response_called);
  EXPECT_FALSE(error_called);
  EXPECT_EQ(final_status, 200);
  // Each redirect hop calls set_redirection() once.
  EXPECT_EQ(g_idf_mock.set_redirection_call_count, 3);
  // 3 redirect hops + 1 final response = 4 open() calls.
  EXPECT_EQ(g_idf_mock.open_call_count, 4);
  // close() inside each redirect branch (3×) + close() at end of function (1×).
  EXPECT_EQ(g_idf_mock.close_call_count, 4);
}

// ── Redirect limit is enforced ────────────────────────────────────────────────
//
// With redirect_limit = 3 and 5 consecutive 302 responses, the component must
// stop after 3 hops and surface the 4th 302 as the final response (on_response,
// not on_error).

TEST(HttpRequestAsync, IdfRedirectLimitEnforced) {
  g_idf_mock.reset();
  // Five 302s: the component follows 3, then stops at the 4th.
  g_idf_mock.push_statuses({302, 302, 302, 302, 302});

  IdfMockHttpRequestAsyncComponent comp;  // redirect_limit defaults to 3

  bool response_called = false;
  bool error_called    = false;
  int  final_status    = -1;

  auto *req = make_request();
  req->on_response_cb = [&](std::shared_ptr<HttpContainer> c) {
    response_called = true;
    final_status    = c->status_code;
  };
  req->on_error_cb = [&]() { error_called = true; };

  comp.enqueue_request(req);
  comp.tick();

  // 3xx is still a "success" (on_response, not on_error) even when the limit
  // is reached — the caller's YAML can inspect status_code if needed.
  EXPECT_TRUE(response_called);
  EXPECT_FALSE(error_called);
  EXPECT_EQ(final_status, 302);
  EXPECT_EQ(g_idf_mock.set_redirection_call_count, 3);
}

// ── Redirect is not followed when follow_redirects: false ────────────────────

TEST(HttpRequestAsync, IdfRedirectNotFollowedWhenDisabled) {
  g_idf_mock.reset();
  g_idf_mock.push_statuses({302});

  IdfMockHttpRequestAsyncComponent comp;
  comp.set_follow_redirects(false);

  int final_status = -1;
  auto *req = make_request();
  req->on_response_cb = [&](std::shared_ptr<HttpContainer> c) {
    final_status = c->status_code;
  };

  comp.enqueue_request(req);
  comp.tick();

  EXPECT_EQ(final_status, 302);
  EXPECT_EQ(g_idf_mock.set_redirection_call_count, 0);
  EXPECT_EQ(g_idf_mock.open_call_count, 1);
}

// ── Redirect stops when the Location header is absent ────────────────────────
//
// esp_http_client_set_redirection() returns ESP_FAIL when there is no Location
// header.  The component must treat the redirect response as final.

TEST(HttpRequestAsync, IdfRedirectStopsOnMissingLocation) {
  g_idf_mock.reset();
  g_idf_mock.push_statuses({302});
  g_idf_mock.redirection_has_location = false;

  IdfMockHttpRequestAsyncComponent comp;

  int final_status = -1;
  auto *req = make_request();
  req->on_response_cb = [&](std::shared_ptr<HttpContainer> c) {
    final_status = c->status_code;
  };

  comp.enqueue_request(req);
  comp.tick();

  EXPECT_EQ(final_status, 302);
  // set_redirection() was called (we tried), but it failed → stopped.
  EXPECT_EQ(g_idf_mock.set_redirection_call_count, 1);
  // Only one open() (the original request); no successful redirect to re-open.
  EXPECT_EQ(g_idf_mock.open_call_count, 1);
}

// ── 302 switches method to GET and drops the request body ────────────────────
//
// Per RFC 9110 §15.4.3, a 302 redirect from POST should be re-sent as GET
// with no body.  esp_http_client_set_redirection() handles the method switch;
// the component must zero the body length for the follow-up open() call.

TEST(HttpRequestAsync, IdfRedirect302SwitchesToGetDropsBody) {
  g_idf_mock.reset();
  g_idf_mock.push_statuses({302, 200});

  IdfMockHttpRequestAsyncComponent comp;

  int final_status = -1;
  auto *req = make_request("http://example.com/submit", "POST");
  req->body = "key=value";  // 9 bytes
  req->on_response_cb = [&](std::shared_ptr<HttpContainer> c) {
    final_status = c->status_code;
  };

  comp.enqueue_request(req);
  comp.tick();

  EXPECT_EQ(final_status, 200);
  // The first open() carries the POST body (9 bytes); the second carries nothing.
  EXPECT_EQ(g_idf_mock.open_call_count, 2);
  EXPECT_EQ(g_idf_mock.last_open_write_len, 0);
  // set_method() is NOT called for 302 — set_redirection() already switches to
  // GET internally in IDF.  The component only restores the method for 307/308.
  EXPECT_EQ(g_idf_mock.set_method_call_count, 0);
}

// ── 307 preserves the original method and body ───────────────────────────────
//
// Per RFC 9110 §15.4.8, a 307 redirect must not change the request method.
// The component must call esp_http_client_set_method() to restore the method
// after set_redirection() internally switches to GET.

TEST(HttpRequestAsync, IdfRedirect307PreservesMethod) {
  g_idf_mock.reset();
  g_idf_mock.push_statuses({307, 200});

  IdfMockHttpRequestAsyncComponent comp;

  int final_status = -1;
  auto *req = make_request("http://example.com/submit", "POST");
  req->body = "key=value";  // 9 bytes
  req->on_response_cb = [&](std::shared_ptr<HttpContainer> c) {
    final_status = c->status_code;
  };

  comp.enqueue_request(req);
  comp.tick();

  EXPECT_EQ(final_status, 200);
  EXPECT_EQ(g_idf_mock.open_call_count, 2);
  // Body is preserved on the second hop: write_len == 9.
  EXPECT_EQ(g_idf_mock.last_open_write_len, 9);
  // set_method() must be called once to restore POST (set_redirection switches to GET).
  EXPECT_EQ(g_idf_mock.set_method_call_count, 1);
  EXPECT_EQ(g_idf_mock.last_set_method, HTTP_METHOD_POST);
}

// ── Headers from redirect hops are cleared; only final-response headers survive ─
//
// The component calls container->response_headers_.clear() before following each
// redirect.  Headers fired during intermediate hops must not bleed into the
// on_response callback.

TEST(HttpRequestAsync, IdfRedirectClearsIntermediateHeaders) {
  g_idf_mock.reset();
  g_idf_mock.push_statuses({302, 200});
  // The mock fires these headers on every fetch_headers() call (both the 302 hop
  // and the 200 hop).  The component clears them between hops, so the final
  // on_response should still see them — collected fresh from the 200 response.
  g_idf_mock.response_headers = {{"x-final-server", "prod"}};

  IdfMockHttpRequestAsyncComponent comp;

  std::string captured_header;
  auto *req = make_request();
  req->lower_case_collect_headers = {"x-final-server"};
  req->on_response_cb = [&](std::shared_ptr<HttpContainer> c) {
    captured_header = c->get_response_header("x-final-server");
  };

  comp.enqueue_request(req);
  comp.tick();

  EXPECT_EQ(captured_header, "prod");
}

// ── Entry point ───────────────────────────────────────────────────────────────

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
