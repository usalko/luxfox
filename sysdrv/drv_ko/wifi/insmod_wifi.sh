#!/bin/sh
cmd=$(realpath $0)
_DIR=$(dirname $cmd)
cd $_DIR

export PATH=$PATH:/oem/usr/ko/

WIFI_DIAG_LOG=/tmp/wifi_insmod.log

resolve_module_path() {
	mod="$1"
	if [ -f "$mod" ]; then
		echo "$mod"
		return 0
	fi

	found=$(find /lib/modules -name "$mod" 2>/dev/null | head -n 1)
	if [ -n "$found" ]; then
		echo "$found"
		return 0
	fi

	return 1
}

log_diag() {
	msg="$1"
	echo "[insmod_wifi] $msg"
	echo "[insmod_wifi] $msg" >> "$WIFI_DIAG_LOG"
	echo "insmod_wifi: $msg" > /dev/kmsg 2>/dev/null
}

insmod_if_exists() {
	mod="$1"
	modpath=$(resolve_module_path "$mod")
	if [ -n "$modpath" ]; then
		log_diag "insmod $modpath"
		insmod "$modpath"
		return $?
	fi
	log_diag "missing module $mod"
	return 1
}

#for fastboot
#insmod_wifi.ko ${RK_ENABLE_WIFI_CHIP} ${RK_ENABLE_FASTBOOT}
if [ "${1}"x = "y"x ]; then
	insmod dw_mmc.ko
	insmod dw_mmc-pltfm.ko
	insmod dw_mmc-rockchip.ko
	case "$2" in
	ATBM6441)
		insmod cfg80211.ko
		insmod atbm6041_wifi_sdio.ko
		;;
	HI3861L)
		# cat /sys/bus/sdio/devices/*/uevent | grep "0296:5347"
		insmod /oem/usr/ko/hichannel.ko hi_rk_irq_gpio=40
		;;
	*)
		exit 1
		;;
	esac
	rkwifi_server start &
	exit 0
fi

#AIC8800DW
cat /sys/bus/sdio/devices/*/uevent | grep "8800"
if [ $? -eq 0 ]; then
	insmod cfg80211.ko
	insmod aic_load_fw.ko aic_fw_path=/oem/usr/ko/
	insmod aic8800_fdrv.ko
	insmod bcmdhd.ko
fi

#AP6XXX
cat /sys/bus/sdio/devices/*/uevent | grep -i "02d0"
if [ $? -eq 0 ]; then
	insmod cfg80211.ko
	insmod bcmdhd.ko
fi

#rtl8723bs
cat /sys/bus/sdio/devices/*/uevent | grep "024C:B723"
if [ $? -eq 0 ]; then
	insmod libarc4.ko
	insmod cfg80211.ko
	insmod mac80211.ko
	insmod r8723bs.ko
fi

#rtl8723ds
cat /sys/bus/sdio/devices/*/uevent | grep "024C:D723"
if [ $? -eq 0 ]; then
	insmod cfg80211.ko
	insmod 8723ds.ko
fi

#rtl8189fs
cat /sys/bus/sdio/devices/*/uevent | grep "024C:F179"
if [ $? -eq 0 ]; then
	insmod cfg80211.ko
	insmod 8189fs.ko
fi

#rtl18188fu
cat /sys/bus/usb/devices/*/uevent | grep "bda\/f179"
if [ $? -eq 0 ]; then
	log_diag "Detected USB Realtek 0bda:f179"
	insmod cfg80211.ko
	insmod 8188fu.ko
fi

# Mercusys MW300UH clones (Realtek USB): 2c4e:0104
cat /sys/bus/usb/devices/*/uevent | grep -i "PRODUCT=2c4e/104/"
if [ $? -eq 0 ]; then
	log_diag "Detected USB Realtek 2c4e:0104"
	cat /sys/bus/usb/devices/*/uevent | grep -i "PRODUCT=2c4e/104/" >> "$WIFI_DIAG_LOG" 2>/dev/null

	insmod_if_exists cfg80211.ko
	# Mercusys MW300UH is typically RTL8192EU; keep 8188 variants as fallback.
	insmod_if_exists 8192eu.ko || insmod_if_exists 8188fu.ko || insmod_if_exists 8188eu.ko

	if ifconfig -a 2>/dev/null | grep -q "wlan"; then
		log_diag "Wireless interface found after 2c4e:0104 load attempt"
	else
		log_diag "No wireless interface after 2c4e:0104 load attempt"
		ls -1 /oem/usr/ko/*8188*.ko /oem/usr/ko/*8192*.ko >> "$WIFI_DIAG_LOG" 2>/dev/null
	fi
fi

#ssv6115
cat /sys/bus/usb/devices/*/uevent | grep "8065\/6011"
if [ $? -eq 0 ]; then
	insmod cfg80211.ko
	insmod ssv6115.ko
fi

#ssv6x5x
cat /sys/bus/usb/devices/*/uevent | grep "8065\/6000"
if [ $? -eq 0 ]; then
	insmod cfg80211.ko
	insmod libarc4.ko
	insmod mac80211.ko
	insmod ctr.ko
	insmod ccm.ko
	insmod libaes.ko
	insmod aes_generic.ko
	insmod ssv6x5x.ko
fi

#atbm603x
cat /sys/bus/sdio/devices/*/uevent | grep "007A\:6011"
if [ $? -eq 0 ]; then
	insmod cfg80211.ko
	insmod libarc4.ko
	insmod ctr.ko
	insmod ccm.ko
	insmod libaes.ko
	insmod aes_generic.ko
	insmod atbm603x_.ko
fi

#aic8800
if [ -n "$(cat /proc/device-tree/model | grep "W")" ] || \
[ -n "$(cat /sys/bus/sdio/devices/*/uevent | grep "C8A1\:C18D")" ]; then
	insmod cfg80211.ko
	insmod libarc4.ko
	insmod ctr.ko
	insmod ccm.ko
	insmod libaes.ko
	insmod aes_generic.ko
	insmod aic8800_bsp.ko
	sleep 0.2
	insmod aic8800_fdrv.ko
	sleep 2
	insmod aic8800_btlpm.ko
	sleep 0.1
fi

#start wifi app
if ifconfig wlan0 2>&1 | grep -q "not found"; then
	echo "wlan0 not found. Stop run rkwifi_server."
	log_diag "wlan0 not found; rkwifi_server not started"
else
	rkwifi_server start >/dev/null 2>&1 &
	log_diag "rkwifi_server started"
fi
