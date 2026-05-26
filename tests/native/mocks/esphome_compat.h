/**
 * ESPHome + FreeRTOS compatibility shims for native (host) unit tests.
 *
 * Include this FIRST in every test translation unit (or force it via
 * -include in CMakeLists). It defines all the types that http_request_async.h
 * depends on so the header compiles without the real ESP-IDF / FreeRTOS
 * source trees.
 *
 * The stub header files under mocks/freertos/ and mocks/esphome/ are empty
 * include guards — their role is only to satisfy #include directives inside
 * the component header. All actual type definitions live here.
 */
#pragma once

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <queue>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

// ── Compile-time platform guard ───────────────────────────────────────────────
#ifndef USE_ESP32
#define USE_ESP32
#endif

// ── FreeRTOS type / function shims ────────────────────────────────────────────
// Replace with thin wrappers around std::queue so component logic runs on host.

using TickType_t = uint32_t;
using BaseType_t = int;
using TaskHandle_t = void *;

constexpr TickType_t portMAX_DELAY = 0xFFFFFFFF;
constexpr BaseType_t pdTRUE = 1;
constexpr BaseType_t pdFALSE = 0;
constexpr BaseType_t pdPASS = 1;

inline TickType_t pdMS_TO_TICKS(uint32_t ms) { return ms; }

struct FakeQueue {
  explicit FakeQueue(uint32_t max_depth) : max_depth_(max_depth) {}
  std::queue<void *> q;
  uint32_t max_depth_;
};
using QueueHandle_t = FakeQueue *;

inline QueueHandle_t xQueueCreate(uint32_t depth, uint32_t /*item_size*/) {
  return new FakeQueue(depth);
}

inline BaseType_t xQueueSend(QueueHandle_t q, const void *item,
                              TickType_t timeout) {
  // portMAX_DELAY: block forever until space is available.
  // In the single-threaded mock this always succeeds — simulating that the
  // scheduler would eventually unblock the sender once loop() drains a slot.
  //
  // All *finite* timeouts (including 0) fail immediately if the queue is full.
  // This mirrors real FreeRTOS semantics: a timed send returns pdFALSE once the
  // deadline expires and the queue is still full. Tests that rely on queue-full
  // behaviour (e.g. enqueue_request overflow) use timeout=0; tests that verify
  // portMAX_DELAY never drops a request use a blocking send and expect success.
  if (timeout != portMAX_DELAY && q->q.size() >= q->max_depth_) {
    return pdFALSE;
  }
  void *val = nullptr;
  std::memcpy(&val, item, sizeof(void *));
  q->q.push(val);
  return pdTRUE;
}

inline BaseType_t xQueueReceive(QueueHandle_t q, void *out,
                                 TickType_t /*timeout*/) {
  if (q->q.empty())
    return pdFALSE;
  void *val = q->q.front();
  q->q.pop();
  std::memcpy(out, &val, sizeof(void *));
  return pdTRUE;
}

// Worker tasks are not created in native tests; the mock drives execute_request_
// manually.
inline BaseType_t xTaskCreate(void (*)(void *), const char *, uint32_t,
                               void *, uint32_t, TaskHandle_t *handle) {
  if (handle)
    *handle = reinterpret_cast<void *>(1);
  return pdPASS;
}

// ── esp_err stubs ─────────────────────────────────────────────────────────────
using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t ESP_FAIL = -1;
inline const char *esp_err_to_name(esp_err_t) { return "ESP_ERR"; }

// ── esp_timer stub ────────────────────────────────────────────────────────────
inline int64_t esp_timer_get_time() {
  using namespace std::chrono;
  return duration_cast<microseconds>(steady_clock::now().time_since_epoch())
      .count();
}

// ── Logging stubs ─────────────────────────────────────────────────────────────
#define ESP_LOGD(tag, fmt, ...) \
  fprintf(stderr, "[D][%s] " fmt "\n", (tag), ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) \
  fprintf(stderr, "[I][%s] " fmt "\n", (tag), ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) \
  fprintf(stderr, "[W][%s] " fmt "\n", (tag), ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...) \
  fprintf(stderr, "[E][%s] " fmt "\n", (tag), ##__VA_ARGS__)
#define ESP_LOGCONFIG(tag, fmt, ...) \
  fprintf(stderr, "[C][%s] " fmt "\n", (tag), ##__VA_ARGS__)

// ── ESPHome types ─────────────────────────────────────────────────────────────
namespace esphome {

inline std::string str_lower_case(const std::string &s) {
  std::string out = s;
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

namespace setup_priority {
static constexpr float AFTER_WIFI = 250.0f;
}

class Component {
 public:
  virtual ~Component() = default;
  virtual void setup() {}
  virtual void loop() {}
  virtual void dump_config() {}
  virtual float get_setup_priority() const { return 0.0f; }
  void mark_failed() { failed_ = true; }
  bool is_failed() const { return failed_; }

 private:
  bool failed_{false};
};

// ── Automation primitives ─────────────────────────────────────────────────────

template<typename... Ts>
class Trigger {
 public:
  void trigger(Ts... x) {
    for (auto &cb : callbacks_)
      cb(x...);
  }
  void add_callback(std::function<void(Ts...)> cb) {
    callbacks_.push_back(std::move(cb));
  }

 private:
  std::vector<std::function<void(Ts...)>> callbacks_;
};

template<typename... Ts>
class Action {
 public:
  virtual ~Action() = default;

  // Pure virtual — every concrete action must implement play().
  // Async actions override play_complex() and leave play() as a no-op stub.
  virtual void play(const Ts &...x) = 0;

  // Default play_complex: increment counter, call play(), advance chain.
  // Async actions override this and call play_next_tuple_() later.
  virtual void play_complex(const Ts &...x) {
    this->num_running_++;
    this->play(x...);
    this->play_next_(x...);
  }

  virtual bool is_running() { return this->num_running_ > 0 || this->is_running_next_(); }

  // stop() cleans up this action's state only.
  // Chain propagation is handled by stop_complex() below.
  virtual void stop() {}

  // stop_complex() mirrors the real ESPHome implementation.
  virtual void stop_complex() {
    if (this->num_running_) {
      this->stop();
      this->num_running_ = 0;
    }
    this->stop_next_();
  }

  void set_next(Action<Ts...> *next) { this->next_ = next; }

 protected:
  // Advance the automation chain after this action completes.
  // Guards on num_running_ > 0 to prevent double-advance.
  void play_next_(const Ts &...x) {
    if (this->num_running_ > 0) {
      this->num_running_--;
      if (this->next_)
        this->next_->play_complex(x...);
    }
  }

  void play_next_tuple_(const std::tuple<Ts...> &t) {
    std::apply([this](const Ts &...a) { this->play_next_(a...); }, t);
  }

  void stop_next_() {
    if (this->next_)
      this->next_->stop_complex();
  }

  // Returns true if any action in the remainder of the chain is still running.
  // Mirrors the real ESPHome Action::is_running_next_(). Used by async action
  // overrides so that is_running() stays true while chained actions execute.
  bool is_running_next_() {
    return this->next_ != nullptr && this->next_->is_running();
  }

  Action<Ts...> *next_{nullptr};
  int num_running_{0};
};

// ── TemplatableValue (simplified, single static-or-lambda variant) ─────────

template<typename T, typename... Ts>
class TemplatableValue {
 public:
  TemplatableValue() = default;
  explicit TemplatableValue(T val) : static_val_(std::move(val)), has_static_(true) {}
  explicit TemplatableValue(std::function<T(Ts...)> fn) : fn_(std::move(fn)) {}

  bool has_value() const { return has_static_ || fn_ != nullptr; }

  T value(Ts... x) const {
    if (fn_)
      return fn_(x...);
    return static_val_;
  }

 private:
  T static_val_{};
  bool has_static_{false};
  std::function<T(Ts...)> fn_;
};

template<typename T, typename... Ts>
using TemplatableFn = TemplatableValue<T, Ts...>;

// TEMPLATABLE_VALUE macro — mirrors the real ESPHome macro.
// Uses Ts... from the enclosing template<typename... Ts> class context.
#define TEMPLATABLE_VALUE(type, name)                                              \
  void set_##name(TemplatableValue<type, Ts...> v) { this->name##_ = std::move(v); } \
  TemplatableValue<type, Ts...> name##_{};

// ── FixedVector (growable in tests; fixed capacity on device) ─────────────────

template<typename T>
class FixedVector {
 public:
  void init(size_t /*max_size*/) {}  // no-op in tests
  void push_back(T item) { data_.push_back(std::move(item)); }
  auto begin() const { return data_.begin(); }
  auto end() const { return data_.end(); }
  bool empty() const { return data_.empty(); }
  size_t size() const { return data_.size(); }

 private:
  std::vector<T> data_;
};

// ── JSON stub ─────────────────────────────────────────────────────────────────
// The native tests do not exercise ArduinoJson. json::build_json produces a
// simple flat JSON from the root map. Tests that need to verify JSON body
// content should use the Tier-2 ESPHome compilation test instead.

using JsonObject = std::map<std::string, std::string> &;

namespace json {
inline std::string build_json(std::function<void(JsonObject)> builder) {
  std::map<std::string, std::string> root;
  builder(root);
  std::string out = "{";
  bool first = true;
  for (const auto &kv : root) {
    if (!first)
      out += ",";
    out += "\"" + kv.first + "\":\"" + kv.second + "\"";
    first = false;
  }
  return out + "}";
}
}  // namespace json

// ── Network connectivity flag ─────────────────────────────────────────────────

namespace network {
inline bool g_is_connected = true;
inline bool is_connected() { return g_is_connected; }
}  // namespace network

}  // namespace esphome
