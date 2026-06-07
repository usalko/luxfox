################################################################################
#
# uhubctl
#
################################################################################

UHUBCTL_VERSION = 2.4.0
UHUBCTL_SITE = $(call github,mvp,uhubctl,$(UHUBCTL_VERSION))
UHUBCTL_LICENSE = GPL-2.0+
UHUBCTL_LICENSE_FILES = README.md

UHUBCTL_DEPENDENCIES = libusb

define UHUBCTL_BUILD_CMDS
	$(MAKE) -C $(@D)
endef

define UHUBCTL_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/uhubctl $(TARGET_DIR)/usr/sbin/uhubctl
endef

$(eval $(generic-package))
