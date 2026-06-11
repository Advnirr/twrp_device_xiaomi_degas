# Copyright (C) 2024 The Android Open Source Project
# SPDX-License-Identifier: Apache-2.0

LOCAL_PATH := device/$(PRODUCT_MANUFACTURER)/$(PRODUCT_DEVICE)

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

# NOTE: FBE decrypt is parked (TEE RoT-bound, unreachable from recovery), so the
# vold / keymaster / gatekeeper packages were removed — they only bloated the
# ramdisk. The health@2.0 HAL was also dropped: battery now reads sysfs directly
# (TW_USE_LEGACY_BATTERY_SERVICES), so the HAL is no longer needed.
