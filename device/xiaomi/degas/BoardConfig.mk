DEVICE_PATH := device/xiaomi/degas

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
TARGET_RECOVERY_FSTAB := $(DEVICE_PATH)/recovery.fstab

# CMDLINE
BOARD_KERNEL_CMDLINE := console=tty0 root=/dev/ram nosoftlockup transparent_hugepage=never kasan.page_alloc.sample=1 disable_dma32=on swiotlb=noforce bootopt=64S3,32N2,64N2 androidboot.selinux=permissive printk.disable_uart=0

# ========================================================
# RECOVERY GKI v4 (vendor_boot)
# ========================================================
BOARD_MOVE_RECOVERY_RESOURCES_TO_VENDOR_BOOT := true
BOARD_USES_VENDOR_BOOT := true
BOARD_INCLUDE_RECOVERY_RAMDISK_IN_VENDOR_BOOT := true
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
# FBE decrypt is not achievable from recovery on this device (TEE Root-of-Trust
# binding — see docs/RESEARCH.md), so the crypto stack is intentionally NOT
# included. It pulled in ~11 MB of keymaster/keymint/gatekeeper/vold libs that
# would push the combined vendor_boot over the 64 MB partition. /data is shown
# as encrypted/unmountable; Format Data still works.
# TW_INCLUDE_CRYPTO / TW_INCLUDE_CRYPTO_FBE / TW_INCLUDE_METADATA_DECRYPT — removed.

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
