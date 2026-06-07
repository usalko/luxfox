ifeq ($(MEDIA_PARAM), )
    MEDIA_PARAM := ../Makefile.param
    include $(MEDIA_PARAM)
endif

export LC_ALL=C
SHELL := /bin/bash

CURRENT_DIR := $(shell pwd)

CC := $(RK_MEDIA_CROSS)-gcc
AR := $(RK_MEDIA_CROSS)-ar

PKG_NAME := unow
PKG_OUT := $(CURRENT_DIR)/out
PKG_BUILD := $(CURRENT_DIR)/build

UNOW_PCAP_SYSROOT ?= $(CURRENT_DIR)/../../sysdrv/source/buildroot/buildroot-2023.02.6/output/host/arm-buildroot-linux-uclibcgnueabihf/sysroot

LIB_SRCS := $(wildcard $(CURRENT_DIR)/src/*.c)
LIB_OBJS := $(patsubst $(CURRENT_DIR)/src/%.c,$(PKG_BUILD)/src/%.o,$(LIB_SRCS))
TOOL_SRCS := $(wildcard $(CURRENT_DIR)/tools/*.c)
TOOL_OBJS := $(patsubst $(CURRENT_DIR)/tools/%.c,$(PKG_BUILD)/tools/%.o,$(TOOL_SRCS))

TARGET_LIB_SO := libunow.so
TARGET_LIB_A := libunow.a
TARGET_BIN := unow_diag

INC_FLAGS := -I$(CURRENT_DIR)/include -I$(CURRENT_DIR)/src -I$(RK_MEDIA_OUTPUT)/include
ifneq ($(wildcard $(UNOW_PCAP_SYSROOT)/usr/include/pcap/pcap.h),)
INC_FLAGS += -I$(UNOW_PCAP_SYSROOT)/usr/include
endif

CFLAGS += $(RK_MEDIA_CROSS_CFLAGS) -D_DEFAULT_SOURCE -g -Wall -Wextra -std=c11 $(INC_FLAGS) -fPIC
LDFLAGS += -L$(RK_MEDIA_OUTPUT)/lib
ifneq ($(wildcard $(UNOW_PCAP_SYSROOT)/usr/lib/libpcap.so),)
LDFLAGS += -L$(UNOW_PCAP_SYSROOT)/usr/lib -Wl,-rpath-link,$(UNOW_PCAP_SYSROOT)/usr/lib
endif
LDLIBS += -lpcap -lpthread

.PHONY: all clean distclean

all: unow-build
	@$(call MAROC_COPY_PKG_TO_MEDIA_OUTPUT, $(RK_MEDIA_OUTPUT), $(PKG_OUT))
	@echo "build $(PKG_NAME) done";

unow-build: $(TARGET_LIB_SO) $(TARGET_LIB_A) $(TARGET_BIN)
	@mkdir -p $(PKG_OUT)/bin $(PKG_OUT)/include $(PKG_OUT)/lib
	@cp -f $(TARGET_LIB_SO) $(TARGET_LIB_A) $(PKG_OUT)/lib/
	@cp -f $(TARGET_BIN) $(PKG_OUT)/bin/
	@cp -rfa $(CURRENT_DIR)/include/* $(PKG_OUT)/include/
	@cp -f $(CURRENT_DIR)/scripts/* $(PKG_OUT)/bin/

$(PKG_BUILD)/src/%.o: $(CURRENT_DIR)/src/%.c
	@mkdir -p $(dir $@)
	$(CC) -c $< $(CFLAGS) -o $@

$(PKG_BUILD)/tools/%.o: $(CURRENT_DIR)/tools/%.c
	@mkdir -p $(dir $@)
	$(CC) -c $< $(CFLAGS) -o $@

$(TARGET_LIB_SO): $(LIB_OBJS)
	$(CC) $(CFLAGS) -shared $(LIB_OBJS) -o $@ $(LDFLAGS) $(LDLIBS)

$(TARGET_LIB_A): $(LIB_OBJS)
	$(AR) -rcs $@ $(LIB_OBJS)

$(TARGET_BIN): $(TOOL_OBJS) $(TARGET_LIB_SO)
	$(CC) $(CFLAGS) $(TOOL_OBJS) -L. -lunow -o $@ $(LDFLAGS) $(LDLIBS)

clean:
	@rm -rf $(PKG_BUILD) $(TARGET_LIB_SO) $(TARGET_LIB_A) $(TARGET_BIN)
	@rm -rf $(PKG_OUT)

distclean: clean