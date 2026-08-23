BUILD_DIR ?= build
BUILD_TYPE ?= Release
PREFIX ?= $(HOME)/.local
GENERATOR ?= Ninja

CMAKE ?= cmake
CLANG_TIDY ?= clang-tidy
CLAZY ?= clazy-standalone
QMLLINT ?= qmllint

LINT_SOURCES := $(wildcard src/*.cpp tests/*.cpp)
LINT_CHECKS ?= -*,clang-analyzer-*,bugprone-*,performance-*,misc-*

.PHONY: all configure build clean install check smoke lint qt-lint

all: build

configure:
	$(CMAKE) -S . -B $(BUILD_DIR) -G $(GENERATOR) \
		-DCMAKE_BUILD_TYPE=$(BUILD_TYPE) \
		-DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
		-DCMAKE_INSTALL_PREFIX=$(PREFIX)

build: configure
	$(CMAKE) --build $(BUILD_DIR) --parallel

smoke: build
	QT_QPA_PLATFORM=offscreen $(BUILD_DIR)/omasnap-smoke \
		$(BUILD_DIR)/omasnap-smoke-output

lint: build
	@set -eu; \
	if command -v "$(CLANG_TIDY)" >/dev/null 2>&1; then \
		commands_dir=$$(mktemp -d); \
		trap 'rm -rf "$$commands_dir"' EXIT; \
		cp "$(BUILD_DIR)/compile_commands.json" \
			"$$commands_dir/compile_commands.json"; \
		sed -i 's/ -mno-direct-extern-access//g' \
			"$$commands_dir/compile_commands.json"; \
		printf '%s\n' $(LINT_SOURCES) | xargs -r -P "$$(nproc)" -n 1 \
			"$(CLANG_TIDY)" -p "$$commands_dir" \
			-checks="$(LINT_CHECKS)" -header-filter='.*'; \
	else \
		echo "make check: clang-tidy unavailable; skipping"; \
	fi

qt-lint: build
	@set -eu; \
	if command -v "$(CLAZY)" >/dev/null 2>&1; then \
		for source in $(LINT_SOURCES); do \
			"$(CLAZY)" -p "$(BUILD_DIR)" "$$source"; \
		done; \
	else \
		echo "make check: clazy unavailable; skipping Qt-specific clazy pass"; \
	fi; \
	if command -v "$(QMLLINT)" >/dev/null 2>&1 && test -d qml; then \
		"$(QMLLINT)" qml; \
	else \
		echo "make check: no QML sources or qmllint unavailable; skipping"; \
	fi

check: smoke lint qt-lint

clean:
	@if test -d "$(BUILD_DIR)"; then \
		$(CMAKE) --build "$(BUILD_DIR)" --target clean; \
	fi

install: build
	$(CMAKE) --install $(BUILD_DIR)
