HOST_CC ?= cc
PYTHON ?= python3
BUILD_DIR ?= build
HOST_BIN := $(BUILD_DIR)/host/ps5localsend-host
TEST_CONFIG_BIN := $(BUILD_DIR)/host/test-config
TEST_ROUTER_BIN := $(BUILD_DIR)/host/test-router
TEST_AUTH_BIN := $(BUILD_DIR)/host/test-auth
TEST_UPLOAD_BIN := $(BUILD_DIR)/host/test-upload
TEST_FAST_UPLOAD_BIN := $(BUILD_DIR)/host/test-fast-upload
PS5_BIN := ps5localsend.elf
RELEASE_ARCHIVE := $(BUILD_DIR)/ps5localsend-1.0.tar.gz

MHD_VERSION := 1.0.10
MHD_ARCHIVE := third_party/libmicrohttpd/libmicrohttpd-$(MHD_VERSION).tar.gz
MHD_SHA256 := 04bfe8ef75db7d629a33de767599765cecadc56274a39822d5d081030d577685
MHD_SOURCE := $(BUILD_DIR)/deps/libmicrohttpd-$(MHD_VERSION)
MHD_EXTRACT_STAMP := $(MHD_SOURCE)/.extracted
HOST_MHD_BUILD := $(BUILD_DIR)/host/libmicrohttpd
HOST_MHD_LIB := $(HOST_MHD_BUILD)/src/microhttpd/.libs/libmicrohttpd.a
PS5_MHD_BUILD := $(BUILD_DIR)/ps5/libmicrohttpd
PS5_MHD_LIB := $(PS5_MHD_BUILD)/src/microhttpd/.libs/libmicrohttpd.a
MHD_INCLUDE := $(MHD_SOURCE)/src/include
GENERATED_ASSETS := $(BUILD_DIR)/generated/assets_data.c

COMMON_WARNINGS := -Wall -Wextra -Wpedantic -Werror
COMMON_CPPFLAGS := -Isrc -I$(BUILD_DIR)/generated -I$(MHD_INCLUDE)
COMMON_SOURCES := src/main.c src/config.c src/server.c src/fast_upload_server.c src/router.c src/auth.c src/upload.c src/safe_path.c src/sha256.c $(GENERATED_ASSETS)
COMMON_HEADERS := src/assets.h src/auth.h src/config.h src/fast_upload_server.h src/platform.h src/router.h \
	src/safe_path.h src/server.h src/sha256.h src/upload.h src/version.h
HOST_SOURCES := $(COMMON_SOURCES) src/platform_host.c
PS5_SOURCES := $(COMMON_SOURCES) src/platform_ps5.c
MHD_CONFIGURE_COMMON := --disable-shared --enable-static --disable-https \
	--disable-postprocessor --disable-messages --disable-doc --disable-examples \
	--disable-curl --disable-tools --disable-dauth --disable-bauth --disable-cookie

.PHONY: all host test ps5 release github-release clean verify-mhd

all: host

host: $(HOST_BIN)

verify-mhd: $(MHD_ARCHIVE)
	@printf '%s  %s\n' '$(MHD_SHA256)' '$(MHD_ARCHIVE)' | shasum -a 256 -c -

$(MHD_EXTRACT_STAMP): $(MHD_ARCHIVE)
	@mkdir -p $(BUILD_DIR)/deps
	@printf '%s  %s\n' '$(MHD_SHA256)' '$(MHD_ARCHIVE)' | shasum -a 256 -c -
	@rm -rf $(MHD_SOURCE)
	@tar -xzf $(MHD_ARCHIVE) -C $(BUILD_DIR)/deps
	@touch $@

$(HOST_MHD_BUILD)/Makefile: $(MHD_EXTRACT_STAMP)
	@mkdir -p $(HOST_MHD_BUILD)
	@cd $(HOST_MHD_BUILD) && mhd_cv_works_func_getsockname=yes \
		$(abspath $(MHD_SOURCE))/configure \
		CC='$(HOST_CC)' $(MHD_CONFIGURE_COMMON)

$(HOST_MHD_LIB): $(HOST_MHD_BUILD)/Makefile
	$(MAKE) -C $(HOST_MHD_BUILD)

$(GENERATED_ASSETS): tools/embed_assets.py web/index.html web/app.css web/app.js web/ps5-console.png src/assets.h
	$(PYTHON) tools/embed_assets.py --web-dir web --output $@

$(HOST_BIN): $(HOST_SOURCES) $(COMMON_HEADERS) $(HOST_MHD_LIB)
	@mkdir -p $(@D)
	$(HOST_CC) -std=c11 $(COMMON_WARNINGS) $(COMMON_CPPFLAGS) -DPS5LOCALSEND_HOST -O2 -pthread \
		-o $@ $(HOST_SOURCES) $(HOST_MHD_LIB)

$(TEST_CONFIG_BIN): tests/test_config.c src/config.c src/config.h
	@mkdir -p $(@D)
	$(HOST_CC) -std=c11 $(COMMON_WARNINGS) -Isrc -O2 -o $@ tests/test_config.c src/config.c

$(TEST_ROUTER_BIN): tests/test_router.c src/router.c src/auth.c src/upload.c src/safe_path.c src/sha256.c src/config.c src/platform_host.c $(GENERATED_ASSETS) src/router.h src/auth.h src/upload.h src/safe_path.h src/sha256.h src/assets.h src/config.h src/platform.h src/version.h
	@mkdir -p $(@D)
	$(HOST_CC) -std=c11 $(COMMON_WARNINGS) -Isrc -I$(BUILD_DIR)/generated -DPS5LOCALSEND_HOST -O2 -pthread \
		-o $@ tests/test_router.c src/router.c src/auth.c src/upload.c src/safe_path.c src/sha256.c src/config.c src/platform_host.c $(GENERATED_ASSETS)

$(TEST_AUTH_BIN): tests/test_auth.c src/auth.c src/auth.h src/platform_host.c src/platform.h
	@mkdir -p $(@D)
	$(HOST_CC) -std=c11 $(COMMON_WARNINGS) -Isrc -O2 -pthread \
		-o $@ tests/test_auth.c src/auth.c src/platform_host.c

$(TEST_UPLOAD_BIN): tests/test_upload.c src/upload.c src/safe_path.c src/sha256.c src/config.c src/platform_host.c src/upload.h src/safe_path.h src/sha256.h src/config.h src/platform.h
	@mkdir -p $(@D)
	$(HOST_CC) -std=c11 $(COMMON_WARNINGS) -Isrc -DPS5LOCALSEND_HOST -O2 -pthread \
		-o $@ tests/test_upload.c src/upload.c src/safe_path.c src/sha256.c src/config.c src/platform_host.c

$(TEST_FAST_UPLOAD_BIN): tests/test_fast_upload_server.c src/fast_upload_server.c src/fast_upload_server.h src/router.h src/upload.h
	@mkdir -p $(@D)
	$(HOST_CC) -std=c11 $(COMMON_WARNINGS) -Isrc -DPS5LOCALSEND_HOST -O2 -pthread \
		-o $@ tests/test_fast_upload_server.c src/fast_upload_server.c

test: $(HOST_BIN) $(TEST_CONFIG_BIN) $(TEST_ROUTER_BIN) $(TEST_AUTH_BIN) $(TEST_UPLOAD_BIN) $(TEST_FAST_UPLOAD_BIN)
	$(TEST_CONFIG_BIN)
	$(TEST_ROUTER_BIN)
	$(TEST_AUTH_BIN)
	$(TEST_UPLOAD_BIN)
	$(TEST_FAST_UPLOAD_BIN)
	$(PYTHON) tests/test_http_server.py $(HOST_BIN)

ps5:
	@if [ -z "$(PS5_PAYLOAD_SDK)" ]; then \
		echo "error: PS5_PAYLOAD_SDK is not set (supported SDK: ps5-payload-dev/sdk v0.43)" >&2; \
		exit 2; \
	fi
	@if [ ! -f "$(PS5_PAYLOAD_SDK)/toolchain/prospero.mk" ]; then \
		echo "error: $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk not found" >&2; \
		exit 2; \
	fi
	$(MAKE) PS5_CROSS_BUILD=1 $(PS5_BIN)

ifeq ($(PS5_CROSS_BUILD),1)
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

$(PS5_MHD_BUILD)/Makefile: $(MHD_EXTRACT_STAMP)
	@mkdir -p $(PS5_MHD_BUILD)
	@cd $(PS5_MHD_BUILD) && $(abspath $(MHD_SOURCE))/configure \
		--host=x86_64-unknown-freebsd CC='$(CC)' AR='$(AR)' RANLIB='$(RANLIB)' \
		STRIP='$(STRIP)' $(MHD_CONFIGURE_COMMON)

$(PS5_MHD_LIB): $(PS5_MHD_BUILD)/Makefile
	$(MAKE) -C $(PS5_MHD_BUILD)

$(PS5_BIN): $(PS5_SOURCES) $(COMMON_HEADERS) $(PS5_MHD_LIB)
	$(CC) -std=c11 $(COMMON_WARNINGS) $(COMMON_CPPFLAGS) -O3 -pthread \
		-o $@ $(PS5_SOURCES) $(PS5_MHD_LIB) -lkernel_sys
endif

release: ps5
	$(PYTHON) tools/package_release.py --root . --output $(RELEASE_ARCHIVE)
	@echo "created $(RELEASE_ARCHIVE)"

github-release: release
	$(PYTHON) tools/prepare_github_project.py --root . --output github-release/ps5localsend
	@echo "created github-release/ps5localsend"

clean:
	rm -rf $(BUILD_DIR) $(PS5_BIN)
