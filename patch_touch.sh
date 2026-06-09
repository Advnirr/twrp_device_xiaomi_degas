#!/bin/bash
# ============================================================================
# patch_touch.sh — repack a built TWRP vendor_boot into a flashable image with:
#   * the patched Goodix touch driver + firmware (so touch works in recovery),
#   * a `recovery` wrapper that brings up the keystore stack in-image (waits for
#     keymint, mounts system_a, starts vndservicemanager + keystore2, then execs
#     recovery_real with throw_logger.so preloaded), and
#   * init.rc triggers for tee-supplicant / keymint-mitee / mount-vendor-bg.
#
# This is the "baked into the image" counterpart of decrypt/from_scratch.sh
# (which instead drives the same bring-up live over ADB). See docs/RESEARCH.md.
#
# NOTE: the PATH SETTINGS below are environment-specific. Adjust WORKDIR and the
# input artifact names before running. Requires: unpack_bootimg.py, mkbootimg,
# lz4, cpio, python3, and a prior TWRP build under out/.
# ============================================================================
set -e

# --- PATH SETTINGS (environment-specific — adjust before running) ---
WORKDIR="/home/mikhail/MyProjectsFolder/TWRP_Workspace"
BOOTIMG="$WORKDIR/twrp_degas-vendor_boot.img"
FW_CFG="$WORKDIR/goodix_cfg_group_degas.bin"
FW_BIN="$WORKDIR/goodix_firmware_degas.bin"
PATCHED_KO="$WORKDIR/xiaomi_touch_common_patched.ko"
TWRP_ROOT="$WORKDIR/out/target/product/degas/recovery/root"

UNPACK_BIN="unpack_bootimg.py"
MKBOOTIMG_BIN="mkbootimg"

cd "$WORKDIR"

# Check required tools
if ! command -v $UNPACK_BIN &> /dev/null; then
    echo "ERROR: $UNPACK_BIN not found."; exit 1
fi
if ! command -v $MKBOOTIMG_BIN &> /dev/null; then
    if command -v mkbootimg.py &> /dev/null; then
        MKBOOTIMG_BIN="mkbootimg.py"
    else
        echo "ERROR: mkbootimg not found!"; exit 1
    fi
fi

WS="$WORKDIR/touch_patch_workspace"
EXTRACTED="$WS/extracted_img"
BASE_RD="$WS/base_ramdisk"
TWRP_RD="$WS/twrp_ramdisk"

echo "=== Cleaning old workspace ==="
sudo rm -rf "$WS"
mkdir -p "$EXTRACTED" "$BASE_RD" "$TWRP_RD"

# 1. Unpack vendor_boot
echo "=== [1/5] Unpacking image ==="
$UNPACK_BIN --boot_img "$BOOTIMG" --out "$EXTRACTED" --format=mkbootimg > "$WS/mkbootimg_args"
ARGS=$(cat "$WS/mkbootimg_args")
cat > "$WS/repack.sh" << EOF
#!/bin/bash
$MKBOOTIMG_BIN $ARGS --vendor_boot $WORKDIR/twrp_degas-vendor_boot-touch.img
EOF
chmod +x "$WS/repack.sh"

# 2. Patch vendor_ramdisk00 (kernel modules)
echo "=== [2/5] Patching vendor_ramdisk00 (xiaomi_touch_common.ko) ==="
if [ ! -f "$PATCHED_KO" ]; then
    echo "ERROR: $PATCHED_KO not found!"; exit 1
fi
sudo sh -c "cd '$BASE_RD' && lz4 -d -c '$EXTRACTED/vendor_ramdisk00' | cpio -idm"
echo "  [+] vendor_ramdisk00 unpacked"
sudo cp "$PATCHED_KO" "$BASE_RD/lib/modules/xiaomi_touch_common.ko"
sudo chmod 644 "$BASE_RD/lib/modules/xiaomi_touch_common.ko"
echo "  [+] xiaomi_touch_common.ko (patched) replaced"
sudo sh -c "cd '$BASE_RD' && find . | sort | cpio -H newc -o | lz4 -l -12 --favor-decSpeed -c > '$EXTRACTED/vendor_ramdisk00'"
echo "  [+] vendor_ramdisk00 repacked"

# 3. Unpack vendor_ramdisk01 (TWRP overlay)
echo "=== [3/5] Unpacking TWRP ramdisk ==="
sudo sh -c "cd '$TWRP_RD' && lz4 -d -c '$EXTRACTED/vendor_ramdisk01' | cpio -idm"
echo "  [+] vendor_ramdisk01 unpacked"

# 4. Inject components
echo "=== [4/5] Injecting firmware, libraries, resources ==="

# Goodix firmware
sudo mkdir -p "$TWRP_RD/vendor/firmware" "$TWRP_RD/lib/firmware"
sudo cp "$FW_CFG" "$TWRP_RD/vendor/firmware/"
sudo cp "$FW_BIN" "$TWRP_RD/vendor/firmware/"
sudo cp "$FW_CFG" "$TWRP_RD/lib/firmware/"
sudo cp "$FW_BIN" "$TWRP_RD/lib/firmware/"
sudo chmod 644 "$TWRP_RD/vendor/firmware/goodix"* "$TWRP_RD/lib/firmware/goodix"*
echo "  [+] Goodix firmware injected"

# recovery.fstab
sudo mkdir -p "$TWRP_RD/system/etc"
sudo cp "$WORKDIR/device/xiaomi/degas/recovery.fstab" "$TWRP_RD/system/etc/recovery.fstab"
echo "  [+] recovery.fstab injected"

# VINTF manifest — KeyMint 3 (mitee)
# Pre-populate /vendor in the ramdisk so TWRP finds the manifest BEFORE /vendor is mounted
sudo mkdir -p "$TWRP_RD/vendor/etc/vintf"
sudo tee "$TWRP_RD/vendor/etc/vintf/manifest.xml" \
         "$TWRP_RD/vendor/etc/vintf/manifest_mt6897.xml" > /dev/null << 'VINTF_EOF'
<?xml version="1.0" encoding="utf-8"?>
<manifest version="2.0" type="device">
    <hal format="aidl">
        <name>android.hardware.security.keymint</name>
        <version>3</version>
        <interface>
            <name>IKeyMintDevice</name>
            <instance>default</instance>
        </interface>
    </hal>
    <hal format="aidl">
        <name>android.hardware.security.sharedsecret</name>
        <version>1</version>
        <interface>
            <name>ISharedSecret</name>
            <instance>default</instance>
        </interface>
    </hal>
    <hal format="aidl">
        <name>android.hardware.gatekeeper</name>
        <version>1</version>
        <interface>
            <name>IGatekeeper</name>
            <instance>default</instance>
        </interface>
    </hal>
</manifest>
VINTF_EOF
echo "  [+] Fake vendor VINTF manifest injected (KeyMint 3 + keymaster 4.1)"

# Recovery binary (ADB hack).
# We rename the real recovery to recovery_real (the wrapper above execs it), and
# binary-patch the property name `sys.usb.config` -> `sys.bak.config` inside it.
# This neuters TWRP's own USB-gadget reconfiguration: it can no longer tear down
# / re-init the USB gadget, which keeps the init-provided ADB gadget rock-solid
# throughout the keystore bring-up.
#
# ⚠️ SIDE EFFECT (intentional): with USB-gadget switching disabled, **MTP and
# USB-OTG mode switching do not work** — only ADB. This is a deliberate
# trade-off, not a bug. To restore normal USB behaviour, drop this sed line
# (and revisit why the gadget churn was destabilising ADB in the first place).
sudo cp "$TWRP_ROOT/system/bin/recovery" "$TWRP_RD/system/bin/recovery_real"
sudo chmod 755 "$TWRP_RD/system/bin/recovery_real"
sudo sed -i 's/sys.usb.config/sys.bak.config/g' "$TWRP_RD/system/bin/recovery_real"

sudo tee "$TWRP_RD/system/bin/recovery" > /dev/null << 'WRAPPER_EOF'
#!/system/bin/sh
LOG=/tmp/wrapper.log
echo "=== wrapper start ===" >> $LOG

(
    # Wait for keymint from the RC chain (mount-vendor-bg → tee-supplicant → keymint)
    i=0
    while [ $i -lt 120 ]; do
        KMPID=$(ps -A | grep keymint | grep -v grep | awk '{print $2}' | head -1)
        if [ -n "$KMPID" ]; then
            THREADS=$(ls /proc/$KMPID/task/ 2>/dev/null | wc -l)
            [ "${THREADS:-0}" -gt 1 ] && break
        fi
        sleep 1
        i=$((i+1))
    done
    echo "keymint ready i=$i threads=$THREADS PID=$KMPID" >> $LOG

    # system_a for keystore2
    i=0
    while [ $i -lt 60 ]; do
        [ -b /dev/block/mapper/system_a ] && break
        sleep 1
        i=$((i+1))
    done
    mkdir -p /mnt_sys
    mount -t erofs -o ro /dev/block/mapper/system_a /mnt_sys 2>>$LOG \
        && echo "system_a OK" >> $LOG \
        || echo "system_a FAIL" >> $LOG

    setprop apexd.status activated 2>/dev/null
    /vendor/bin/vndservicemanager /dev/vndbinder >/tmp/vndsd.log 2>&1 &
    echo "vndservicemanager started" >> $LOG
    mkdir -p /tmp/keystore2_db
    LD_LIBRARY_PATH=/mnt_sys/system/lib64:/vendor/lib64 \
        /mnt_sys/system/bin/keystore2 /tmp/keystore2_db >>/tmp/ks2.log 2>&1 &
    echo "keystore2 started" >> $LOG
) &

echo "=== exec recovery_real ===" >> $LOG
exec env \
    LD_PRELOAD=/system/bin/throw_logger.so \
    LD_LIBRARY_PATH=/tw_libs:/vendor/lib64:/system/lib64 \
    /system/bin/recovery_real "$@"
WRAPPER_EOF
sudo chmod 755 "$TWRP_RD/system/bin/recovery"

# GUI resources
[ -d "$TWRP_ROOT/twres" ] && sudo cp -r "$TWRP_ROOT/twres" "$TWRP_RD/"
[ -d "$TWRP_ROOT/res" ]   && sudo cp -r "$TWRP_ROOT/res"   "$TWRP_RD/"
echo "  [+] GUI resources copied"

# throw_logger
sudo cp "$WORKDIR/throw_logger.so" "$TWRP_RD/system/bin/throw_logger.so"
sudo chmod 755 "$TWRP_RD/system/bin/throw_logger.so"
echo "  [+] throw_logger.so injected"

sudo tee "$TWRP_RD/system/bin/mount_vendor_bg.sh" > /dev/null << 'BGSCRIPT_EOF'
#!/system/bin/sh
i=0
while [ $i -lt 60 ]; do
    if [ -b /dev/block/mapper/vendor_a ]; then
        /system/bin/mount -t erofs -o ro /dev/block/mapper/vendor_a /vendor 2>/dev/null
        setprop vendor.keymint.ready 1
        exit 0
    fi
    sleep 1
    i=$((i+1))
done
BGSCRIPT_EOF
sudo chmod 755 "$TWRP_RD/system/bin/mount_vendor_bg.sh"
echo "  [+] mount_vendor_bg.sh created"

# TWRP libs
FORCE_OVERRIDE_LIBS="
android.hardware.authsecret@1.0.so
android.hardware.confirmationui@1.0.so
android.hardware.gatekeeper@1.0.so
android.hardware.keymaster@3.0.so
android.hardware.keymaster@4.0.so
android.hardware.keymaster@4.1.so
android.hardware.oemlock@1.0.so
android.hardware.security.keymint-V1-ndk_platform.so
android.hardware.security.secureclock-V1-ndk_platform.so
android.hardware.weaver@1.0.so
android.security.apc-ndk_platform.so
android.security.authorization-ndk_platform.so
android.security.maintenance-ndk_platform.so
android.system.keystore2-V1-ndk_platform.so
libbase.so
libbinder.so
libbinder_ndk.so
libbootloader_message.so
libc++.so
libcgrouprc.so
libcrypto.so
libcutils.so
libf2fs_sparseblock.so
libfs_mgr.so
libfscrypttwrp.so
libgatekeeper_aidl.so
libgpt_twrp.so
libhidl-gen-utils.so
libhidlbase.so
libhwbinder.so
libkeymaster4_1support.so
libkeymaster4support.so
libkeymaster_messages.so
libkeymint_support.so
libkeystore-attestation-application-id.so
libkeystoreinfo.so
liblp.so
libprocessgroup.so
libresetprop.so
libselinux.so
libsqlite.so
libtar.so
libutils.so
"

sudo mkdir -p "$TWRP_RD/tw_libs"

NEEDED=$(python3 "$WORKDIR/get_needed_libs.py" \
    "$TWRP_ROOT/system/lib64" \
    "$TWRP_ROOT/system/bin/recovery")

ADDED=0
for lib in $NEEDED; do
    if [ ! -f "$TWRP_RD/system/lib64/$lib" ] || \
       echo "$FORCE_OVERRIDE_LIBS" | grep -qw "$lib"; then
        sudo cp "$TWRP_ROOT/system/lib64/$lib" "$TWRP_RD/tw_libs/" 2>/dev/null \
            && ADDED=$((ADDED+1)) || true
    fi
done
echo "  [+] $ADDED libs → /tw_libs"

# RC triggers
RC_FILE="$TWRP_RD/init.recovery.mt6897.rc"
if [ ! -f "$RC_FILE" ]; then
    echo "ERROR: $RC_FILE not found!"; exit 1
fi
sudo tee -a "$RC_FILE" > /dev/null << 'RC_EOF'

service tee-supplicant /vendor/bin/tee-supplicant
    user root
    group root
    capabilities SYS_RAWIO
    seclabel u:r:recovery:s0
    disabled

service mount-vendor-bg /system/bin/sh /system/bin/mount_vendor_bg.sh
    oneshot
    seclabel u:r:recovery:s0
    disabled

service vendor.keymint-mitee /vendor/bin/hw/android.hardware.security.keymint@3.0-service.mitee
    class hal
    user root
    group root
    seclabel u:r:recovery:s0
    disabled

service recovery /system/bin/recovery
    override
    seclabel u:r:recovery:s0

on early-init
    write /sys/class/touch/touch_dev/enable_touch_raw 0

on boot
    start servicemanager
    start mount-vendor-bg
    write /sys/class/touch/touch_dev/enable_touch_raw 0
    write /sys/class/touch/touch_dev/panel_display "U"

on property:vendor.keymint.ready=1
    start tee-supplicant

on property:init.svc.tee-supplicant=running
    start vendor.keymint-mitee
RC_EOF
echo "  [+] RC triggers injected"

# 5. Build the final image
echo "=== [5/5] Repacking and building ==="
sudo sh -c "cd '$TWRP_RD' && find . | sort | cpio -H newc -o | lz4 -l -12 --favor-decSpeed -c > '$EXTRACTED/vendor_ramdisk01'"
echo "  [+] vendor_ramdisk01 repacked"

bash "$WS/repack.sh"

echo ""
echo "=== DONE! twrp_degas-vendor_boot-touch.img created in $WORKDIR ==="
