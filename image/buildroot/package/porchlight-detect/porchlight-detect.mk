################################################################################
#
# porchlight-detect
#
################################################################################

# Google's own hosted copy, unchanged since 2018. Quantised to uint8, so it
# runs on the CPU's integer units, and it ends in TFLite_Detection_PostProcess,
# which is what PersonModel in record-clip.py reads its outputs by. Not YOLO:
# Ultralytics' is AGPL, which a device that ships cannot carry.
PORCHLIGHT_DETECT_VERSION = 1.0_quant_2018_06_29
PORCHLIGHT_DETECT_SOURCE = coco_ssd_mobilenet_v1_$(PORCHLIGHT_DETECT_VERSION).zip
PORCHLIGHT_DETECT_SITE = https://storage.googleapis.com/download.tensorflow.org/models/tflite
PORCHLIGHT_DETECT_LICENSE = Apache-2.0

define PORCHLIGHT_DETECT_EXTRACT_CMDS
	unzip -q $(PORCHLIGHT_DETECT_DL_DIR)/$(PORCHLIGHT_DETECT_SOURCE) -d $(@D)
endef

# The path is porchlightd.json's detect.model default, so turning detection on
# is one line of config and nothing else.
define PORCHLIGHT_DETECT_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0644 $(@D)/detect.tflite \
		$(TARGET_DIR)/usr/local/share/porchlight/person.tflite
endef

$(eval $(generic-package))
