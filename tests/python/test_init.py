"""
Python unit tests for http_request_async/__init__.py

These follow the same pattern as ESPHome's own unit tests in
tests/unit_tests/components/ — they test the Python config-generation layer
(schema validation, defaults, codegen) without building firmware.

Run:
    pip install esphome pytest
    pytest tests/python/
"""

import asyncio
import sys
from pathlib import Path
from unittest.mock import AsyncMock, MagicMock, call, patch

import pytest

# ── Path setup ────────────────────────────────────────────────────────────────
# Add the repo root and ESPHome source to sys.path so imports work.
repo_root = Path(__file__).parent.parent.parent
sys.path.insert(0, str(repo_root))


# ── Helpers ───────────────────────────────────────────────────────────────────

def _component():
    """Return the http_request_async module (cached after first import)."""
    import components.http_request_async as component  # noqa: PLC0415
    return component


def _get_schema():
    """Import the component and return its CONFIG_SCHEMA."""
    return _component().CONFIG_SCHEMA


def _get_action_schemas():
    c = _component()
    return {
        "get": c.HTTP_REQUEST_ASYNC_GET_SCHEMA,
        "post": c.HTTP_REQUEST_ASYNC_POST_SCHEMA,
        "send": c.HTTP_REQUEST_ASYNC_SEND_SCHEMA,
    }


# ── Fixtures ──────────────────────────────────────────────────────────────────

@pytest.fixture(autouse=True)
def mock_esphome_core():
    """
    Patch the parts of esphome.core that require a real CORE object.
    This mirrors ESPHome's own conftest.py approach of patching rather than
    running a full build.

    ESPHome 2026.x uses CORE.target_platform (a Platform StrEnum, e.g. 'esp32')
    for platform checks instead of the older CORE.is_esp32 bool properties.
    Both are set here for compatibility.
    """
    with patch("esphome.core.CORE") as mock_core:
        # New API (2026.x): target_platform is a Platform StrEnum value.
        mock_core.target_platform = "esp32"
        # Legacy helpers — kept for any code still using the old booleans.
        mock_core.is_esp32 = True
        mock_core.is_esp8266 = False
        mock_core.is_rp2040 = False
        mock_core.is_host = False
        mock_core.using_arduino = False
        mock_core.using_esp_idf = True
        yield mock_core


# ═══════════════════════════════════════════════════════════════════════════════
# Hub schema tests
# ═══════════════════════════════════════════════════════════════════════════════

class TestHubSchema:
    def test_minimal_config_is_valid(self):
        """An empty hub config should be valid (all keys optional)."""
        import esphome.config_validation as cv
        schema = _get_schema()
        # Should not raise
        result = schema({})
        assert result is not None

    def test_default_timeout(self):
        import esphome.config_validation as cv
        schema = _get_schema()
        result = schema({})
        # ESPHome 2026.x returns TimePeriodMilliseconds; use total_milliseconds.
        assert result["timeout"].total_milliseconds == 4500

    def test_default_verify_ssl(self):
        schema = _get_schema()
        result = schema({})
        assert result["verify_ssl"] is True

    def test_default_redirect_limit(self):
        schema = _get_schema()
        result = schema({})
        assert result["redirect_limit"] == 3

    def test_default_task_stack_size(self):
        schema = _get_schema()
        result = schema({})
        assert result["task_stack_size"] == 8192

    def test_default_task_priority(self):
        schema = _get_schema()
        result = schema({})
        assert result["task_priority"] == 5

    def test_custom_timeout(self):
        schema = _get_schema()
        result = schema({"timeout": "10s"})
        assert result["timeout"].total_milliseconds == 10000

    def test_invalid_task_priority_too_high(self):
        import esphome.config_validation as cv
        schema = _get_schema()
        with pytest.raises(cv.Invalid):
            schema({"task_priority": 25})  # max is 24

    def test_invalid_task_priority_zero(self):
        import esphome.config_validation as cv
        schema = _get_schema()
        with pytest.raises(cv.Invalid):
            schema({"task_priority": 0})  # min is 1

    def test_invalid_redirect_limit_too_high(self):
        import esphome.config_validation as cv
        schema = _get_schema()
        with pytest.raises(cv.Invalid):
            schema({"redirect_limit": 10})  # max is 9

    def test_custom_useragent(self):
        schema = _get_schema()
        result = schema({"useragent": "MyDevice/2.0"})
        assert result["useragent"] == "MyDevice/2.0"

    def test_buffer_sizes_accepted(self):
        schema = _get_schema()
        result = schema({"buffer_size_rx": 1024, "buffer_size_tx": 512})
        assert result["buffer_size_rx"] == 1024
        assert result["buffer_size_tx"] == 512

    def test_default_task_count(self):
        schema = _get_schema()
        result = schema({})
        assert result["task_count"] == 1

    def test_custom_task_count(self):
        schema = _get_schema()
        result = schema({"task_count": 3})
        assert result["task_count"] == 3

    def test_invalid_task_count_zero(self):
        import esphome.config_validation as cv
        schema = _get_schema()
        with pytest.raises(cv.Invalid):
            schema({"task_count": 0})  # min is 1

    def test_invalid_task_count_too_high(self):
        import esphome.config_validation as cv
        schema = _get_schema()
        with pytest.raises(cv.Invalid):
            schema({"task_count": 9})  # max is 8


# ═══════════════════════════════════════════════════════════════════════════════
# Action schema tests
# ═══════════════════════════════════════════════════════════════════════════════

class TestGetActionSchema:
    def test_url_is_required(self):
        import esphome.config_validation as cv
        schemas = _get_action_schemas()
        with pytest.raises((cv.Invalid, KeyError)):
            schemas["get"]({})  # missing url

    def test_shorthand_url(self):
        """http_request_async.get: http://example.com (string shorthand)."""
        schemas = _get_action_schemas()
        result = schemas["get"]("http://example.com")
        assert result["url"] == "http://example.com"

    def test_method_defaults_to_get(self):
        schemas = _get_action_schemas()
        result = schemas["get"]({"url": "http://example.com"})
        assert result["method"] == "GET"

    def test_capture_response_default_false(self):
        schemas = _get_action_schemas()
        result = schemas["get"]({"url": "http://example.com"})
        assert result["capture_response"] is False

    def test_capture_response_true(self):
        schemas = _get_action_schemas()
        result = schemas["get"]({"url": "http://example.com", "capture_response": True})
        assert result["capture_response"] is True

    def test_request_headers_accepted(self):
        schemas = _get_action_schemas()
        result = schemas["get"]({
            "url": "http://example.com",
            "request_headers": {"Content-Type": "application/json"},
        })
        assert result["request_headers"] == {"Content-Type": "application/json"}

    def test_collect_headers_accepted(self):
        schemas = _get_action_schemas()
        result = schemas["get"]({
            "url": "http://example.com",
            "collect_headers": ["Content-Type", "X-Request-Id"],
        })
        assert "Content-Type" in result["collect_headers"]

    def test_invalid_url_scheme(self):
        import esphome.config_validation as cv
        schemas = _get_action_schemas()
        with pytest.raises(cv.Invalid):
            schemas["get"]({"url": "ftp://example.com"})


class TestPostActionSchema:
    def test_method_defaults_to_post(self):
        schemas = _get_action_schemas()
        result = schemas["post"]({"url": "http://example.com"})
        assert result["method"] == "POST"

    def test_json_lambda_accepted(self):
        from esphome.core import Lambda
        schemas = _get_action_schemas()
        result = schemas["post"]({
            "url": "http://example.com",
            "json": Lambda('root["key"] = "val";'),
        })
        assert isinstance(result["json"], Lambda)

    def test_json_dict_accepted(self):
        schemas = _get_action_schemas()
        result = schemas["post"]({
            "url": "http://example.com",
            "json": {"key": "value", "other": "thing"},
        })
        assert result["json"]["key"] == "value"

    def test_body_and_json_are_exclusive(self):
        import esphome.config_validation as cv
        schemas = _get_action_schemas()
        with pytest.raises(cv.Invalid):
            schemas["post"]({
                "url": "http://example.com",
                "body": "raw body",
                "json": {"key": "value"},
            })

    def test_body_accepted(self):
        schemas = _get_action_schemas()
        result = schemas["post"]({"url": "http://example.com", "body": "raw"})
        assert result["body"] == "raw"


class TestSendActionSchema:
    def test_method_is_required(self):
        import esphome.config_validation as cv
        schemas = _get_action_schemas()
        with pytest.raises((cv.Invalid, KeyError)):
            schemas["send"]({"url": "http://example.com"})  # missing method

    def test_valid_methods(self):
        schemas = _get_action_schemas()
        for method in ("GET", "POST", "PUT", "DELETE", "PATCH"):
            result = schemas["send"]({"url": "http://example.com", "method": method})
            assert result["method"] == method

    def test_invalid_method(self):
        import esphome.config_validation as cv
        schemas = _get_action_schemas()
        with pytest.raises(cv.Invalid):
            schemas["send"]({"url": "http://example.com", "method": "HEAD"})

    def test_method_case_insensitive(self):
        schemas = _get_action_schemas()
        result = schemas["send"]({"url": "http://example.com", "method": "get"})
        assert result["method"] == "GET"


# ═══════════════════════════════════════════════════════════════════════════════
# SSL / TLS codegen tests
# ═══════════════════════════════════════════════════════════════════════════════
#
# The C++ SSL configuration lives inside execute_request_(), which is IDF-only
# and not reachable by Tier-2 native tests. What IS testable is the Python
# to_code() function, which controls which sdkconfig options are emitted.
# Those options are what actually disable or enable TLS verification at the
# mbedTLS layer — so these tests directly cover the original bug (CA bundle
# being attached while verification was supposed to be off).

class TestSslCodegen:
    """Verify that to_code() emits the correct sdkconfig options for TLS."""

    def _run_to_code(self, config_overrides):
        """
        Execute to_code() against a real schema-validated config with all ESPHome
        codegen calls mocked out. Returns the esp32 mock so callers can inspect
        which add_idf_sdkconfig_option() calls were made.
        """
        with patch("components.http_request_async.cg") as mock_cg, \
             patch("components.http_request_async.esp32") as mock_esp32:
            mock_var = MagicMock()
            mock_cg.new_Pvariable.return_value = mock_var
            mock_cg.register_component = AsyncMock()

            schema = _get_schema()
            config = schema(config_overrides)
            asyncio.run(_component().to_code(config))

            return mock_esp32

    def test_verify_ssl_true_attaches_certificate_bundle(self):
        """verify_ssl: true must enable the bundled Mozilla CA store."""
        mock_esp32 = self._run_to_code({"verify_ssl": True})
        mock_esp32.add_idf_sdkconfig_option.assert_any_call(
            "CONFIG_MBEDTLS_CERTIFICATE_BUNDLE", True
        )
        mock_esp32.add_idf_sdkconfig_option.assert_any_call(
            "CONFIG_ESP_TLS_INSECURE", False
        )
        mock_esp32.add_idf_sdkconfig_option.assert_any_call(
            "CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY", False
        )

    def test_verify_ssl_false_does_not_attach_bundle(self):
        """
        verify_ssl: false must NOT set CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=True.
        Attaching the bundle while also disabling verification is self-defeating:
        the bundle re-enables cert chain validation at the mbedTLS layer even
        when the user expects it to be fully off.
        """
        mock_esp32 = self._run_to_code({"verify_ssl": False})

        bundle_enabled = [
            c for c in mock_esp32.add_idf_sdkconfig_option.call_args_list
            if c == call("CONFIG_MBEDTLS_CERTIFICATE_BUNDLE", True)
        ]
        assert not bundle_enabled, (
            "CONFIG_MBEDTLS_CERTIFICATE_BUNDLE must not be True when verify_ssl=false; "
            f"found calls: {mock_esp32.add_idf_sdkconfig_option.call_args_list}"
        )

    def test_verify_ssl_false_sets_insecure_flags(self):
        """verify_ssl: false must set both ESP-IDF insecure options."""
        mock_esp32 = self._run_to_code({"verify_ssl": False})
        mock_esp32.add_idf_sdkconfig_option.assert_any_call(
            "CONFIG_ESP_TLS_INSECURE", True
        )
        mock_esp32.add_idf_sdkconfig_option.assert_any_call(
            "CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY", True
        )

    def test_verify_ssl_true_does_not_set_insecure_flags(self):
        """verify_ssl: true must not leave the insecure flags enabled."""
        mock_esp32 = self._run_to_code({"verify_ssl": True})
        mock_esp32.add_idf_sdkconfig_option.assert_any_call(
            "CONFIG_ESP_TLS_INSECURE", False
        )
        mock_esp32.add_idf_sdkconfig_option.assert_any_call(
            "CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY", False
        )
