################################################################################
#
# python-ai-edge-litert
#
################################################################################

# cp314 because the interpreter is a CPython extension module, unlike
# python-livekit's FFI library: it loads only into the Python it was built for,
# and this image's is 3.14. 2.3.0 is the first release checked against it.
PYTHON_AI_EDGE_LITERT_VERSION = 2.3.0
PYTHON_AI_EDGE_LITERT_SOURCE = ai_edge_litert-$(PYTHON_AI_EDGE_LITERT_VERSION)-cp314-cp314-manylinux_2_27_aarch64.whl
PYTHON_AI_EDGE_LITERT_SITE = https://files.pythonhosted.org/packages/3b/16/49b1997efd47ac188c8eb23473c328ad9e2ddb4dd1f14bb44628074aa169
PYTHON_AI_EDGE_LITERT_LICENSE = Apache-2.0
PYTHON_AI_EDGE_LITERT_LICENSE_FILES = ai_edge_litert-$(PYTHON_AI_EDGE_LITERT_VERSION).dist-info/licenses/LICENSE

define PYTHON_AI_EDGE_LITERT_EXTRACT_CMDS
	unzip -q $(PYTHON_AI_EDGE_LITERT_DL_DIR)/$(PYTHON_AI_EDGE_LITERT_SOURCE) -d $(@D)
endef

# The wheel is 41 MB unpacked; the interpreter needs 17 MB of it. These are
# the files `import ai_edge_litert.interpreter` reaches: the package's
# __init__, the interpreter and the two metrics modules it imports, its
# extension, and the one library that extension links. libLiteRt.so is left
# out on purpose - __init__ loads it only if it is there, and only the
# compiled-model API uses it. The wheel's other dependencies (protobuf,
# flatbuffers, ml_dtypes, tqdm) belong to the parts left out as well.
PYTHON_AI_EDGE_LITERT_FILES = \
	__init__.py \
	interpreter.py \
	metrics_portable.py \
	metrics_interface.py \
	_pywrap_litert_interpreter_wrapper.so \
	libpywrap_litert_common.so

define PYTHON_AI_EDGE_LITERT_INSTALL_TARGET_CMDS
	for f in $(PYTHON_AI_EDGE_LITERT_FILES); do \
		$(INSTALL) -D -m 0644 $(@D)/ai_edge_litert/$$f \
			$(TARGET_DIR)/usr/lib/python$(PYTHON3_VERSION_MAJOR)/site-packages/ai_edge_litert/$$f \
			|| exit 1; \
	done
	chmod 0755 $(TARGET_DIR)/usr/lib/python$(PYTHON3_VERSION_MAJOR)/site-packages/ai_edge_litert/*.so
endef

$(eval $(generic-package))
