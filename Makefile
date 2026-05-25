# ── http_request_async – development Makefile ─────────────────────────────────
#
# Single entry point for all development operations. Agents and humans use the
# same targets so there's no ambiguity about how to run things.
#
# Usage:
#   make              → show this help
#   make init         → create .venv and install Python dev dependencies
#   make test         → run all tests (C++ + Python)
#   make test-native  → C++ unit tests only (fastest, no ESPHome needed)
#   make test-python  → Python schema tests only  (runs make init if needed)
#   make compile      → esphome compile (full codegen + C++ compile, no flash)
#   make flash        → compile + OTA flash + tail logs  (needs secrets.yaml)
#   make compile-device → compile device_test.yaml only  (needs secrets.yaml)
#   make logs         → tail device logs without flashing
#   make clean        → remove build artefacts (keeps .venv)
#   make clean-venv   → remove .venv  (forces a fresh pip install on next init)
#   make rebuild      → clean + test

.DEFAULT_GOAL := help

# ── Paths ──────────────────────────────────────────────────────────────────────
NATIVE_BUILD   := tests/native/build
NATIVE_RUNNER  := $(NATIVE_BUILD)/test_runner
NATIVE_CMAKE   := tests/native
ESPHOME_CONFIG := tests/esphome/test_config.yaml
PYTHON_TESTS   := tests/python/

# Device test (Tier 4 — hardware, requires secrets.yaml)
DEVICE_CONFIG  := tests/esphome/device_test.yaml
DEVICE_SECRETS := tests/esphome/secrets.yaml
# Optional: override target device  e.g.  make flash DEVICE=192.168.1.42
DEVICE      ?=
DEVICE_FLAG := $(if $(DEVICE),--device $(DEVICE),)

# Auto-detect available parallelism
NPROC := $(shell nproc 2>/dev/null || sysctl -n hw.logicalcpu 2>/dev/null || echo 4)

# ── Virtual environment ────────────────────────────────────────────────────────
# .venv is created by `make init`.  All Python targets depend on the sentinel
# file so they auto-initialise on first use.  The venv is intentionally NOT
# removed by `make clean` — use `make clean-venv` for that.

VENV          := .venv
VENV_SENTINEL := $(VENV)/.installed
PYTEST        := $(VENV)/bin/pytest
PYTHON        := $(VENV)/bin/python3
# The venv's esphome binary is used for `make compile` when the venv exists.
ESPHOME       := $(shell test -f $(VENV)/bin/esphome && echo $(VENV)/bin/esphome || echo esphome)

# ── Colours (suppressed when not a terminal) ───────────────────────────────────
ifeq ($(TERM),)
  BOLD :=
  GREEN :=
  RED :=
  YELLOW :=
  RESET :=
else
  BOLD   := $(shell tput bold 2>/dev/null)
  GREEN  := $(shell tput setaf 2 2>/dev/null)
  RED    := $(shell tput setaf 1 2>/dev/null)
  YELLOW := $(shell tput setaf 3 2>/dev/null)
  RESET  := $(shell tput sgr0 2>/dev/null)
endif

# ── Virtualenv setup ──────────────────────────────────────────────────────────

.PHONY: init
init: $(VENV_SENTINEL)  ## Create .venv and install Python dev dependencies (idempotent)

# Re-runs if requirements_test.txt changes.
$(VENV_SENTINEL): requirements_test.txt
	@echo "$(BOLD)Setting up Python virtual environment…$(RESET)"
	python3 -m venv $(VENV)
	$(VENV)/bin/pip install --quiet --upgrade pip
	$(VENV)/bin/pip install --quiet -r requirements_test.txt
	@touch $(VENV_SENTINEL)
	@echo "$(GREEN)Virtual environment ready at $(VENV)/$(RESET)"

# ── Top-level targets ─────────────────────────────────────────────────────────

.PHONY: test
test: test-native test-python  ## Run all tests (C++ unit tests + Python schema tests)
	@echo "$(GREEN)$(BOLD)All tests passed.$(RESET)"

.PHONY: test-native
test-native: $(NATIVE_RUNNER)  ## Build and run C++ unit tests (no ESPHome/hardware needed)
	@echo "$(BOLD)── C++ unit tests ──$(RESET)"
	@$(NATIVE_RUNNER) && echo "$(GREEN)C++ tests: PASSED$(RESET)" || \
	  (echo "$(RED)C++ tests: FAILED$(RESET)"; exit 1)

.PHONY: test-python
test-python: $(VENV_SENTINEL)  ## Run Python schema tests (auto-inits venv if needed)
	@echo "$(BOLD)── Python tests ──$(RESET)"
	@$(PYTEST) $(PYTHON_TESTS) -v && echo "$(GREEN)Python tests: PASSED$(RESET)" || \
	  (echo "$(RED)Python tests: FAILED$(RESET)"; exit 1)

.PHONY: compile
compile: $(VENV_SENTINEL)  ## ESPHome compile check — codegen + C++ compile, no flash
	@echo "$(BOLD)── ESPHome compilation check ──$(RESET)"
	@$(ESPHOME) compile $(ESPHOME_CONFIG) && \
	  echo "$(GREEN)Compilation: PASSED$(RESET)" || \
	  (echo "$(RED)Compilation: FAILED$(RESET)"; exit 1)

# ── Device targets (Tier 4 — hardware) ───────────────────────────────────────
#
# Require tests/esphome/secrets.yaml (gitignored).
# Copy tests/esphome/secrets.yaml.example → tests/esphome/secrets.yaml and fill in.

.PHONY: _check-secrets
_check-secrets:
	@test -f $(DEVICE_SECRETS) || ( \
	  echo "$(RED)Missing: $(DEVICE_SECRETS)$(RESET)"; \
	  echo "  cp tests/esphome/secrets.yaml.example tests/esphome/secrets.yaml"; \
	  echo "  # then fill in wifi_ssid, api_key, ota_password, test_server"; \
	  exit 1 )

.PHONY: flash
flash: $(VENV_SENTINEL) _check-secrets  ## Compile + OTA flash + tail logs  [DEVICE=ip optional]
	@$(ESPHOME) run $(DEVICE_CONFIG) $(DEVICE_FLAG)

.PHONY: compile-device
compile-device: $(VENV_SENTINEL) _check-secrets  ## Compile device_test.yaml only (local component, no flash)
	@echo "$(BOLD)── Device test compilation ──$(RESET)"
	@$(ESPHOME) compile $(DEVICE_CONFIG) && \
	  echo "$(GREEN)Device compilation: PASSED$(RESET)" || \
	  (echo "$(RED)Device compilation: FAILED$(RESET)"; exit 1)

.PHONY: logs
logs: $(VENV_SENTINEL)  ## Tail device logs without flashing  [DEVICE=ip optional]
	@$(ESPHOME) logs $(DEVICE_CONFIG) $(DEVICE_FLAG)

# ── Build ─────────────────────────────────────────────────────────────────────

.PHONY: build-native
build-native: $(NATIVE_RUNNER)  ## Build C++ tests without running them

# Configure if the build directory doesn't contain a cache yet
$(NATIVE_BUILD)/CMakeCache.txt:
	@echo "$(BOLD)Configuring native tests…$(RESET)"
	@cmake -B $(NATIVE_BUILD) $(NATIVE_CMAKE) \
	    -DCMAKE_BUILD_TYPE=Debug 2>&1 | grep -v "^--" || true

# Build: depends on configuration and all source files
$(NATIVE_RUNNER): $(NATIVE_BUILD)/CMakeCache.txt \
                  $(NATIVE_CMAKE)/main.cpp \
                  $(NATIVE_CMAKE)/component_stubs.cpp \
                  components/http_request_async/http_request_async.h
	@echo "$(BOLD)Building native tests ($(NPROC) jobs)…$(RESET)"
	@cmake --build $(NATIVE_BUILD) -j$(NPROC) 2>&1 | grep -E "error:|warning:|Linking|Built" || true

# ── Maintenance ───────────────────────────────────────────────────────────────

.PHONY: clean
clean:  ## Remove build artefacts and Python cache (keeps .venv)
	@echo "Cleaning build artefacts…"
	@rm -rf $(NATIVE_BUILD)
	@find . -name "__pycache__" -type d -not -path "./.venv/*" -exec rm -rf {} + 2>/dev/null; true
	@find . -name "*.pyc" -not -path "./.venv/*" -delete 2>/dev/null; true
	@echo "Done.  (Run 'make clean-venv' to also remove the virtual environment.)"

.PHONY: upgrade
upgrade: $(VENV_SENTINEL)  ## Upgrade all Python deps to latest (run after each ESPHome monthly release)
	@echo "$(BOLD)Upgrading Python dependencies to latest…$(RESET)"
	@$(VENV)/bin/pip install --quiet --upgrade -r requirements_test.txt
	@touch $(VENV_SENTINEL)
	@echo "$(GREEN)Upgraded.$(RESET)  Now run: make test && make compile"

.PHONY: clean-venv
clean-venv:  ## Remove .venv (forces fresh install on next make init / test-python)
	@echo "Removing virtual environment…"
	@rm -rf $(VENV)
	@echo "Done.  Run 'make init' to recreate."

.PHONY: rebuild
rebuild: clean test  ## Clean then run all tests

# ── Developer helpers ─────────────────────────────────────────────────────────

.PHONY: check-deps
check-deps:  ## Check that required tools are installed
	@echo "Checking dependencies…"
	@command -v cmake    >/dev/null 2>&1 && echo "  cmake:        $(GREEN)OK$(RESET)"     || echo "  cmake:        $(RED)MISSING$(RESET)"
	@command -v g++      >/dev/null 2>&1 && echo "  g++:          $(GREEN)OK$(RESET)"     || echo "  g++:          $(RED)MISSING$(RESET)"
	@command -v python3  >/dev/null 2>&1 && echo "  python3:      $(GREEN)OK$(RESET)"     || echo "  python3:      $(RED)MISSING$(RESET)"
	@test -f $(VENV_SENTINEL)            && echo "  .venv:        $(GREEN)ready$(RESET)"  || echo "  .venv:        $(YELLOW)not initialised — run: make init$(RESET)"
	@test -f $(VENV)/bin/pytest          && echo "  pytest:       $(GREEN)OK (venv)$(RESET)" || \
	  (command -v pytest >/dev/null 2>&1 && echo "  pytest:       $(YELLOW)OK (system — prefer: make init)$(RESET)" || echo "  pytest:       $(RED)MISSING — run: make init$(RESET)")
	@test -f $(VENV)/bin/esphome         && echo "  esphome:      $(GREEN)OK (venv)$(RESET)" || \
	  (command -v esphome>/dev/null 2>&1 && echo "  esphome:      $(YELLOW)OK (system — prefer: make init)$(RESET)" || echo "  esphome:      $(YELLOW)not found — run: make init$(RESET)")

# ── Help ──────────────────────────────────────────────────────────────────────

.PHONY: help
help:  ## Show this help
	@echo "$(BOLD)http_request_async — development targets$(RESET)"
	@echo ""
	@grep -E '^[a-z][a-z_-]*:.*##' $(MAKEFILE_LIST) | \
	  awk 'BEGIN{FS=":.*##"} {printf "  $(BOLD)%-16s$(RESET) %s\n", $$1, $$2}'
	@echo ""
	@echo "$(BOLD)Quickstart:$(RESET)"
	@echo "  make init          create .venv + install all Python deps (run once)"
	@echo "  make upgrade       pull latest ESPHome after a monthly release"
	@echo "  make check-deps    verify all tools are installed"
	@echo "  make test          run C++ + Python tests"
	@echo "  make compile       full ESPHome compilation check (~3 min)"
	@echo ""
	@echo "$(BOLD)Hardware testing (Tier 4):$(RESET)"
	@echo "  cp tests/esphome/secrets.yaml.example tests/esphome/secrets.yaml"
	@echo "  # fill in wifi_ssid, api_key, ota_password, test_server"
	@echo "  make flash         compile + OTA flash + tail logs"
	@echo "  make logs          tail logs without flashing"
