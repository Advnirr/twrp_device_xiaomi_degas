# Inherit from those products. Most specific first.
$(call inherit-product, $(SRC_TARGET_DIR)/product/base.mk)
$(call inherit-product, $(SRC_TARGET_DIR)/product/core_64_bit.mk)

# Inherit some common TWRP stuff.
$(call inherit-product, vendor/twrp/config/common.mk)

$(call inherit-product, device/xiaomi/degas/device.mk)

# Virtual A/B OTA
$(call inherit-product, $(SRC_TARGET_DIR)/product/virtual_ab_ota/compression.mk)
PRODUCT_USE_DYNAMIC_PARTITIONS := true

# Device
PRODUCT_DEVICE := degas
PRODUCT_NAME := twrp_degas
PRODUCT_BRAND := Xiaomi
PRODUCT_MODEL := Xiaomi 14T
PRODUCT_MANUFACTURER := Xiaomi

# Platform
PRODUCT_PLATFORM := mt6897
