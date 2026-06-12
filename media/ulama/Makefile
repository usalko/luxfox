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

PKG_NAME := ulama
PKG_OUT := $(CURRENT_DIR)/out
PKG_BUILD := $(CURRENT_DIR)/build

HOST_CC ?= gcc
UNOW_ROOT := $(CURRENT_DIR)/../unow
UNOW_PCAP_SYSROOT ?= $(CURRENT_DIR)/../../sysdrv/source/buildroot/buildroot-2023.02.6/output/host/arm-buildroot-linux-uclibcgnueabihf/sysroot

LIB_SRCS := $(wildcard $(CURRENT_DIR)/src/core/*.c) $(wildcard $(CURRENT_DIR)/src/common/*.c)
LIB_OBJS := $(patsubst $(CURRENT_DIR)/src/%.c,$(PKG_BUILD)/src/%.o,$(LIB_SRCS))
UNOW_SRCS := $(wildcard $(UNOW_ROOT)/src/*.c)
TARGET_APP_SRC := $(CURRENT_DIR)/tools/ulamad.c
TARGET_BIN := ulamad

HOST_TEST_SRCS := $(wildcard $(CURRENT_DIR)/tests/*.c)
HOST_TEST_BINS := $(patsubst $(CURRENT_DIR)/tests/%.c,$(PKG_BUILD)/tests/%,$(HOST_TEST_SRCS))
HOST_TOOL_SRCS := $(wildcard $(CURRENT_DIR)/tools/*.c)
HOST_TOOL_BINS := $(patsubst $(CURRENT_DIR)/tools/%.c,$(PKG_BUILD)/host-tools/%,$(HOST_TOOL_SRCS))

TARGET_LIB_A := libulama.a

INC_FLAGS := -I$(CURRENT_DIR)/include -I$(CURRENT_DIR)/src -I$(RK_MEDIA_OUTPUT)/include -I$(UNOW_ROOT)/include -I$(UNOW_ROOT)/src
ifneq ($(wildcard $(UNOW_PCAP_SYSROOT)/usr/include/pcap/pcap.h),)
INC_FLAGS += -I$(UNOW_PCAP_SYSROOT)/usr/include
endif
CFLAGS += $(RK_MEDIA_CROSS_CFLAGS) -D_DEFAULT_SOURCE -DULAMA_WITH_UNOW=1 -g -Wall -Wextra -std=c11 $(INC_FLAGS) -fPIC

HOST_INC_FLAGS := -I$(CURRENT_DIR)/include -I$(CURRENT_DIR)/src -I$(UNOW_ROOT)/include -I$(UNOW_ROOT)/src
HOST_CFLAGS := -D_DEFAULT_SOURCE -DULAMA_WITH_UNOW=0 -g -Wall -Wextra -std=c11 $(HOST_INC_FLAGS)
APP_LDFLAGS += -lpcap -lpthread
HOST_LDFLAGS += -lpthread
ifneq ($(wildcard $(UNOW_PCAP_SYSROOT)/usr/lib/libpcap.so),)
APP_LDFLAGS += -L$(UNOW_PCAP_SYSROOT)/usr/lib -Wl,-rpath-link,$(UNOW_PCAP_SYSROOT)/usr/lib
endif

.PHONY: all clean distclean host test

all: ulama-build
	@$(call MAROC_COPY_PKG_TO_MEDIA_OUTPUT, $(RK_MEDIA_OUTPUT), $(PKG_OUT))
	@echo "build $(PKG_NAME) done";

ulama-build: $(TARGET_LIB_A) $(TARGET_BIN)
	@mkdir -p $(PKG_OUT)/bin $(PKG_OUT)/include $(PKG_OUT)/lib
	@cp -f $(TARGET_BIN) $(PKG_OUT)/bin/
	@cp -f $(TARGET_LIB_A) $(PKG_OUT)/lib/
	@cp -rfa $(CURRENT_DIR)/include/* $(PKG_OUT)/include/

$(PKG_BUILD)/src/%.o: $(CURRENT_DIR)/src/%.c
	@mkdir -p $(dir $@)
	$(CC) -c $< $(CFLAGS) -o $@

$(TARGET_LIB_A): $(LIB_OBJS)
	$(AR) -rcs $@ $(LIB_OBJS)

$(TARGET_BIN): $(TARGET_APP_SRC) $(LIB_SRCS) $(UNOW_SRCS)
	$(CC) $(CFLAGS) $< $(LIB_SRCS) $(UNOW_SRCS) -o $@ $(APP_LDFLAGS)

host: $(HOST_TEST_BINS) $(HOST_TOOL_BINS)

test: host
	@for test_bin in $(HOST_TEST_BINS); do \
		echo "running $$test_bin"; \
		"$$test_bin" || exit $$?; \
	done

$(PKG_BUILD)/tests/%: $(CURRENT_DIR)/tests/%.c $(LIB_SRCS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_CFLAGS) $< $(LIB_SRCS) -o $@ $(HOST_LDFLAGS)

$(PKG_BUILD)/host-tools/%: $(CURRENT_DIR)/tools/%.c $(LIB_SRCS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_CFLAGS) $< $(LIB_SRCS) -o $@ $(HOST_LDFLAGS)

clean:
	@rm -rf $(PKG_BUILD) $(TARGET_LIB_A) $(TARGET_BIN)
	@rm -rf $(PKG_OUT)

distclean: clean