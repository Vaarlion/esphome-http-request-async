cmake_minimum_required(VERSION 3.16)
project(http_request_async_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Google Test (fetched automatically) ──────────────────────────────────────
include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# ── Test executable ───────────────────────────────────────────────────────────
#
# We compile the real http_request_async_idf.cpp rather than component_stubs.cpp.
# All esp_http_client_* calls are intercepted by idf_http_mock.cpp, which provides
# controllable stub implementations.  This lets redirect-following tests exercise
# the actual IDF-level logic in execute_request_() without any hardware.
add_executable(test_runner
  main.cpp
  idf_http_mock.cpp
  ../../components/http_request_async/http_request_async_idf.cpp
)

target_include_directories(test_runner PRIVATE
  # mocks/ must come BEFORE the component root so that stub headers shadow
  # any #include of real FreeRTOS / ESPHome / ESP-IDF headers.
  ${CMAKE_CURRENT_SOURCE_DIR}/mocks
  # Component root (so #include "../../components/..." resolves)
  ${CMAKE_CURRENT_SOURCE_DIR}/../..
)

# Force-include the compat header before everything else so all translation
# units see the type definitions before the component header's #includes run.
target_compile_options(test_runner PRIVATE
  -include ${CMAKE_CURRENT_SOURCE_DIR}/mocks/esphome_compat.h
  -Wall
  -Wextra
  -Wno-unused-parameter   # compat stubs have unused params intentionally
)

target_compile_definitions(test_runner PRIVATE
  USE_ESP32=1
  NATIVE_TEST=1
)

target_link_libraries(test_runner PRIVATE
  GTest::gtest
  GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(test_runner)
