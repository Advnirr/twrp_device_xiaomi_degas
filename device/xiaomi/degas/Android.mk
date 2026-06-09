LOCAL_PATH := $(call my-dir)

ifeq ($(TARGET_DEVICE),degas)
include $(call all-makefiles-under,$(LOCAL_PATH))
endif
