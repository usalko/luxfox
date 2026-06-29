ifeq ($(MEDIA_PARAM), )
    MEDIA_PARAM := ../Makefile.param
    include $(MEDIA_PARAM)
endif

export LC_ALL=C
SHELL := /bin/bash
.DEFAULT_GOAL := all

CURRENT_DIR := $(shell pwd)

CC := $(RK_MEDIA_CROSS)-gcc
AR := $(RK_MEDIA_CROSS)-ar

PKG_NAME := radiod
PKG_OUT := $(CURRENT_DIR)/out
PKG_BUILD := $(CURRENT_DIR)/build
PKG_SCRIPT_DIR := $(CURRENT_DIR)/scripts
PKG_CONFIG_DIR := $(CURRENT_DIR)/defaults

HOST_CC ?= gcc
ULAMA_ROOT := $(CURRENT_DIR)/../ulama
UNOW_ROOT := $(CURRENT_DIR)/../unow
UNOW_PCAP_SYSROOT ?= $(CURRENT_DIR)/../../sysdrv/source/buildroot/buildroot-2023.02.6/output/host/arm-buildroot-linux-uclibcgnueabihf/sysroot

RADIOD_SRCS := $(wildcard $(CURRENT_DIR)/src/*.c)
ULAMA_LIB_SRCS := $(wildcard $(ULAMA_ROOT)/src/core/*.c) $(wildcard $(ULAMA_ROOT)/src/common/*.c)
UNOW_SRCS := $(wildcard $(UNOW_ROOT)/src/*.c)
TARGET_APP_SRC := $(CURRENT_DIR)/tools/radiod.c
TARGET_BIN := radiod

HOST_TEST_SRCS := $(wildcard $(CURRENT_DIR)/tests/test_*.c)
HOST_TEST_BINS := $(patsubst $(CURRENT_DIR)/tests/%.c,$(PKG_BUILD)/tests/%,$(HOST_TEST_SRCS))

INC_FLAGS := -I$(CURRENT_DIR)/include -I$(ULAMA_ROOT)/include -I$(ULAMA_ROOT)/src -I$(RK_MEDIA_OUTPUT)/include -I$(UNOW_ROOT)/include -I$(UNOW_ROOT)/src
ifneq ($(wildcard $(UNOW_PCAP_SYSROOT)/usr/include/pcap/pcap.h),)
INC_FLAGS += -isystem $(UNOW_PCAP_SYSROOT)/usr/include
endif
CFLAGS += $(RK_MEDIA_CROSS_CFLAGS) -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L -DULAMA_WITH_UNOW=1 -g -Wall -Wextra -std=c11 $(INC_FLAGS) -fPIC

HOST_INC_FLAGS := -I$(CURRENT_DIR)/include -I$(ULAMA_ROOT)/include -I$(ULAMA_ROOT)/src -I$(UNOW_ROOT)/include -I$(UNOW_ROOT)/src
HOST_CFLAGS := -D_DEFAULT_SOURCE -DULAMA_WITH_UNOW=0 -g -Wall -Wextra -std=c11 $(HOST_INC_FLAGS)
APP_LDFLAGS += -lpcap -lpthread -lrt
ifneq ($(wildcard $(UNOW_PCAP_SYSROOT)/usr/lib/libpcap.so),)
APP_LDFLAGS += -L$(UNOW_PCAP_SYSROOT)/usr/lib -Wl,-rpath-link,$(UNOW_PCAP_SYSROOT)/usr/lib
endif
HOST_LDFLAGS += -lpthread

HOST_PCAP_CFLAGS := $(shell pkg-config --cflags libpcap 2>/dev/null)
HOST_PCAP_LIBS := $(shell pkg-config --libs libpcap 2>/dev/null)
ifeq ($(HOST_PCAP_LIBS),)
ifneq ($(wildcard /usr/include/pcap/pcap.h),)
HOST_PCAP_CFLAGS := -I/usr/include
HOST_PCAP_LIBS := -lpcap
endif
endif
ifeq ($(HOST_PCAP_LIBS),)
HAVE_HOST_PCAP := 0
else
HAVE_HOST_PCAP := 1
endif

HOST_UNOW_CFLAGS := -D_DEFAULT_SOURCE -DULAMA_WITH_UNOW=1 -g -Wall -Wextra -std=c11 $(HOST_INC_FLAGS) $(HOST_PCAP_CFLAGS)
HOST_UNOW_LDFLAGS := $(HOST_PCAP_LIBS) -lpthread

.PHONY: all clean distclean host host-unow test

all: radiod-build
	@$(call MAROC_COPY_PKG_TO_MEDIA_OUTPUT, $(RK_MEDIA_OUTPUT), $(PKG_OUT))
	@echo "build $(PKG_NAME) done";

radiod-build: $(TARGET_BIN)
	@mkdir -p $(PKG_OUT)/bin $(PKG_OUT)/etc $(PKG_OUT)/root/etc/init.d
	@cp -f $(TARGET_BIN) $(PKG_OUT)/bin/
	@if [ -f $(PKG_SCRIPT_DIR)/S96radiod ]; then cp -f $(PKG_SCRIPT_DIR)/S96radiod $(PKG_OUT)/root/etc/init.d/; fi
	@if [ -f $(PKG_CONFIG_DIR)/radiod.conf ]; then cp -f $(PKG_CONFIG_DIR)/radiod.conf $(PKG_OUT)/etc/; fi

ULAMA_VERSION_HDR := $(ULAMA_ROOT)/include/ulama/ulama_version.h

$(TARGET_BIN): $(TARGET_APP_SRC) $(RADIOD_SRCS) $(ULAMA_LIB_SRCS) $(UNOW_SRCS) $(ULAMA_VERSION_HDR)
	$(CC) $(CFLAGS) $< $(RADIOD_SRCS) $(ULAMA_LIB_SRCS) $(UNOW_SRCS) -o $@ $(APP_LDFLAGS)

host: $(HOST_TEST_BINS)

ifeq ($(HAVE_HOST_PCAP),1)
host-unow: $(PKG_BUILD)/host-unow-tools/radiod
else
host-unow:
	@echo "host-unow requires libpcap development files on the host." >&2
	@echo "Install them first, for example: sudo apt install libpcap-dev" >&2
	@exit 1
endif

test: host
	@for test_bin in $(HOST_TEST_BINS); do \
		echo "running $$test_bin"; \
		"$$test_bin" || exit $$?; \
	done

$(PKG_BUILD)/tests/%: $(CURRENT_DIR)/tests/%.c $(RADIOD_SRCS) $(ULAMA_LIB_SRCS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_CFLAGS) $< $(RADIOD_SRCS) $(ULAMA_LIB_SRCS) -o $@ $(HOST_LDFLAGS)

$(PKG_BUILD)/host-unow-tools/radiod: $(TARGET_APP_SRC) $(RADIOD_SRCS) $(ULAMA_LIB_SRCS) $(UNOW_SRCS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_UNOW_CFLAGS) $< $(RADIOD_SRCS) $(ULAMA_LIB_SRCS) $(UNOW_SRCS) -o $@ $(HOST_UNOW_LDFLAGS)

clean:
	@rm -rf $(PKG_BUILD) $(TARGET_BIN)
	@rm -rf $(PKG_OUT)

distclean: clean
