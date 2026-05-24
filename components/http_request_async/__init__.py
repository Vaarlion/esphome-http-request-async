"""
ESPHome external component: http_request_async

Fully asynchronous HTTP client for ESP32 (ESP-IDF framework only).

Requests run on a dedicated FreeRTOS worker task; the ESPHome main loop is never
blocked. Inside an automation, the HTTP action pauses the chain and resumes it
when the response arrives — sequential scripts work exactly as written.

YAML interface intentionally mirrors http_request for easy migration.
"""

from esphome import automation
import esphome.codegen as cg
from esphome.components import esp32
import esphome.config_validation as cv
from esphome.const import (
    CONF_CAPTURE_RESPONSE,
    CONF_ID,
    CONF_METHOD,
    CONF_ON_ERROR,
    CONF_ON_RESPONSE,
    CONF_TIMEOUT,
    CONF_URL,
    __version__,
)
from esphome.core import CORE, Lambda

CODEOWNERS = ["@your-github-handle"]
DEPENDENCIES = ["network"]
AUTO_LOAD = ["json"]

# ── Private config-key constants ──────────────────────────────────────────────
CONF_FOLLOW_REDIRECTS = "follow_redirects"
CONF_REDIRECT_LIMIT = "redirect_limit"
CONF_VERIFY_SSL = "verify_ssl"
CONF_BUFFER_SIZE_RX = "buffer_size_rx"
CONF_BUFFER_SIZE_TX = "buffer_size_tx"
CONF_TASK_STACK_SIZE = "task_stack_size"
CONF_TASK_PRIORITY = "task_priority"
CONF_REQUEST_HEADERS = "request_headers"
CONF_COLLECT_HEADERS = "collect_headers"
CONF_BODY = "body"
CONF_JSON = "json"
CONF_CA_CERTIFICATE_PATH = "ca_certificate_path"
CONF_USERAGENT = "useragent"
CONF_MAX_RESPONSE_BUFFER_SIZE = "max_response_buffer_size"
CONF_HTTP_REQUEST_ASYNC_ID = "http_request_async_id"

# ── C++ namespace / class declarations ────────────────────────────────────────
http_request_async_ns = cg.esphome_ns.namespace("http_request_async")

HttpRequestAsyncComponent = http_request_async_ns.class_(
    "HttpRequestAsyncComponent", cg.Component
)
HttpContainer = http_request_async_ns.class_("HttpContainer")
HttpRequestAsyncSendAction = http_request_async_ns.class_(
    "HttpRequestAsyncSendAction", automation.Action
)

# ── Hub configuration schema ──────────────────────────────────────────────────
CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(HttpRequestAsyncComponent),
            cv.Optional(
                CONF_TIMEOUT, default="4.5s"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_FOLLOW_REDIRECTS, default=True): cv.boolean,
            cv.Optional(CONF_REDIRECT_LIMIT, default=3): cv.int_range(min=0, max=9),
            cv.Optional(CONF_VERIFY_SSL, default=True): cv.boolean,
            cv.Optional(CONF_BUFFER_SIZE_RX, default=512): cv.uint16_t,
            cv.Optional(CONF_BUFFER_SIZE_TX, default=512): cv.uint16_t,
            cv.Optional(
                CONF_TASK_STACK_SIZE, default=8192
            ): cv.int_range(min=4096, max=65536),
            cv.Optional(CONF_TASK_PRIORITY, default=5): cv.int_range(min=1, max=24),
            cv.Optional(
                CONF_USERAGENT,
                default=f"ESPHome/{__version__} (https://esphome.io)",
            ): cv.string,
            cv.Optional(CONF_CA_CERTIFICATE_PATH): cv.All(
                cv.file_,
                cv.only_on_esp32,
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on_esp32,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_timeout(config[CONF_TIMEOUT]))
    cg.add(var.set_follow_redirects(config[CONF_FOLLOW_REDIRECTS]))
    cg.add(var.set_redirect_limit(config[CONF_REDIRECT_LIMIT]))
    cg.add(var.set_verify_ssl(config[CONF_VERIFY_SSL]))
    cg.add(var.set_buffer_size_rx(config[CONF_BUFFER_SIZE_RX]))
    cg.add(var.set_buffer_size_tx(config[CONF_BUFFER_SIZE_TX]))
    cg.add(var.set_task_stack_size(config[CONF_TASK_STACK_SIZE]))
    cg.add(var.set_task_priority(config[CONF_TASK_PRIORITY]))
    cg.add(var.set_useragent(config[CONF_USERAGENT]))

    # Re-enable the ESP-IDF HTTP client component (excluded by default to reduce
    # compile time when not needed).
    esp32.include_builtin_idf_component("esp_http_client")

    if config[CONF_VERIFY_SSL]:
        if ca_cert_path := config.get(CONF_CA_CERTIFICATE_PATH):
            with open(ca_cert_path, encoding="utf-8") as f:
                cg.add(var.set_ca_certificate(f.read()))
        else:
            # Use the bundled Mozilla CA store (covers ~99% of public CAs).
            # For less common CAs, supply ca_certificate_path or call
            # esp32.require_full_certificate_bundle() from another component.
            esp32.add_idf_sdkconfig_option("CONFIG_MBEDTLS_CERTIFICATE_BUNDLE", True)

    esp32.add_idf_sdkconfig_option(
        "CONFIG_ESP_TLS_INSECURE", not config[CONF_VERIFY_SSL]
    )
    esp32.add_idf_sdkconfig_option(
        "CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY", not config[CONF_VERIFY_SSL]
    )


# ── URL validator ─────────────────────────────────────────────────────────────
def _validate_url(value):
    value = cv.url(value)
    if value.startswith("http://") or value.startswith("https://"):
        return value
    raise cv.Invalid("URL must start with 'http://' or 'https://'")


# ── Shared action schema pieces ───────────────────────────────────────────────
_BODY_EXTENSION = {
    cv.Exclusive(CONF_BODY, "body"): cv.templatable(cv.string),
    cv.Exclusive(CONF_JSON, "body"): cv.Any(
        cv.lambda_,
        cv.All(cv.Schema({cv.string: cv.templatable(cv.string_strict)})),
    ),
}

_ACTION_BASE_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_HTTP_REQUEST_ASYNC_ID): cv.use_id(
            HttpRequestAsyncComponent
        ),
        cv.Required(CONF_URL): cv.templatable(_validate_url),
        cv.Optional(CONF_REQUEST_HEADERS): cv.All(
            cv.Schema({cv.string: cv.templatable(cv.string)})
        ),
        cv.Optional(CONF_COLLECT_HEADERS): cv.ensure_list(cv.string),
        cv.Optional(CONF_CAPTURE_RESPONSE, default=False): cv.boolean,
        cv.Optional(
            CONF_MAX_RESPONSE_BUFFER_SIZE, default="1kB"
        ): cv.validate_bytes,
        cv.Optional(CONF_ON_RESPONSE): automation.validate_automation(single=True),
        cv.Optional(CONF_ON_ERROR): automation.validate_automation(single=True),
    }
)

HTTP_REQUEST_ASYNC_GET_SCHEMA = automation.maybe_conf(
    CONF_URL,
    _ACTION_BASE_SCHEMA.extend(
        {cv.Optional(CONF_METHOD, default="GET"): cv.one_of("GET", upper=True)}
    ),
)

HTTP_REQUEST_ASYNC_POST_SCHEMA = automation.maybe_conf(
    CONF_URL,
    _ACTION_BASE_SCHEMA.extend(
        {
            cv.Optional(CONF_METHOD, default="POST"): cv.one_of("POST", upper=True),
            **_BODY_EXTENSION,
        }
    ),
)

HTTP_REQUEST_ASYNC_SEND_SCHEMA = _ACTION_BASE_SCHEMA.extend(
    {
        cv.Required(CONF_METHOD): cv.one_of(
            "GET", "POST", "PUT", "DELETE", "PATCH", upper=True
        ),
        **_BODY_EXTENSION,
    }
)


# ── Shared action code-generator ──────────────────────────────────────────────
async def _build_action(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_HTTP_REQUEST_ASYNC_ID])
    var = cg.new_Pvariable(action_id, template_arg, parent)

    # URL
    url_t = await cg.templatable(config[CONF_URL], args, cg.std_string)
    cg.add(var.set_url(url_t))

    # Method
    method_t = await cg.templatable(config[CONF_METHOD], args, cg.const_char_ptr)
    cg.add(var.set_method(method_t))

    # capture_response
    capture = config[CONF_CAPTURE_RESPONSE]
    if capture:
        cg.add(var.set_capture_response(True))

    cg.add(var.set_max_response_buffer_size(config[CONF_MAX_RESPONSE_BUFFER_SIZE]))

    # Request headers
    request_headers = config.get(CONF_REQUEST_HEADERS, {})
    if request_headers:
        cg.add(var.init_request_headers(len(request_headers)))
    for key, value in request_headers.items():
        t = await cg.templatable(value, args, cg.const_char_ptr)
        cg.add(var.add_request_header(key, t))

    # Collect (response) headers
    for h in config.get(CONF_COLLECT_HEADERS, []):
        cg.add(var.add_collect_header(h.lower()))

    # Verbatim string body
    if CONF_BODY in config:
        body_t = await cg.templatable(config[CONF_BODY], args, cg.std_string)
        cg.add(var.set_body(body_t))

    # JSON body — lambda form: `json: |-  root["key"] = val;`
    # or dict form:            `json: {key: value}`
    if CONF_JSON in config:
        json_cfg = config[CONF_JSON]
        if isinstance(json_cfg, Lambda):
            # Lambda receives (Ts..., JsonObject root) and returns void.
            args_ = args + [(cg.JsonObject, "root")]
            lambda_ = await cg.process_lambda(
                json_cfg, args_, return_type=cg.void
            )
            cg.add(var.set_json(lambda_))
        else:
            # Static/templatable key→value dict
            cg.add(var.init_json(len(json_cfg)))
            for key, value in json_cfg.items():
                t = await cg.templatable(value, args, cg.std_string)
                cg.add(var.add_json(key, t))

    # on_response trigger
    if response_conf := config.get(CONF_ON_RESPONSE):
        if capture:
            # With body: lambda receives (response, body, Ts...)
            await automation.build_automation(
                var.get_response_trigger(),
                [
                    (cg.std_shared_ptr.template(HttpContainer), "response"),
                    (cg.std_string_ref, "body"),
                    *args,
                ],
                response_conf,
            )
        else:
            # Without body: lambda receives (response, Ts...)
            await automation.build_automation(
                var.get_response_trigger_no_body(),
                [(cg.std_shared_ptr.template(HttpContainer), "response"), *args],
                response_conf,
            )

    # on_error trigger — lambda receives (Ts...)
    if error_conf := config.get(CONF_ON_ERROR):
        await automation.build_automation(var.get_error_trigger(), args, error_conf)

    return var


# ── Action registrations ──────────────────────────────────────────────────────
# synchronous=False: play_next_() is deferred to loop() when the response
# arrives — never called within play_complex() itself.
@automation.register_action(
    "http_request_async.get",
    HttpRequestAsyncSendAction,
    HTTP_REQUEST_ASYNC_GET_SCHEMA,
    synchronous=False,
)
@automation.register_action(
    "http_request_async.post",
    HttpRequestAsyncSendAction,
    HTTP_REQUEST_ASYNC_POST_SCHEMA,
    synchronous=False,
)
@automation.register_action(
    "http_request_async.send",
    HttpRequestAsyncSendAction,
    HTTP_REQUEST_ASYNC_SEND_SCHEMA,
    synchronous=False,
)
async def http_request_async_action_to_code(config, action_id, template_arg, args):
    return await _build_action(config, action_id, template_arg, args)
