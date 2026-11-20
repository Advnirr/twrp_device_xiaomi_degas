# Copyright (C) 2024 The Android Open Source Project
# SPDX-License-Identifier: Apache-2.0

# Literal path: PRODUCT_MANUFACTURER/PRODUCT_DEVICE are set in twrp_degas.mk
# only after this file is inherited, so they are empty here.
LOCAL_PATH := device/xiaomi/degas

# A/B
AB_OTA_POSTINSTALL_CONFIG += \
    RUN_POSTINSTALL_system=true \
    POSTINSTALL_PATH_system=system/bin/otapreopt_script \
    FILESYSTEM_TYPE_system=ext4 \
    POSTINSTALL_OPTIONAL_system=true

PRODUCT_PACKAGES += \
    otapreopt_script

# API
PRODUCT_SHIPPING_API_LEVEL := 32

# Boot Control HAL
PRODUCT_PACKAGES += \
    android.hardware.boot@1.2-mtkimpl \
    android.hardware.boot@1.2-mtkimpl.recovery \
    bootctrl.mt6897 \
    bootctrl.mt6897.recovery

# Dynamic Partitions
PRODUCT_USE_DYNAMIC_PARTITIONS := true

# Fastbootd
PRODUCT_PACKAGES += \
    android.hardware.fastboot@1.0-impl-mock \
    fastbootd

# VNDK
PRODUCT_TARGET_VNDK_VERSION := 34

# Update Engine
PRODUCT_PACKAGES += \
    update_engine \
    update_verifier \
    update_engine_sideload

# No health HAL: battery is read from sysfs (TW_USE_LEGACY_BATTERY_SERVICES).

# ── vendor_boot ramdisk00 (platform, used for normal boot AND recovery) ──────
# This is a TARGET_NO_KERNEL build: the vendor kernel modules are not built, so
# the stock ones are shipped verbatim (incl. modules.load / .recovery / .dep).
# Source: stock vendor_boot of HyperOS OS3.0.306.0.WNEEUXM (vermagic
# 6.1.157-android14-11), unmodified. The recovery-only patched touch driver
# lives in recovery/root/lib/modules/ (ramdisk01 overrides it in recovery).
PRODUCT_COPY_FILES += \
    $(call find-copy-subdir-files,*,$(LOCAL_PATH)/vendor_ramdisk/lib/modules,$(TARGET_COPY_OUT_VENDOR_RAMDISK)/lib/modules)

# First-stage fstab. The generated vendor ramdisk has none, and first-stage init
# needs it to mount system/vendor/... Normal boot uses it too, so it is the stock
# HyperOS fstab plus the ColorOS my_* entries (nofail).
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/vendor_ramdisk/first_stage_ramdisk/fstab.mt6897:$(TARGET_COPY_OUT_VENDOR_RAMDISK)/first_stage_ramdisk/fstab.mt6897

# task_profiles.json for the recovery ramdisk. bootable/recovery builds it
# (TWRP_REQUIRED_MODULES) but its copy into recovery/root is commented out
# (bootable/recovery/prebuilt/Android.mk). Without it libprocessgroup aborts
# logd ("Failed to read task profiles from /etc/task_profiles.json").
# Same source as the system copy, so it matches the built libprocessgroup.
PRODUCT_COPY_FILES += \
    system/core/libprocessgroup/profiles/task_profiles.json:$(TARGET_COPY_OUT_RECOVERY)/root/system/etc/task_profiles.json

# UBSan runtime for the stock eSE HAL (vendor.xiaomi.hardware.secure_element-service
# NEEDs libclang_rt.ubsan_standalone-aarch64-android.so, which lives in the
# Android system partition). Taken from the build's own clang, whatever version
# the manifest ships; it provides the __ubsan_handle_* symbols the HAL imports.
DEGAS_UBSAN_RT := $(firstword $(wildcard prebuilts/clang/host/linux-x86/$(LLVM_PREBUILTS_VERSION)/lib/clang/*/lib/linux/libclang_rt.ubsan_standalone-aarch64-android.so) \
    $(wildcard prebuilts/clang/host/linux-x86/clang-r*/lib/clang/*/lib/linux/libclang_rt.ubsan_standalone-aarch64-android.so))
ifeq ($(DEGAS_UBSAN_RT),)
    $(error libclang_rt.ubsan_standalone-aarch64-android.so not found in prebuilts/clang)
endif
PRODUCT_COPY_FILES += \
    $(DEGAS_UBSAN_RT):$(TARGET_COPY_OUT_RECOVERY)/root/vendor/lib64/libclang_rt.ubsan_standalone-aarch64-android.so

# Relinked into recovery (BoardConfig.mk), so they must be built.
PRODUCT_PACKAGES += \
    android.security.aaid_aidl-cpp \
    android.hardware.security.keymint-V3-ndk \
    android.hardware.security.rkp-V3-ndk \
    android.hardware.confirmationui-V1-ndk \
    android.system.suspend-V1-ndk
