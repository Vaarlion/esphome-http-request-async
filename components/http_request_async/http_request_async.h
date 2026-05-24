#pragma once

#ifdef USE_ESP32

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "esphome/components/json/json_util.h"
#include "esphome/core/alloc_helpers.h"
#include "esphome/core/application.h"
#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

namespace esphome {
namespace http_request_async {

static const char *const TAG = "http_request_async";

// ── Data types ────────────────────────────────────────────────────────────────

struct Header {
  std::string name;
  std::string value;
};

/// Immutable result of a completed HTTP request.
/// Heap-allocated and reference-counted; safe to hold beyond the on_response
/// callback if the user captures the shared_ptr.
class HttpContainer {
 public:
  int status_code{-1};
  size_t content_length{0};
  uint32_t duration_ms{0};
  /// Populated only when the action had capture_response: true.
  std::string body;
  /// false if a transport-level error occurred (network unreachable, DNS failure,
  /// TLS handshake error, timeout). HTTP ≥ 400 is still success = true.
  bool success{false};

  /// Case-insensitive response header lookup.
  /// Returns empty string if the header was not collected.
  std::string get_response_header(const std::string &name) const {
    const std::string lower = str_lower_case(name);
    auto it = this->response_headers_.find(lower);
    return (it != this->response_headers_.end()) ? it->second : std::string{};
  }

  // Internal — populated by the worker task via the event handler.
  std::map<std::string, std::string> response_headers_;
};

/// Internal per-request descriptor.
///
/// Ownership protocol (always raw pointer, never smart pointer at boundaries):
///   1. Action's play_complex() allocates with `new`, calls enqueue_request().
///   2. enqueue_request() sends the pointer into request_queue_ and releases it.
///   3. Worker task receives the pointer from request_queue_, fills container,
///      then sends the pointer into response_queue_.
///   4. loop() receives the pointer from response_queue_, fires callbacks,
///      then deletes it with `delete`.
///
/// All callbacks are invoked from the ESPHome main loop task — never from the
/// worker task. This means they are safe to access ESPHome component state.
struct PendingRequest {
  // ── Request parameters set by the action ────────────────────────────────
  std::string url;
  std::string method;
  std::string body;
  std::vector<Header> request_headers;
  std::vector<std::string> lower_case_collect_headers;
  bool capture_response{false};
  size_t max_response_buffer_size{1024};

  // ── Result filled by the worker task ─────────────────────────────────────
  std::shared_ptr<HttpContainer> container;

  // ── Callbacks invoked by loop() ───────────────────────────────────────────
  /// Fires the on_response trigger (with or without body, depending on the action).
  /// Receives the container regardless of success; callers check container->success.
  std::function<void(std::shared_ptr<HttpContainer>)> on_response_cb;
  /// Fires the on_error trigger.
  std::function<void()> on_error_cb;
  /// Resumes the automation chain (the step after the HTTP action).
  /// Called after on_response_cb / on_error_cb.
  std::function<void()> on_complete_cb;
};

// ── Component ─────────────────────────────────────────────────────────────────

class HttpRequestAsyncComponent : public Component {
 public:
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }
  void setup() override;
  void loop() override;
  void dump_config() override;

  // ── Config setters (called from generated code) ────────────────────────
  void set_timeout(uint32_t ms) { this->timeout_ms_ = ms; }
  void set_follow_redirects(bool v) { this->follow_redirects_ = v; }
  void set_redirect_limit(uint8_t v) { this->redirect_limit_ = v; }
  void set_verify_ssl(bool v) { this->verify_ssl_ = v; }
  void set_buffer_size_rx(uint16_t v) { this->buffer_size_rx_ = v; }
  void set_buffer_size_tx(uint16_t v) { this->buffer_size_tx_ = v; }
  void set_task_stack_size(uint32_t v) { this->task_stack_size_ = v; }
  void set_task_priority(uint8_t v) { this->task_priority_ = v; }
  void set_useragent(std::string v) { this->useragent_ = std::move(v); }
  void set_ca_certificate(std::string v) { this->ca_certificate_ = std::move(v); }

  /// Enqueue a request for async processing.
  /// Takes ownership of `req` (raw pointer). Non-blocking. Safe to call from the
  /// ESPHome main loop task.
  void enqueue_request(PendingRequest *req);

 protected:
  // ── Config ────────────────────────────────────────────────────────────────
  uint32_t timeout_ms_{4500};
  bool follow_redirects_{true};
  uint8_t redirect_limit_{3};
  bool verify_ssl_{true};
  uint16_t buffer_size_rx_{512};
  uint16_t buffer_size_tx_{512};
  uint32_t task_stack_size_{8192};
  uint8_t task_priority_{5};
  std::string useragent_;
  std::string ca_certificate_;

  // ── FreeRTOS ──────────────────────────────────────────────────────────────
  TaskHandle_t worker_task_handle_{nullptr};
  /// Main loop → worker: carries PendingRequest* (request pending execution)
  QueueHandle_t request_queue_{nullptr};
  /// Worker → main loop: carries PendingRequest* (request completed)
  QueueHandle_t response_queue_{nullptr};

  static constexpr uint8_t QUEUE_DEPTH = 8;

  static void worker_task_fn(void *param);

  /// Blocking — runs on the worker task. Performs the HTTP request and
  /// populates req->container. Posts req to response_queue_ when done.
  ///
  /// Virtual to allow test subclasses to inject mock responses without
  /// starting a FreeRTOS task.
  virtual void execute_request_(PendingRequest *req);
};

// ── Action template ───────────────────────────────────────────────────────────
//
// Inherits Action<Ts...> where Ts... are the enclosing automation's parameter
// types (e.g. std::string, int, int for a script with three parameters).
//
// play_complex() is overridden to NOT immediately advance to the next action.
// Instead it:
//   1. Builds a PendingRequest with the runtime-evaluated URL, headers, body.
//   2. Stores three lambdas in the request: on_response_cb, on_error_cb,
//      on_complete_cb. Each captures `this` and a copy of the automation args.
//   3. Calls enqueue_request() and returns. The main loop keeps running.
//
// When loop() drains the response queue it calls:
//   1. on_response_cb or on_error_cb (fires the YAML trigger).
//   2. on_complete_cb (advances to the next automation step via play_next_tuple_).
//
// is_running() returns true while a request is in flight. ESPHome's automation
// engine uses this to prevent re-entry when script mode is not queued.

template<typename... Ts>
class HttpRequestAsyncSendAction : public Action<Ts...> {
 public:
  explicit HttpRequestAsyncSendAction(HttpRequestAsyncComponent *parent)
      : parent_(parent) {}

  // ── Templatable config values ────────────────────────────────────────────
  TEMPLATABLE_VALUE(std::string, url)
  TEMPLATABLE_VALUE(const char *, method)
  TEMPLATABLE_VALUE(std::string, body)

  void set_capture_response(bool v) { this->capture_response_ = v; }
  void set_max_response_buffer_size(size_t v) { this->max_response_buffer_size_ = v; }

  void init_request_headers(size_t count) { this->request_headers_.init(count); }
  void add_request_header(const char *key, TemplatableFn<const char *, Ts...> value) {
    this->request_headers_.push_back({key, std::move(value)});
  }

  void add_collect_header(const char *lower_name) {
    this->lower_case_collect_headers_.emplace_back(lower_name);
  }

  void init_json(size_t count) { this->json_pairs_.init(count); }
  void add_json(const char *key, TemplatableValue<std::string, Ts...> value) {
    this->json_pairs_.push_back({key, std::move(value)});
  }
  void set_json(std::function<void(Ts..., JsonObject)> func) {
    this->json_func_ = std::move(func);
  }

  // ── Trigger accessors (used by Python codegen) ───────────────────────────

  /// on_response trigger when capture_response: true
  /// Lambda variables: (response: shared_ptr<HttpContainer>, body: string&, Ts...)
  Trigger<std::shared_ptr<HttpContainer>, std::string &, Ts...> *get_response_trigger() {
    return &this->response_trigger_;
  }

  /// on_response trigger when capture_response: false
  /// Lambda variables: (response: shared_ptr<HttpContainer>, Ts...)
  Trigger<std::shared_ptr<HttpContainer>, Ts...> *get_response_trigger_no_body() {
    return &this->response_trigger_no_body_;
  }

  /// on_error trigger
  /// Lambda variables: (Ts...)
  Trigger<Ts...> *get_error_trigger() { return &this->error_trigger_; }

  // ── Async action implementation ──────────────────────────────────────────

  // play() is pure virtual in Action<Ts...> (ESPHome 2024.x+).
  // play_complex() is overridden below so play() is never called in practice;
  // this stub satisfies the abstract class requirement.
  void play(const Ts &...x) override {}  // NOLINT(readability-named-parameter)

  void play_complex(const Ts &...x) override {
    // Match the base class contract: increment num_running_ before deferring.
    // play_next_tuple_() checks num_running_ > 0 before advancing the chain.
    this->num_running_++;

    auto *req = new PendingRequest();  // NOLINT(cppcoreguidelines-owning-memory)

    // ── Evaluate templatable fields ──────────────────────────────────────
    req->url = this->url_.value(x...);
    req->method = std::string(this->method_.value(x...));
    req->capture_response = this->capture_response_;
    req->max_response_buffer_size = this->max_response_buffer_size_;
    req->lower_case_collect_headers = this->lower_case_collect_headers_;

    // Request headers
    for (const auto &kv : this->request_headers_) {
      req->request_headers.push_back({kv.first, kv.second.value(x...)});
    }

    // Body: verbatim string takes priority; then JSON lambda; then JSON dict
    if (this->body_.has_value()) {
      req->body = this->body_.value(x...);
    } else if (this->json_func_ != nullptr) {
      req->body = json::build_json(
          [this, x...](JsonObject root) { this->json_func_(x..., root); });
    } else if (!this->json_pairs_.empty()) {
      req->body = json::build_json([this, x...](JsonObject root) {
        for (const auto &pair : this->json_pairs_) {
          root[pair.first] = pair.second.value(x...);
        }
      });
    }

    // ── Callbacks ────────────────────────────────────────────────────────
    //
    // `alive` is a shared flag between the action instance and the lambdas
    // stored in the request. If stop() is called before the response arrives,
    // `*alive` is set to false and the callbacks become no-ops.
    this->is_waiting_ = true;
    auto alive = std::make_shared<bool>(true);
    this->alive_ = alive;

    const bool capture = this->capture_response_;
    auto args_tuple = std::make_tuple(x...);

    req->on_response_cb = [this, alive, args_tuple,
                           capture](std::shared_ptr<HttpContainer> container) {
      if (!*alive)
        return;
      if (capture) {
        // Pass body by reference — the lambda runs synchronously so the copy
        // in `container->body` is alive for the entire trigger invocation.
        std::string body = container->body;
        std::apply(
            [this, &container, &body](Ts... a) {
              this->response_trigger_.trigger(container, body, a...);
            },
            args_tuple);
      } else {
        std::apply(
            [this, &container](Ts... a) {
              this->response_trigger_no_body_.trigger(container, a...);
            },
            args_tuple);
      }
    };

    req->on_error_cb = [this, alive, args_tuple]() {
      if (!*alive)
        return;
      std::apply([this](Ts... a) { this->error_trigger_.trigger(a...); },
                 args_tuple);
    };

    req->on_complete_cb = [this, alive, args_tuple]() {
      if (!*alive)
        return;
      this->is_waiting_ = false;
      this->play_next_tuple_(args_tuple);
    };

    this->parent_->enqueue_request(req);
    // Returns immediately. The automation chain is resumed by on_complete_cb.
  }

  bool is_running() override { return this->is_waiting_; }

  void stop() override {
    // Mark all in-flight callbacks as dead so they become no-ops when they
    // eventually fire. We can't cancel the network request itself.
    // NOTE: do NOT call stop_next_() here — that is the responsibility of
    // Action::stop_complex(), which calls stop() then stop_next_().
    if (this->alive_) {
      *this->alive_ = false;
    }
    this->is_waiting_ = false;
  }

 protected:
  HttpRequestAsyncComponent *parent_;
  bool capture_response_{false};
  size_t max_response_buffer_size_{1024};
  std::vector<std::string> lower_case_collect_headers_;

  FixedVector<std::pair<const char *, TemplatableFn<const char *, Ts...>>>
      request_headers_{};
  FixedVector<std::pair<const char *, TemplatableValue<std::string, Ts...>>>
      json_pairs_{};
  std::function<void(Ts..., JsonObject)> json_func_{nullptr};

  // Triggers — value members (one per action instance, created at startup).
  Trigger<std::shared_ptr<HttpContainer>, std::string &, Ts...> response_trigger_;
  Trigger<std::shared_ptr<HttpContainer>, Ts...> response_trigger_no_body_;
  Trigger<Ts...> error_trigger_;

  bool is_waiting_{false};
  std::shared_ptr<bool> alive_{std::make_shared<bool>(false)};
};

}  // namespace http_request_async
}  // namespace esphome

#endif  // USE_ESP32
