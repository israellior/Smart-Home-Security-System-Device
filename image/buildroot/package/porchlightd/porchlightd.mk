################################################################################
#
# porchlightd
#
################################################################################

# Built from this repository, not downloaded: the daemon and the image change
# together, in one commit. Buildroot copies the tree with rsync on every
# porchlightd-rebuild, so an edit is picked up without a version bump.
PORCHLIGHTD_VERSION = local
PORCHLIGHTD_SITE = $(BR2_EXTERNAL_PORCHLIGHT_PATH)/../../porchlightd
PORCHLIGHTD_SITE_METHOD = local
PORCHLIGHTD_LICENSE = Proprietary

# json-for-modern-cpp is header-only, but if CMake cannot find it, it fetches
# it from GitHub mid-build. Depending on it is what keeps that from happening.
PORCHLIGHTD_DEPENDENCIES = host-pkgconf libgpiod2 json-for-modern-cpp

PORCHLIGHTD_CONF_OPTS = -DPORCHLIGHT_GPIO=ON

# CMakeLists.txt has no install() rules, so cmake --install would put nothing
# on the device.
#
# The config is the tracked example as it is: it already names the gpio
# backends, and porchlight-firstboot.py writes device_id and server.base_url
# into it from the birth certificate. Every unit is the same until then.
define PORCHLIGHTD_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(PORCHLIGHTD_BUILDDIR)/porchlightd \
		$(TARGET_DIR)/usr/bin/porchlightd
	$(INSTALL) -D -m 0644 $(@D)/porchlightd.example.json \
		$(TARGET_DIR)/etc/porchlight/porchlightd.json
endef

# The daemon's own user: no password, no home, no shell. firstboot gives it the
# credential, and the unit runs as it.
define PORCHLIGHTD_USERS
	porchlight -1 porchlight -1 * - - - Porchlight doorbell
endef

# Clips wait here until they are uploaded, so it is the daemon's to write.
define PORCHLIGHTD_PERMISSIONS
	/var/lib/porchlight/spool d 0750 porchlight porchlight - - - - -
endef

$(eval $(cmake-package))
