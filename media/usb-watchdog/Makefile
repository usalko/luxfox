ifeq ($(MEDIA_PARAM), )
    MEDIA_PARAM := ../Makefile.param
    include $(MEDIA_PARAM)
endif

export LC_ALL=C
SHELL := /bin/bash

CURRENT_DIR := $(shell pwd)
PKG_NAME := usb-watchdog
PKG_OUT := $(CURRENT_DIR)/out

.PHONY: all clean distclean

all:
	@mkdir -p $(PKG_OUT)/bin $(PKG_OUT)/etc/init.d
	@cp -f $(CURRENT_DIR)/scripts/usb-watchdog.sh $(PKG_OUT)/bin/usb-watchdog.sh
	@cp -f $(CURRENT_DIR)/scripts/S90usb-watchdog $(PKG_OUT)/etc/init.d/S90usb-watchdog
	@chmod +x $(PKG_OUT)/bin/usb-watchdog.sh $(PKG_OUT)/etc/init.d/S90usb-watchdog
	@$(call MAROC_COPY_PKG_TO_MEDIA_OUTPUT, $(RK_MEDIA_OUTPUT), $(PKG_OUT))
	@echo "build $(PKG_NAME) done"

clean:
	@rm -rf $(PKG_OUT)

distclean: clean
