DEVICE_PATH := device/xiaomi/degas

# Stock kernel modules (.ko, ELF) are shipped via PRODUCT_COPY_FILES into the
# vendor ramdisk (see device.mk) — this is a TARGET_NO_KERNEL build.
BUILD_BROKEN_ELF_PREBUILT_PRODUCT_COPY_FILES := true

# Arch
TARGET_ARCH := arm64
TARGET_ARCH_VARIANT := armv8-a
TARGET_CPU_ABI := arm64-v8a
TARGET_CPU_ABI2 :=
TARGET_CPU_VARIANT := generic
TARGET_CPU_VARIANT_RUNTIME := cortex-a715
TARGET_SUPPORTS_64_BIT_APPS := true

# Sec Arch
TARGET_2ND_ARCH := arm
TARGET_2ND_ARCH_VARIANT := armv8-a
TARGET_2ND_CPU_ABI := armeabi-v7a
TARGET_2ND_CPU_ABI2 := armeabi
TARGET_2ND_CPU_VARIANT := generic
TARGET_2ND_CPU_VARIANT_RUNTIME := cortex-a510

# Platform
TARGET_BOARD_PLATFORM := mt6897
TARGET_BOOTLOADER_BOARD_NAME := degas

# Kernel Settings (GKI v4)
BOARD_BOOT_HEADER_VERSION := 4
BOARD_VENDOR_BOOT_HEADER_VERSION := 4
BOARD_MKBOOTIMG_ARGS += --header_version 4 --dtb $(TARGET_PREBUILT_DTB)
# The vendor_boot rule passes only --base/--pagesize; the BOARD_*_OFFSET vars
# below are NOT used for vendor_boot, so pass them explicitly. Without this the
# header gets mkbootimg defaults (kernel 0x8000, ramdisk 0x1000000, tags 0x100,
# dtb 0x1f00000) and the device dies before the screen comes on. Values = stock
# vendor_boot (kernel 0x40000000, ramdisk 0x66f00000, tags/dtb 0x47c80000).
BOARD_MKBOOTIMG_ARGS += --kernel_offset $(BOARD_KERNEL_OFFSET)
BOARD_MKBOOTIMG_ARGS += --ramdisk_offset $(BOARD_RAMDISK_OFFSET)
BOARD_MKBOOTIMG_ARGS += --tags_offset $(BOARD_TAGS_OFFSET)
BOARD_MKBOOTIMG_ARGS += --dtb_offset $(BOARD_DTB_OFFSET)
BOARD_KERNEL_PAGESIZE := 4096
BOARD_RAMDISK_USE_LZ4 := true
BOARD_KERNEL_BASE        := 0x00000000
BOARD_KERNEL_OFFSET      := 0x40000000
BOARD_RAMDISK_OFFSET     := 0x66f00000
BOARD_TAGS_OFFSET        := 0x47c80000
BOARD_DTB_OFFSET         := 0x47c80000
TARGET_NO_KERNEL := true
BOARD_KERNEL_IMAGE_NAME := Image
TARGET_PREBUILT_DTB := $(DEVICE_PATH)/prebuilt/dtb/degas.dtb
TARGET_RECOVERY_FSTAB := $(DEVICE_PATH)/recovery/root/system/etc/recovery.fstab

# CMDLINE
# Vendor cmdline exactly as in the stock vendor_boot (and the working Advnirr
# image, which reused the stock header). Kernel cmdline itself comes from boot.
BOARD_KERNEL_CMDLINE := bootopt=64S3,32N2,64N2

# ========================================================
# RECOVERY GKI v4 (vendor_boot)
# ========================================================
BOARD_MOVE_RECOVERY_RESOURCES_TO_VENDOR_BOOT := true
BOARD_USES_VENDOR_BOOT := true
BOARD_INCLUDE_RECOVERY_RAMDISK_IN_VENDOR_BOOT := true

# Stock kernel modules go into the main vendor ramdisk via PRODUCT_COPY_FILES
# (device.mk, vendor_ramdisk/). A prebuilt vendor_boot ramdisk fragment does not
# work here: first-stage init never loads modules from it.
BOARD_RECOVERYIMAGE_PARTITION_SIZE := 104857600
# vendor_boot partition is 64 MB on degas — needed so AVB add_hash_footer gets a
# size (build was failing at the footer step with an empty --partition_size).
BOARD_VENDOR_BOOTIMAGE_PARTITION_SIZE := 67108864

# Partitions
AB_OTA_UPDATER := true
AB_OTA_PARTITIONS += boot dtbo vendor_boot init_boot vbmeta vbmeta_system vbmeta_vendor

BOARD_SUPER_PARTITION_GROUPS := xiaomi_dynamic_partitions
BOARD_XIAOMI_DYNAMIC_PARTITIONS_PARTITION_LIST := system system_ext vendor product odm vendor_dlkm odm_dlkm
BOARD_XIAOMI_DYNAMIC_PARTITIONS_SIZE := 9122611200
BOARD_SUPER_PARTITION_SIZE := 9126805504

BOARD_USES_VENDORIMAGE := true
TARGET_COPY_OUT_VENDOR := vendor

# TWRP Settings
TW_THEME := portrait_hdpi
# Maintainer tag shown next to the version (e.g. "3.7.1_14-Advnirr").
TW_DEVICE_VERSION := Advnirr
RECOVERY_SDCARD_ON_DATA := true
TARGET_RECOVERY_PIXEL_FORMAT := BGRA_8888
TW_BRIGHTNESS_PATH := "/sys/class/leds/lcd-backlight/brightness"
TW_MAX_BRIGHTNESS := 2047
TW_DEFAULT_BRIGHTNESS := 1200
TW_HAPTICS_OEM_VIBRATOR := true
# Touch and display
BOARD_HAS_MTK_HARDWARE := true
TW_INPUT_BLACKLIST := "hbtp_vm"

# Battery — read sysfs directly instead of the health HAL. Without
# TW_USE_LEGACY_BATTERY_SERVICES, TWRP queries android.hardware.health over
# binder, which is unreliable in standalone recovery → stuck at 100% / only
# updates after a manual restart. Legacy mode reads the gauge node directly.
# USB: MTP enabled. The matching ConfigFS support (ffs.mtp function + mount +
# the `on property:sys.usb.config=mtp,adb ... configfs=1` bind trigger) is added
# via init.recovery.mt6897.rc. Without it,
# enabling MTP would unbind the gadget UDC and never rebind -> all USB dies.

TW_USE_LEGACY_BATTERY_SERVICES := true
# Pin the real gauge node (battery + bms are both type=Battery, plus many
# charger nodes → auto-detect was ambiguous). capacity/status track real charge.
TW_CUSTOM_BATTERY_PATH := /sys/class/power_supply/battery
# TW_SCREEN_BLANK_ON_BOOT := true
# TW_NO_SCREEN_BLANK := true
# TW_NO_SCREEN_TIMEOUT := true

# SEPOLICY
BOARD_SEPOLICY_DIRS += $(DEVICE_PATH)/sepolicy

# ── Encryption ────────────────────────────────────────
# FBE v2 + metadata encryption (dm-default-key), KeyMint 3.0 / Gatekeeper in
# MiTEE: stock 306 blobs + init in recovery/root (see init.recovery.mt6897.rc).
TW_INCLUDE_CRYPTO := true
TW_INCLUDE_CRYPTO_FBE := true
TW_INCLUDE_FBE_METADATA_DECRYPT := true
BOARD_USES_METADATA_PARTITION := true
TW_USE_FSCRYPT_POLICY := 2
# logd + logcat: with TW_INCLUDE_CRYPTO the recovery binary and libtar link
# libsysutils, which recovery only installs together with logd. Without it the
# recovery service fails to link ("libsysutils.so not found") and the device
# sits on the boot logo. logcat is also needed: keystore2 logs only to logd.
TWRP_INCLUDE_LOGCAT := true
TARGET_USES_LOGD := true
# Native OMAPI service for the Weaver HAL (device/xiaomi/degas/omapi): built as
# a vendor binary, copied into the recovery ramdisk's /vendor/bin/hw.
TW_RECOVERY_ADDITIONAL_RELINK_VENDOR_HW_BINARY_FILES += $(TARGET_OUT_VENDOR_EXECUTABLES)/hw/se_omapi_recovery
# Use only recovery.fstab. Otherwise TWRP re-parses the vendor fstab
# (/etc/additional.fstab) and its stock /metadata options replace ours, so
# /metadata got mounted rw although recovery.fstab says ro.
TW_SKIP_ADDITIONAL_FSTAB := true
# PLATFORM_SECURITY_PATCH := 2099-12-31 is deliberately NOT used: KeyMint's
# OS version / patch levels are set at runtime from the device build.props by
# degas_prepdecrypt.sh. A recovery patch level above Android's would make
# KeyMint request a key *upgrade* and vold would rewrite the key blob.

# ── AVB ───────────────────────────────────────────────
BOARD_AVB_ENABLE                          := true
BOARD_AVB_MAKE_VBMETA_IMAGE_ARGS          += --flags 3
BOARD_AVB_RECOVERY_KEY_PATH               := external/avb/test/data/testkey_rsa4096.pem
BOARD_AVB_RECOVERY_ALGORITHM              := SHA256_RSA4096
BOARD_AVB_RECOVERY_ROLLBACK_INDEX         := 1
BOARD_AVB_RECOVERY_ROLLBACK_INDEX_LOCATION := 1

# ── Dynamic Partition sizes ──
BOARD_SYSTEMIMAGE_PARTITION_RESERVED_SIZE   := 104857600
BOARD_VENDORIMAGE_PARTITION_RESERVED_SIZE   := 104857600
BOARD_VENDORIMAGE_FILE_SYSTEM_TYPE          := ext4
BOARD_SYSTEMIMAGE_FILE_SYSTEM_TYPE          := ext4
BOARD_PRODUCTIMAGE_FILE_SYSTEM_TYPE         := ext4
BOARD_SYSTEM_EXTIMAGE_FILE_SYSTEM_TYPE      := ext4
TARGET_COPY_OUT_PRODUCT                     := product
TARGET_COPY_OUT_SYSTEM_EXT                  := system_ext

TW_INCLUDE_RESETPROP := true
