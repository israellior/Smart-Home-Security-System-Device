################################################################################
#
# porchlight-call
#
################################################################################

PORCHLIGHT_CALL_VERSION = local
PORCHLIGHT_CALL_SITE = $(BR2_EXTERNAL_PORCHLIGHT_PATH)/../../pi
PORCHLIGHT_CALL_SITE_METHOD = local
PORCHLIGHT_CALL_LICENSE = Proprietary

# porchlightd.json runs the call as media.python + media.script. On Raspberry
# Pi OS media.python is a venv holding the pip-installed SDK; here the SDK is
# part of the image, so the venv's interpreter is the system one. A link
# rather than a different config, so one porchlightd.json serves both - and
# made here rather than in the overlay, because a Windows checkout turns a
# symlink into a text file.
define PORCHLIGHT_CALL_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/webrtc-video.py \
		$(TARGET_DIR)/usr/local/lib/porchlight/webrtc-video.py
	mkdir -p $(TARGET_DIR)/opt/porchlight/venv/bin
	ln -sf /usr/bin/python3 $(TARGET_DIR)/opt/porchlight/venv/bin/python3
endef

$(eval $(generic-package))
