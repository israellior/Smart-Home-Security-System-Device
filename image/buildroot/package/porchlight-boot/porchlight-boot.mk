################################################################################
#
# porchlight-boot
#
################################################################################

# From this repository's pi/, like porchlightd from porchlightd/.
PORCHLIGHT_BOOT_VERSION = local
PORCHLIGHT_BOOT_SITE = $(BR2_EXTERNAL_PORCHLIGHT_PATH)/../../pi
PORCHLIGHT_BOOT_SITE_METHOD = local
PORCHLIGHT_BOOT_LICENSE = Proprietary

# Nothing to compile: Python scripts and two units. firstboot and setup run
# before the daemon; server-bridge.py is the daemon's child, holding the one
# WebSocket that makes the device "online" in the app.
PORCHLIGHT_BOOT_DEPENDENCIES = porchlightd

# The scripts go where provision.sh puts them on Raspberry Pi OS, because the
# units' ExecStart lines name that path and stay the same on both.
define PORCHLIGHT_BOOT_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/porchlight-firstboot.py \
		$(TARGET_DIR)/usr/local/lib/porchlight/porchlight-firstboot.py
	$(INSTALL) -D -m 0755 $(@D)/porchlight-setup.py \
		$(TARGET_DIR)/usr/local/lib/porchlight/porchlight-setup.py
	$(INSTALL) -D -m 0755 $(@D)/server-bridge.py \
		$(TARGET_DIR)/usr/local/lib/porchlight/server-bridge.py
	$(INSTALL) -D -m 0755 $(@D)/porchlight-update.py \
		$(TARGET_DIR)/usr/local/lib/porchlight/porchlight-update.py
	$(INSTALL) -D -m 0755 $(@D)/porchlight-wifi.py \
		$(TARGET_DIR)/usr/local/lib/porchlight/porchlight-wifi.py
endef

# Installing a unit is enough to enable it: Buildroot runs systemctl preset-all
# at the end of the build, and its preset enables every unit with an [Install]
# section. Every unit here says WantedBy=multi-user.target - the update
# service included, through its path unit: the service has no [Install] of its
# own because only a request should ever start it.
define PORCHLIGHT_BOOT_INSTALL_INIT_SYSTEMD
	$(INSTALL) -D -m 0644 $(@D)/systemd/porchlight-firstboot.service \
		$(TARGET_DIR)/usr/lib/systemd/system/porchlight-firstboot.service
	$(INSTALL) -D -m 0644 $(@D)/systemd/porchlight-setup.service \
		$(TARGET_DIR)/usr/lib/systemd/system/porchlight-setup.service
	$(INSTALL) -D -m 0644 $(@D)/systemd/porchlight-update.path \
		$(TARGET_DIR)/usr/lib/systemd/system/porchlight-update.path
	$(INSTALL) -D -m 0644 $(@D)/systemd/porchlight-update.service \
		$(TARGET_DIR)/usr/lib/systemd/system/porchlight-update.service
	$(INSTALL) -D -m 0644 $(@D)/systemd/porchlight-wifi.path \
		$(TARGET_DIR)/usr/lib/systemd/system/porchlight-wifi.path
	$(INSTALL) -D -m 0644 $(@D)/systemd/porchlight-wifi.service \
		$(TARGET_DIR)/usr/lib/systemd/system/porchlight-wifi.service
endef

$(eval $(generic-package))
