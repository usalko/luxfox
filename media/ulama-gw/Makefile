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

PKG_NAME := ulama-gw
PKG_OUT := $(CURRENT_DIR)/out
PKG_BUILD := $(CURRENT_DIR)/build
PKG_SCRIPT_DIR := $(CURRENT_DIR)/scripts
PKG_CONFIG_DIR := $(CURRENT_DIR)/defaults

HOST_CC ?= gcc
ULAMA_ROOT := $(CURRENT_DIR)/../ulama
UNOW_ROOT := $(CURRENT_DIR)/../unow
UNOW_PCAP_SYSROOT ?= $(CURRENT_DIR)/../../sysdrv/source/buildroot/buildroot-2023.02.6/output/host/arm-buildroot-linux-uclibcgnueabihf/sysroot

GW_SRCS := $(wildcard $(CURRENT_DIR)/src/*.c)
GW_OBJS := $(patsubst $(CURRENT_DIR)/src/%.c,$(PKG_BUILD)/src/%.o,$(GW_SRCS))
ULAMA_LIB_SRCS := $(wildcard $(ULAMA_ROOT)/src/core/*.c) $(wildcard $(ULAMA_ROOT)/src/common/*.c)
UNOW_SRCS := $(wildcard $(UNOW_ROOT)/src/*.c)
TARGET_APP_SRC := $(CURRENT_DIR)/tools/ulama_gw.c
TARGET_BIN := ulama-gw

HOST_TEST_SRCS := $(wildcard $(CURRENT_DIR)/tests/*.c)
HOST_TEST_BINS := $(patsubst $(CURRENT_DIR)/tests/%.c,$(PKG_BUILD)/tests/%,$(HOST_TEST_SRCS))
HOST_TOOL_BINS := $(PKG_BUILD)/host-tools/ulama_gw

TARGET_LIB_A := libulama_gw.a

INC_FLAGS := -I$(CURRENT_DIR)/include -I$(ULAMA_ROOT)/include -I$(ULAMA_ROOT)/src -I$(RK_MEDIA_OUTPUT)/include -I$(UNOW_ROOT)/include -I$(UNOW_ROOT)/src
ifneq ($(wildcard $(UNOW_PCAP_SYSROOT)/usr/include/pcap/pcap.h),)
INC_FLAGS += -isystem $(UNOW_PCAP_SYSROOT)/usr/include
endif
CFLAGS += $(RK_MEDIA_CROSS_CFLAGS) -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L -DULAMA_WITH_UNOW=1 -g -Wall -Wextra -std=c11 $(INC_FLAGS) -fPIC

HOST_INC_FLAGS := -I$(CURRENT_DIR)/include -I$(ULAMA_ROOT)/include -I$(ULAMA_ROOT)/src -I$(UNOW_ROOT)/include -I$(UNOW_ROOT)/src
HOST_CFLAGS := -D_DEFAULT_SOURCE -DULAMA_WITH_UNOW=0 -g -Wall -Wextra -std=c11 $(HOST_INC_FLAGS)
APP_LDFLAGS += -lpcap -lpthread -lrt
HOST_LDFLAGS += -lpthread
ifneq ($(wildcard $(UNOW_PCAP_SYSROOT)/usr/lib/libpcap.so),)
APP_LDFLAGS += -L$(UNOW_PCAP_SYSROOT)/usr/lib -Wl,-rpath-link,$(UNOW_PCAP_SYSROOT)/usr/lib
endif

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

all: gw-build
	@echo "build $(PKG_NAME) done (host-only, not staged to device output)";

# ulama-gw is a host-only tool; its artifacts stay under $(PKG_OUT) for local
# inspection but are intentionally NOT copied into RK_MEDIA_OUTPUT, so it
# never ends up in the OEM partition or rootfs shipped to the device.
gw-build: $(TARGET_LIB_A) $(TARGET_BIN)
	@mkdir -p $(PKG_OUT)/bin $(PKG_OUT)/etc $(PKG_OUT)/include $(PKG_OUT)/lib
	@cp -f $(TARGET_BIN) $(PKG_OUT)/bin/
	@cp -f $(TARGET_LIB_A) $(PKG_OUT)/lib/
	@cp -rfa $(CURRENT_DIR)/include/* $(PKG_OUT)/include/
	@if [ -f $(PKG_SCRIPT_DIR)/S98ulama-gw ]; then cp -f $(PKG_SCRIPT_DIR)/S98ulama-gw $(PKG_OUT)/etc/; fi
	@if [ -f $(PKG_CONFIG_DIR)/ulama-gw.conf ]; then cp -f $(PKG_CONFIG_DIR)/ulama-gw.conf $(PKG_OUT)/etc/; fi

$(PKG_BUILD)/src/%.o: $(CURRENT_DIR)/src/%.c
	@mkdir -p $(dir $@)
	$(CC) -c $< $(CFLAGS) -o $@

$(TARGET_LIB_A): $(GW_OBJS)
	$(AR) -rcs $@ $(GW_OBJS)

$(TARGET_BIN): $(TARGET_APP_SRC) $(GW_SRCS) $(ULAMA_LIB_SRCS) $(UNOW_SRCS)
	$(CC) $(CFLAGS) $< $(GW_SRCS) $(ULAMA_LIB_SRCS) $(UNOW_SRCS) -o $@ $(APP_LDFLAGS)

host: $(HOST_TEST_BINS) $(HOST_TOOL_BINS)

ifeq ($(HAVE_HOST_PCAP),1)
host-unow: $(PKG_BUILD)/host-unow-tools/ulama_gw
else
host-unow:
	@echo "host-unow requires libpcap development files on the host." >&2
	@echo "Install them first, for example: sudo apt install libpcap-dev iw tcpdump" >&2
	@exit 1
endif

test: host
	@for test_bin in $(HOST_TEST_BINS); do \
		echo "running $$test_bin"; \
		"$$test_bin" || exit $$?; \
	done

$(PKG_BUILD)/tests/%: $(CURRENT_DIR)/tests/%.c $(GW_SRCS) $(ULAMA_LIB_SRCS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_CFLAGS) $< $(GW_SRCS) $(ULAMA_LIB_SRCS) -o $@ $(HOST_LDFLAGS)

$(PKG_BUILD)/host-tools/ulama_gw: $(TARGET_APP_SRC) $(GW_SRCS) $(ULAMA_LIB_SRCS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_CFLAGS) $< $(GW_SRCS) $(ULAMA_LIB_SRCS) -o $@ $(HOST_LDFLAGS)

$(PKG_BUILD)/host-unow-tools/ulama_gw: $(TARGET_APP_SRC) $(GW_SRCS) $(ULAMA_LIB_SRCS) $(UNOW_SRCS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_UNOW_CFLAGS) $< $(GW_SRCS) $(ULAMA_LIB_SRCS) $(UNOW_SRCS) -o $@ $(HOST_UNOW_LDFLAGS)

clean:
	@rm -rf $(PKG_BUILD) $(TARGET_LIB_A) $(TARGET_BIN)
	@rm -rf $(PKG_OUT)

distclean: clean
