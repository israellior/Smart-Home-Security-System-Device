################################################################################
#
# porchlight-ab
#
################################################################################

# Everything is in this directory; there is nothing to download or build.
PORCHLIGHT_AB_VERSION = local
PORCHLIGHT_AB_SITE = $(PORCHLIGHT_AB_PKGDIR)
PORCHLIGHT_AB_SITE_METHOD = local
PORCHLIGHT_AB_LICENSE = Proprietary

define PORCHLIGHT_AB_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0644 $(@D)/system.conf $(TARGET_DIR)/etc/rauc/system.conf
	$(INSTALL) -D -m 0644 $(@D)/keyring.pem $(TARGET_DIR)/etc/rauc/keyring.pem
	$(INSTALL) -D -m 0644 $(@D)/fw_env.config $(TARGET_DIR)/etc/fw_env.config
	$(INSTALL) -D -m 0644 $(@D)/nm-data.conf \
		$(TARGET_DIR)/etc/NetworkManager/conf.d/porchlight-data.conf
	$(INSTALL) -D -m 0644 $(@D)/watchdog.conf \
		$(TARGET_DIR)/etc/systemd/system.conf.d/porchlight-watchdog.conf
	$(INSTALL) -D -m 0644 $(@D)/journald.conf \
		$(TARGET_DIR)/etc/systemd/journald.conf.d/porchlight.conf
	$(INSTALL) -D -m 0644 $(@D)/journal-flush-data.conf \
		$(TARGET_DIR)/usr/lib/systemd/system/systemd-journal-flush.service.d/porchlight-data.conf
	$(INSTALL) -D -m 0644 $(@D)/firstboot-boot-rw.conf \
		$(TARGET_DIR)/usr/lib/systemd/system/porchlight-firstboot.service.d/boot-rw.conf
	$(INSTALL) -D -m 0755 $(@D)/porchlight-data \
		$(TARGET_DIR)/usr/libexec/porchlight/porchlight-data
	$(INSTALL) -D -m 0755 $(@D)/porchlight-health \
		$(TARGET_DIR)/usr/libexec/porchlight/porchlight-health
endef

# Enabled by Buildroot's preset-all, like every unit with an [Install] section.
define PORCHLIGHT_AB_INSTALL_INIT_SYSTEMD
	$(INSTALL) -D -m 0644 $(@D)/porchlight-data.service \
		$(TARGET_DIR)/usr/lib/systemd/system/porchlight-data.service
	$(INSTALL) -D -m 0644 $(@D)/porchlight-health.service \
		$(TARGET_DIR)/usr/lib/systemd/system/porchlight-health.service
endef

$(eval $(generic-package))
