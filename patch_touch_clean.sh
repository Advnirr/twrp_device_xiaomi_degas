#!/bin/bash
# ============================================================================
# patch_touch_clean.sh — assemble a flashable, CLEAN TWRP vendor_boot by
# combining:
#   * vendor_ramdisk00 (kernel modules) from a base image that HAS them, with
#     the patched Goodix touch driver swapped in, and
#   * vendor_ramdisk01 (the freshly built TWRP recovery ramdisk, which already
#     carries our recovery.fstab + battery fix + goodix firmware), with the
#     touch sysfs init triggers added.
#
# WHY a combo: the device tree builds with TARGET_NO_KERNEL, so `mka
# vendorbootimage` produces a vendor_boot WITHOUT the 215 kernel modules
# (touch/display/UFS). Those live in the stock/base vendor_ramdisk00. We graft
# the fresh TWRP ramdisk onto the base's module ramdisk + proven geometry.
#
# Carries NO decrypt scaffolding and does NOT touch sys.usb.config, so
# MTP / USB-OTG / sideload work.
#
# Needs (ramdisk /dev nodes). Run it yourself: `bash patch_touch_clean.sh`
# Requires: unpack_bootimg.py, mkbootimg, lz4, cpio.
# ============================================================================
set -e

WORKDIR="/home/mikhail/MyProjectsFolder/TWRP_Workspace"
MODBASE="$WORKDIR/twrp_degas-vendor_boot.img"          # base: modules + proven geometry
FRESH="$WORKDIR/out/target/product/degas/vendor_boot.img"  # fresh build: TWRP ramdisk01
OUTIMG="$WORKDIR/twrp_degas-vendor_boot-clean.img"
FW_CFG="$WORKDIR/goodix_cfg_group_degas.bin"
FW_BIN="$WORKDIR/goodix_firmware_degas.bin"
PATCHED_KO="$WORKDIR/xiaomi_touch_common_patched.ko"

UNPACK_BIN="unpack_bootimg.py"
MKBOOTIMG_BIN="mkbootimg"

cd "$WORKDIR"
command -v $UNPACK_BIN   >/dev/null || { echo "ERROR: $UNPACK_BIN not found"; exit 1; }
command -v $MKBOOTIMG_BIN >/dev/null || { command -v mkbootimg.py >/dev/null && MKBOOTIMG_BIN=mkbootimg.py || { echo "ERROR: mkbootimg not found"; exit 1; }; }
for f in "$MODBASE" "$FRESH" "$PATCHED_KO" "$FW_CFG" "$FW_BIN"; do
    [ -f "$f" ] || { echo "ERROR: missing $f"; exit 1; }
done

WS="$WORKDIR/touch_clean_workspace"
EXTRACTED="$WS/base_extracted"      # from MODBASE (gives geometry + ramdisk00 modules)
FRESH_EXTRACTED="$WS/fresh_extracted"
BASE_RD="$WS/base_ramdisk00"        # modules ramdisk unpacked
TWRP_RD="$WS/twrp_ramdisk01"        # fresh TWRP ramdisk unpacked

echo "=== Cleaning workspace ==="
rm -rf "$WS"
mkdir -p "$EXTRACTED" "$FRESH_EXTRACTED" "$BASE_RD" "$TWRP_RD"

# 1. Unpack base (modules + geometry)
echo "=== [1/6] Unpacking base $MODBASE (geometry + modules) ==="
$UNPACK_BIN --boot_img "$MODBASE" --out "$EXTRACTED" --format=mkbootimg > "$WS/mkbootimg_args"
ARGS=$(cat "$WS/mkbootimg_args")
printf '#!/bin/bash\n%s --vendor_boot %s\n' "$MKBOOTIMG_BIN $ARGS" "$OUTIMG" > "$WS/repack.sh"
chmod +x "$WS/repack.sh"

# 2. Patch the touch driver into the base's module ramdisk (vendor_ramdisk00)
echo "=== [2/6] Patching touch .ko into vendor_ramdisk00 ==="
sh -c "cd '$BASE_RD' && lz4 -d -c '$EXTRACTED/vendor_ramdisk00' | cpio -idm"
cp "$PATCHED_KO" "$BASE_RD/lib/modules/xiaomi_touch_common.ko"
chmod 644 "$BASE_RD/lib/modules/xiaomi_touch_common.ko"
sh -c "cd '$BASE_RD' && find . | sort | cpio -H newc -o | lz4 -l -12 --favor-decSpeed -c > '$EXTRACTED/vendor_ramdisk00'"
echo "  [+] patched .ko in modules ramdisk"

# 3. Pull the FRESH TWRP ramdisk (vendor_ramdisk01) out of the new build
echo "=== [3/6] Extracting fresh TWRP ramdisk from build ==="
$UNPACK_BIN --boot_img "$FRESH" --out "$FRESH_EXTRACTED" >/dev/null
sh -c "cd '$TWRP_RD' && lz4 -d -c '$FRESH_EXTRACTED/vendor_ramdisk01' | cpio -idm"

# 4. Inject touch firmware (vendor/firmware) + touch init triggers
echo "=== [4/6] Injecting touch firmware + init triggers into fresh TWRP ramdisk ==="
mkdir -p "$TWRP_RD/vendor/firmware" "$TWRP_RD/lib/firmware"
cp "$FW_CFG" "$FW_BIN" "$TWRP_RD/vendor/firmware/"
cp "$FW_CFG" "$FW_BIN" "$TWRP_RD/lib/firmware/"
chmod 644 "$TWRP_RD/vendor/firmware/goodix"* "$TWRP_RD/lib/firmware/goodix"*

# init.recovery.mt6897.rc is loaded by init (ro.hardware=mt6897). Create it with
# ONLY the touch sysfs writes (clean: no decrypt services).
tee "$TWRP_RD/init.recovery.mt6897.rc" > /dev/null << 'RC_EOF'
# Touch driver bring-up
on early-init
    write /sys/class/touch/touch_dev/enable_touch_raw 0

on boot
    write /sys/class/touch/touch_dev/enable_touch_raw 0
    write /sys/class/touch/touch_dev/panel_display "U"

# USB: force the ConfigFS gadget. TWRP's init.rc hardcodes sys.usb.configfs=0,
# which drives the legacy /sys/class/android_usb path — that node does not exist
# on MT6897 (Android 16), so adb/MTP never enumerate. The ConfigFS path (g1
# gadget) is already present in init.rc and works once configfs=1. `on init`
# runs after init.rc's early-init (=0) and before `on fs` (which reads it).
on init
    setprop sys.usb.configfs 1

# USB MTP over ConfigFS. TWRP's init.rc has triggers for adb/fastboot/sideload
# but none for mtp,adb, so enabling MTP would leave the UDC unbound. Create the
# ffs.mtp function and mount its functionfs at boot (runs after init.rc's own
# `on fs && configfs=1` that creates g1), then bind mtp+adb once TWRP's
# twrpmtp-ffs daemon has written the descriptors (sys.usb.ffs.mtp.ready=1).
on fs && property:sys.usb.configfs=1
    mkdir /config/usb_gadget/g1/functions/ffs.mtp
    mkdir /dev/usb-ffs/mtp 0770 shell shell
    # NB: this kernel's functionfs rejects the `dmode` option (Invalid argument);
    # keep it to the same simple options adb uses.
    mount functionfs mtp /dev/usb-ffs/mtp uid=2000,gid=2000

on property:sys.usb.config=mtp,adb && property:sys.usb.ffs.ready=1 && property:sys.usb.ffs.mtp.ready=1 && property:sys.usb.configfs=1
    write /config/usb_gadget/g1/idProduct 0x4EE2
    write /config/usb_gadget/g1/configs/b.1/strings/0x409/configuration "mtp,adb"
    symlink /config/usb_gadget/g1/functions/ffs.mtp /config/usb_gadget/g1/configs/b.1/f1
    symlink /config/usb_gadget/g1/functions/ffs.adb /config/usb_gadget/g1/configs/b.1/f2
    write /config/usb_gadget/g1/UDC ${sys.usb.controller}
    setprop sys.usb.state ${sys.usb.config}
RC_EOF
chmod 644 "$TWRP_RD/init.recovery.mt6897.rc"
echo "  [+] firmware + touch init.recovery.mt6897.rc added"

# 5. Repack the fresh TWRP ramdisk over the base's vendor_ramdisk01
echo "=== [5/6] Repacking TWRP ramdisk ==="
sh -c "cd '$TWRP_RD' && find . | sort | cpio -H newc -o | lz4 -l -12 --favor-decSpeed -c > '$EXTRACTED/vendor_ramdisk01'"

# 6. Build final vendor_boot
echo "=== [6/6] Building final image ==="
bash "$WS/repack.sh"
echo ""
echo "=== sizes ==="
echo "  ramdisk00 (modules+touch): $(stat -c%s "$EXTRACTED/vendor_ramdisk00") B"
echo "  ramdisk01 (TWRP+touch):    $(stat -c%s "$EXTRACTED/vendor_ramdisk01") B"
echo "  final image:               $(stat -c%s "$OUTIMG") B   (partition limit: 67108864)"
[ "$(stat -c%s "$OUTIMG")" -le 67108864 ] && echo "  FITS in vendor_boot ✓" || echo "  ⚠️  OVER 64 MB — won't fit, need to slim further"
echo ""
echo "=== DONE: $OUTIMG ==="
echo "Flash with: fastboot flash vendor_boot $OUTIMG   (or to the active slot)"
