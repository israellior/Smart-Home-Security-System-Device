################################################################################
#
# porchlight-recorder
#
################################################################################

# From this repository's pi/, like porchlight-boot.
PORCHLIGHT_RECORDER_VERSION = local
PORCHLIGHT_RECORDER_SITE = $(BR2_EXTERNAL_PORCHLIGHT_PATH)/../../pi
PORCHLIGHT_RECORDER_SITE_METHOD = local
PORCHLIGHT_RECORDER_LICENSE = Proprietary

# Where porchlightd.json's recorder and server.uploader_path look for them.
define PORCHLIGHT_RECORDER_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/record-clip.py \
		$(TARGET_DIR)/usr/local/lib/porchlight/record-clip.py
	$(INSTALL) -D -m 0755 $(@D)/upload-clip.py \
		$(TARGET_DIR)/usr/local/lib/porchlight/upload-clip.py
endef

$(eval $(generic-package))
