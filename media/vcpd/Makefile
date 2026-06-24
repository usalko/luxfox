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

PKG_NAME := vcpd
PKG_OUT := $(CURRENT_DIR)/out
PKG_BUILD := $(CURRENT_DIR)/build
PKG_SCRIPT_DIR := $(CURRENT_DIR)/scripts
PKG_CONFIG_DIR := $(CURRENT_DIR)/defaults

HOST_CC ?= gcc
ULAMA_ROOT := $(CURRENT_DIR)/../ulama
UNOW_ROOT := $(CURRENT_DIR)/../unow
UNOW_PCAP_SYSROOT ?= $(CURRENT_DIR)/../../sysdrv/source/buildroot/buildroot-2023.02.6/output/host/arm-buildroot-linux-uclibcgnueabihf/sysroot

VCPD_SRCS := $(CURRENT_DIR)/src/lts_encoder.c $(CURRENT_DIR)/src/uvcp.c

# MPP hardware encoder (default for target ARM build)
# Falls back to ffmpeg if MPP libs not found
MPP_SAMPLE_DIR := $(RK_MEDIA_OUTPUT)/../../samples/example
MPP_LIBS_AVAIL := $(wildcard $(RK_MEDIA_OUTPUT)/lib/librockchip_mpp.so)
ifneq ($(MPP_LIBS_AVAIL),)
    VCPD_VIDEO_SRCS := $(CURRENT_DIR)/src/video_mpp.c
    CFLAGS += -DVCPD_WITH_MPP=1 -I$(MPP_SAMPLE_DIR)/include
    APP_LDFLAGS += -L$(RK_MEDIA_OUTPUT)/lib -lrockit_full -lrockchip_mpp -lrga -lrt
else
    VCPD_VIDEO_SRCS := $(CURRENT_DIR)/src/video_ffmpeg.c
endif
ULAMA_LIB_SRCS := $(wildcard $(ULAMA_ROOT)/src/core/*.c) $(wildcard $(ULAMA_ROOT)/src/common/*.c)
UNOW_SRCS := $(wildcard $(UNOW_ROOT)/src/*.c)
TARGET_APP_SRC := $(CURRENT_DIR)/tools/vcpd.c
TARGET_BIN := vcpd

HOST_TEST_SRCS := $(wildcard $(CURRENT_DIR)/tests/test_*.c)
HOST_TEST_BINS := $(patsubst $(CURRENT_DIR)/tests/%.c,$(PKG_BUILD)/tests/%,$(HOST_TEST_SRCS))

INC_FLAGS := -I$(CURRENT_DIR)/include -I$(ULAMA_ROOT)/include -I$(ULAMA_ROOT)/src -I$(RK_MEDIA_OUTPUT)/include -I$(UNOW_ROOT)/include -I$(UNOW_ROOT)/src
ifneq ($(wildcard $(UNOW_PCAP_SYSROOT)/usr/include/pcap/pcap.h),)
INC_FLAGS += -isystem $(UNOW_PCAP_SYSROOT)/usr/include
endif
CFLAGS += $(RK_MEDIA_CROSS_CFLAGS) -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L -DULAMA_WITH_UNOW=1 -g -Wall -Wextra -std=c11 $(INC_FLAGS) -fPIC

HOST_INC_FLAGS := -I$(CURRENT_DIR)/include -I$(ULAMA_ROOT)/include -I$(ULAMA_ROOT)/src -I$(UNOW_ROOT)/include -I$(UNOW_ROOT)/src
VCPD_BUILD_ID := $(shell git -C $(CURRENT_DIR)/.. describe --always --dirty 2>/dev/null || date +%Y%m%d-%H%M%S)
CFLAGS += -DVCPD_BUILD_ID='"$(VCPD_BUILD_ID)"'
HOST_CFLAGS := -D_DEFAULT_SOURCE -DULAMA_WITH_UNOW=0 -DVCPD_BUILD_ID='"$(VCPD_BUILD_ID)"' -g -Wall -Wextra -std=c11 $(HOST_INC_FLAGS)
APP_LDFLAGS += -lpcap -lpthread -lrt
ifneq ($(wildcard $(UNOW_PCAP_SYSROOT)/usr/lib/libpcap.so),)
APP_LDFLAGS += -L$(UNOW_PCAP_SYSROOT)/usr/lib -Wl,-rpath-link,$(UNOW_PCAP_SYSROOT)/usr/lib
endif
HOST_LDFLAGS += -lpthread

.PHONY: all clean distclean host test

all: vcpd-build
	@$(call MAROC_COPY_PKG_TO_MEDIA_OUTPUT, $(RK_MEDIA_OUTPUT), $(PKG_OUT))
	@echo "build $(PKG_NAME) done";

vcpd-build: $(TARGET_BIN)
	@mkdir -p $(PKG_OUT)/bin $(PKG_OUT)/etc $(PKG_OUT)/etc/init.d
	@cp -f $(TARGET_BIN) $(PKG_OUT)/bin/
	@if [ -f $(PKG_SCRIPT_DIR)/S97vcpd ]; then cp -f $(PKG_SCRIPT_DIR)/S97vcpd $(PKG_OUT)/etc/init.d/; fi
	@if [ -f $(PKG_CONFIG_DIR)/vcpd.conf ]; then cp -f $(PKG_CONFIG_DIR)/vcpd.conf $(PKG_OUT)/etc/; fi

$(TARGET_BIN): $(TARGET_APP_SRC) $(VCPD_SRCS) $(VCPD_VIDEO_SRCS) $(ULAMA_LIB_SRCS) $(UNOW_SRCS)
	$(CC) $(CFLAGS) $< $(VCPD_SRCS) $(VCPD_VIDEO_SRCS) $(ULAMA_LIB_SRCS) $(UNOW_SRCS) -o $@ $(APP_LDFLAGS)

host: $(HOST_TEST_BINS)

test: host
	@for test_bin in $(HOST_TEST_BINS); do \
		echo "running $$test_bin"; \
		"$$test_bin" || exit $$?; \
	done

$(PKG_BUILD)/tests/%: $(CURRENT_DIR)/tests/%.c $(VCPD_SRCS) $(ULAMA_LIB_SRCS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_CFLAGS) $< $(VCPD_SRCS) $(ULAMA_LIB_SRCS) -o $@ $(HOST_LDFLAGS)

clean:
	@rm -rf $(PKG_BUILD) $(TARGET_BIN)
	@rm -rf $(PKG_OUT)

distclean: clean
