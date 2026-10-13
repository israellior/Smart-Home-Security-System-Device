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

# Nothing to compile: two Python scripts and their units.
PORCHLIGHT_BOOT_DEPENDENCIES = porchlightd

# The scripts go where provision.sh puts them on Raspberry Pi OS, because the
# units' ExecStart lines name that path and stay the same on both.
define PORCHLIGHT_BOOT_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/porchlight-firstboot.py \
		$(TARGET_DIR)/usr/local/lib/porchlight/porchlight-firstboot.py
	$(INSTALL) -D -m 0755 $(@D)/porchlight-setup.py \
		$(TARGET_DIR)/usr/local/lib/porchlight/porchlight-setup.py
endef

# Installing a unit is enough to enable it: Buildroot runs systemctl preset-all
# at the end of the build, and its preset enables every unit with an [Install]
# section. Both units say WantedBy=multi-user.target.
define PORCHLIGHT_BOOT_INSTALL_INIT_SYSTEMD
	$(INSTALL) -D -m 0644 $(@D)/systemd/porchlight-firstboot.service \
		$(TARGET_DIR)/usr/lib/systemd/system/porchlight-firstboot.service
	$(INSTALL) -D -m 0644 $(@D)/systemd/porchlight-setup.service \
		$(TARGET_DIR)/usr/lib/systemd/system/porchlight-setup.service
endef

$(eval $(generic-package))
