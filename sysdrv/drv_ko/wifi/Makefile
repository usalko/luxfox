ifeq ($(SYSDRV_PARAM), )
	SYSDRV_PARAM:=../../Makefile.param
	include $(SYSDRV_PARAM)
endif

ifneq ($(SYSDRV_KERNEL_OBJS_OUTPUT_DIR),)
WIFI_BUILD_KERNEL_OBJ_DIR=$(SYSDRV_KERNEL_OBJS_OUTPUT_DIR)
else
WIFI_BUILD_KERNEL_OBJ_DIR=$(KERNEL_DIR)
endif

ifeq ($(ENABLE_WIFI),y)

WIFI_USB = $(shell grep -o "CONFIG_USB=y" $(WIFI_BUILD_KERNEL_OBJ_DIR)/.config)
WIFI_SDIO = $(shell grep -o "CONFIG_MMC=y" $(WIFI_BUILD_KERNEL_OBJ_DIR)/.config)
WIFI_RFKILL = $(shell grep -o "CONFIG_RFKILL=y" $(WIFI_BUILD_KERNEL_OBJ_DIR)/.config)
export WIFI_RFKILL WIFI_BUILD_KERNEL_OBJ_DIR

ifneq ($(WIFI_USB),)
	PKG_TARGET := build-usb
	PKG_CLEAN_TARGET += build-usb-clean
endif

ifneq ($(WIFI_SDIO),)
	PKG_TARGET += build-sdio
	PKG_CLEAN_TARGET += build-sdio-clean
endif

endif

ifeq ($(M_OUT_DIR),)
# TODO
$(error Please build on upper directory)
endif

all: $(PKG_TARGET)
	@echo "build wifi done"
	@echo "build wifi $(WIFI_RFKILL)"
ifeq ($(ENABLE_WIFI),y)
	@cp insmod_wifi.sh $(M_OUT_DIR);
endif

ifneq ($(RK_ENABLE_WIFI_CHIP),)

build-usb:
ifneq ($(findstring RTL8188FTV,$(RK_ENABLE_WIFI_CHIP)),)
	@make -C rtl8188ftv/
endif

ifneq ($(findstring RTL8192EU,$(RK_ENABLE_WIFI_CHIP)),)
	@make -C rtl8188ftv/ clean
	@make -C rtl8188ftv/ \
		CONFIG_RTL8188F=n CONFIG_RTL8192E=y \
		CONFIG_USB_HCI=y CONFIG_SDIO_HCI=n
endif

ifneq ($(findstring SSV6X5X,$(RK_ENABLE_WIFI_CHIP)),)
	@make -C ssv6x5x/
endif

ifneq ($(findstring SSV6115,$(RK_ENABLE_WIFI_CHIP)),)
	@make -C ssv6115/
endif

ifneq ($(findstring AIC8800DW_USB,$(RK_ENABLE_WIFI_CHIP)),)
	@make -C aic8800_usb/
endif

ifneq ($(findstring AIC8800MC,$(RK_ENABLE_WIFI_CHIP)),)
	@make -C aic8800_netdrv/
endif

build-sdio:
ifneq ($(findstring RTL8189FS,$(RK_ENABLE_WIFI_CHIP)),)
	@make -C rtl8189fs/
endif

ifneq ($(findstring HI3861L,$(RK_ENABLE_WIFI_CHIP)),)
	@make -C hichannel/
endif

ifneq ($(findstring ATBM603X,$(RK_ENABLE_WIFI_CHIP)),)
	@make -C atbm/
endif

ifneq ($(findstring ATBM6441,$(RK_ENABLE_WIFI_CHIP)),)
	@make -C atbm6441/
endif

ifneq ($(findstring AIC8800DC,$(RK_ENABLE_WIFI_CHIP)),)
	@make -C aic8800dc/
endif

ifneq ($(WIFI_RFKILL),)
ifneq ($(findstring AP6XXX,$(RK_ENABLE_WIFI_CHIP)),)
	@make -C bcmdhd/
endif
endif

else

build-usb:
	@make -C rtl8188ftv/
	@make -C ssv6x5x/
	@make -C ssv6115/
	@make -C aic8800_usb/

build-sdio:
	@make -C rtl8189fs/
	@make -C hichannel/
	@make -C atbm/
	@make -C atbm6441/
	@make -C aic8800dc/
ifneq ($(WIFI_RFKILL),)
	@make -C bcmdhd/
endif
	@make -C aic8800_netdrv/

endif

build-sdio-clean:
	@make -C rtl8189fs/ clean
	@make -C bcmdhd/ clean
	@make -C hichannel/ clean
	@make -C atbm/ clean
	@make -C atbm6441/ clean
	@make -C aic8800_netdrv/ clean

build-usb-clean:
	@make -C rtl8188ftv clean
	@make -C ssv6x5x clean
	@make -C ssv6115 clean
	@make -C aic8800_usb clean

clean: $(PKG_CLEAN_TARGET)
