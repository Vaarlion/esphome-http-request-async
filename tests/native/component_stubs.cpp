/**
 * Native implementations of HttpRequestAsyncComponent methods.
 *
 * The test build does NOT compile http_request_async_idf.cpp because that
 * file includes real ESP-IDF headers (esp_http_client.h, etc.) that are not
 * available on the host. Instead, this file provides minimal implementations
 * of the base class methods that the tests need:
 *
 *   - setup()         : no-op (MockHttpRequestAsyncComponent creates queues itself)
 *   - loop()          : drains response_queue_ and fires callbacks
 *   - dump_config()   : no-op
 *   - enqueue_request(): sends pointer into request_queue_
 *   - execute_request_: fallback (the mock always overrides this)
 *   - worker_task_fn  : no-op (tasks are not started in native tests)
 *
 * These implementations use only the FreeRTOS shims from esphome_compat.h —
 * no ESP-IDF calls.
 */

#include "mocks/esphome_compat.h"
#include "../../components/http_request_async/http_request_async.h"

namespace esphome {
namespace http_request_async {

void HttpRequestAsyncComponent::setup() {
  // MockHttpRequestAsyncComponent initialises the queues in its constructor,
  // bypassing this method.
}

void HttpRequestAsyncComponent::loop() {
  PendingRequest *req = nullptr;
  while (xQueueReceive(this->response_queue_, &req, 0) == pdTRUE) {
    if (req->container != nullptr && req->container->success) {
      if (req->on_response_cb)
        req->on_response_cb(req->container);
    } else {
      if (req->on_error_cb)
        req->on_error_cb();
    }
    if (req->on_complete_cb)
      req->on_complete_cb();
    delete req;  // NOLINT
  }
}

void HttpRequestAsyncComponent::dump_config() {}

void HttpRequestAsyncComponent::enqueue_request(PendingRequest *req) {
  // Check connectivity before touching the queue — this path is always taken
  // (not overridable by mocks), which is what the OnErrorWhenNotConnected
  // test exercises.
  if (!network::is_connected()) {
    ESP_LOGW(TAG, "Not connected — dropping request to %s", req->url.c_str());
    if (req->on_error_cb)
      req->on_error_cb();
    if (req->on_complete_cb)
      req->on_complete_cb();
    delete req;  // NOLINT
    return;
  }
  if (xQueueSend(this->request_queue_, &req, 0) != pdTRUE) {
    if (req->on_error_cb)
      req->on_error_cb();
    if (req->on_complete_cb)
      req->on_complete_cb();
    delete req;  // NOLINT
  }
}

void HttpRequestAsyncComponent::execute_request_(PendingRequest *req) {
  // Base class fallback — should never be called when the mock overrides it.
  auto container = std::make_shared<HttpContainer>();
  container->success = false;
  req->container = container;
  xQueueSend(this->response_queue_, &req, portMAX_DELAY);
}

void HttpRequestAsyncComponent::worker_task_fn(void * /*param*/) {
  // Not used in native tests — pump_worker() drives execute_request_() directly.
}

}  // namespace http_request_async
}  // namespace esphome
