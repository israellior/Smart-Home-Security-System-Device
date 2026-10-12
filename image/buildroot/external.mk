# Every recipe under package/ is picked up without being listed here.
# With no packages yet the wildcard matches nothing and this includes nothing.
include $(sort $(wildcard $(BR2_EXTERNAL_PORCHLIGHT_PATH)/package/*/*.mk))
