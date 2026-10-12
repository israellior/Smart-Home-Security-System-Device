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
define PORCHLIGHTD_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(PORCHLIGHTD_BUILDDIR)/porchlightd \
		$(TARGET_DIR)/usr/bin/porchlightd
endef

$(eval $(cmake-package))
