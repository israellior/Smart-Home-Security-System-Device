################################################################################
#
# porchlight-face-models
#
################################################################################

# BlazeFace from Google's own hosted copy. The URL carries no version, so if
# the file there ever changes the hash below stops the build rather than
# quietly shipping a different model.
PORCHLIGHT_FACE_MODELS_VERSION = 1
PORCHLIGHT_FACE_MODELS_SOURCE = face_detection_full_range.tflite
PORCHLIGHT_FACE_MODELS_SITE = https://storage.googleapis.com/mediapipe-assets

# SFace exists as TFLite only because we converted it, so it lives on a
# release of this repository - see convert-sface.sh beside this file for how
# it was made and how to check it.
PORCHLIGHT_FACE_MODELS_SFACE = sface_2021dec_float32.tflite
PORCHLIGHT_FACE_MODELS_EXTRA_DOWNLOADS = \
	https://github.com/israellior/Smart-Home-Security-System-Device/releases/download/face-models-1/$(PORCHLIGHT_FACE_MODELS_SFACE)

PORCHLIGHT_FACE_MODELS_LICENSE = Apache-2.0

# Neither file is an archive, so there is nothing to unpack: copy them in.
define PORCHLIGHT_FACE_MODELS_EXTRACT_CMDS
	cp $(PORCHLIGHT_FACE_MODELS_DL_DIR)/$(PORCHLIGHT_FACE_MODELS_SOURCE) \
		$(PORCHLIGHT_FACE_MODELS_DL_DIR)/$(PORCHLIGHT_FACE_MODELS_SFACE) $(@D)/
endef

# Which model makes the codes, said once, here: porchlight-faces.py tags the
# saved faces with it and the bridge reports it in its hello. A new model must
# get a new id, or codes from the old one would be compared with the new.
PORCHLIGHT_FACE_MODELS_CODE_ID = sface_2021dec_float32

# The names porchlight-faces.py looks for by default.
define PORCHLIGHT_FACE_MODELS_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0644 $(@D)/$(PORCHLIGHT_FACE_MODELS_SOURCE) \
		$(TARGET_DIR)/usr/local/share/porchlight/face_detection_full_range.tflite
	$(INSTALL) -D -m 0644 $(@D)/$(PORCHLIGHT_FACE_MODELS_SFACE) \
		$(TARGET_DIR)/usr/local/share/porchlight/face_code.tflite
	echo $(PORCHLIGHT_FACE_MODELS_CODE_ID) > $(TARGET_DIR)/usr/local/share/porchlight/face_code.id
	chmod 0644 $(TARGET_DIR)/usr/local/share/porchlight/face_code.id
endef

$(eval $(generic-package))
