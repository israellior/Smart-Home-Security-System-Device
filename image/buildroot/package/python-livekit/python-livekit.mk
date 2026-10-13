################################################################################
#
# python-livekit
#
################################################################################

# 1.1.19 because it is the version webrtc-video.py ran against on porch-2.
# A wheel, not the sdist: the sdist builds liblivekit_ffi from Rust. py3-none
# because Python reaches that library through FFI rather than a CPython
# extension, so it does not care that the image's Python is 3.14.
PYTHON_LIVEKIT_VERSION = 1.1.19
PYTHON_LIVEKIT_SOURCE = livekit-$(PYTHON_LIVEKIT_VERSION)-py3-none-manylinux_2_28_aarch64.whl
PYTHON_LIVEKIT_SITE = https://files.pythonhosted.org/packages/ec/8e/b05ea0cd5fa804ef30fff5e1e0e980413e253edb5bd1ec62265836dbf14e
PYTHON_LIVEKIT_LICENSE = Apache-2.0

# A wheel is a zip with the installed layout already in it: the package and
# its .dist-info, ready to drop into site-packages. unzip is one of
# Buildroot's own host requirements.
define PYTHON_LIVEKIT_EXTRACT_CMDS
	unzip -q $(PYTHON_LIVEKIT_DL_DIR)/$(PYTHON_LIVEKIT_SOURCE) -d $(@D)
endef

define PYTHON_LIVEKIT_INSTALL_TARGET_CMDS
	mkdir -p $(TARGET_DIR)/usr/lib/python$(PYTHON3_VERSION_MAJOR)/site-packages
	cp -a $(@D)/livekit $(@D)/livekit-$(PYTHON_LIVEKIT_VERSION).dist-info \
		$(TARGET_DIR)/usr/lib/python$(PYTHON3_VERSION_MAJOR)/site-packages/
endef

$(eval $(generic-package))
