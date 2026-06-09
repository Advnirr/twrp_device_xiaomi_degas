#!/bin/bash
# ============================================================================
# from_scratch.sh — bring up the whole binder/keystore stack inside TWRP and
#                   attempt `twrp decrypt` from a clean state.
#
# This is the master orchestration script. It builds the LD_PRELOAD shims,
# pushes them to the device, sets up the mounts/binderfs the stack needs, then
# starts servicemanager -> hwservicemanager -> keymint -> gatekeeper ->
# keystore2 in order, and reports whether keystore2 registered.
#
# Run it with the device booted into this TWRP build and reachable over ADB.
# See ../README.md and ../docs/RESEARCH.md for the full theory and current
# blocker.  `adb` here is the wrapper in this directory (filters linker noise).
# ============================================================================
# Resolve this script's own directory, so it runs from anywhere.
PRE="$(cd "$(dirname "$0")" && pwd)"
cd "$PRE"
die() { echo "FATAL: $*" >&2; exit 1; }

# ════════════════════════════════════════════════════════
echo "━━━ [1] BUILD ━━━"
# ════════════════════════════════════════════════════════
CC=aarch64-linux-gnu-gcc
SO="-shared -fPIC -nostdlib -O0 -std=c11"

$CC $SO -o sm_stab3.so      sm_stab3.c      || die "sm_stab3"
echo "  ok sm_stab3.so"
$CC $SO -o km_intercept3.so  km_intercept3.c  || die "km_intercept3"
echo "  ok km_intercept3.so"
$CC $SO -o ks2_log.so       ks2_log.c       || die "ks2_log"
echo "  ok ks2_log.so"
$CC $SO -o hwsm_log.so      hwsm_log.c      || die "hwsm_log"
echo "  ok hwsm_log.so"
$CC -static -O0 -o bind_mount bind_mount.c  || die "bind_mount"
echo "  ok bind_mount"

# ════════════════════════════════════════════════════════
echo ""
echo "━━━ [2] ADB CONNECT ━━━"
# ════════════════════════════════════════════════════════
adb kill-server; sleep 1; adb start-server; sleep 1
DEV=$(adb devices | awk 'NR>1 && ($2=="device" || $2=="recovery"){print $1; exit}')
[ -z "$DEV" ] && die "no device found"
echo "  device: $DEV"

# ════════════════════════════════════════════════════════
echo ""
echo "━━━ [3] PUSH ━━━"
# ════════════════════════════════════════════════════════
adb push sm_stab3.so      /tmp/sm_stab3.so      && echo "  ok sm_stab3.so"
adb push km_intercept3.so  /tmp/km_intercept3.so  && echo "  ok km_intercept3.so"
adb push ks2_log.so       /tmp/ks2_log.so       && echo "  ok ks2_log.so"
adb push hwsm_log.so      /tmp/hwsm_log.so      && echo "  ok hwsm_log.so"
adb push bind_mount       /tmp/bind_mount        && echo "  ok bind_mount"
adb shell chmod 755 /tmp/bind_mount

# ════════════════════════════════════════════════════════
echo ""
echo "━━━ [4] MOUNTS ━━━"
# ════════════════════════════════════════════════════════
# Kill any init-spawned instances first; they would fight ours for the
# context-manager role. servicemanager needs a triple kill to outrun init's
# auto-restart.
adb shell "stop servicemanager 2>/dev/null; true"
adb shell "sleep 1; kill -9 \$(pidof servicemanager) 2>/dev/null; true"
adb shell "sleep 2; kill -9 \$(pidof servicemanager) 2>/dev/null; true"
adb shell "kill -9 \$(pidof android.hardware.security.keymint@3.0-service.mitee) 2>/dev/null; true"
adb shell "kill -9 \$(pidof keystore2) 2>/dev/null; true"
adb shell "kill -9 \$(pidof hwservicemanager) 2>/dev/null; true"
adb shell "sleep 2"

# Mount the system_a image read-only. NB: the erofs image has a `system/`
# subdirectory, so binaries live under /mnt_sys/system/bin, libs under
# /mnt_sys/system/lib64, etc.
adb shell "mkdir -p /mnt_sys"
adb shell "mountpoint -q /mnt_sys || mount -t erofs -o ro /dev/block/mapper/system_a /mnt_sys"
adb shell "mountpoint /mnt_sys" && echo "  ok /mnt_sys" || die "/mnt_sys mount failed"

# Bind the system VINTF dir + libbinder over TWRP's, so the stock binaries see
# a consistent ABI.
adb shell "umount /system/etc/vintf 2>/dev/null; true"
adb shell "umount /system/lib64/libbinder.so 2>/dev/null; true"
adb shell "umount /system/lib64/libbinder_ndk.so 2>/dev/null; true"

adb shell "mount --bind /mnt_sys/system/etc/vintf /system/etc/vintf" && echo "  ok vintf bind"
adb shell "mount --bind /mnt_sys/system/lib64/libbinder.so /system/lib64/libbinder.so" && echo "  ok libbinder"
adb shell "mount --bind /mnt_sys/system/lib64/libbinder_ndk.so /system/lib64/libbinder_ndk.so" && echo "  ok libbinder_ndk"

# Create a fresh private binderfs and point /dev/binder at it.
adb shell "mkdir -p /dev/newbfs"
adb shell "mountpoint -q /dev/newbfs || mount -t binder -o max=1048576 none /dev/newbfs"
adb shell "ls /dev/newbfs/" && echo "  ok newbfs"

adb shell "/tmp/bind_mount /dev/newbfs/binder /dev/binderfs/binder" && echo "  ok binder redirect"

# APEX linker symlink — MANDATORY, else every /mnt_sys/system/bin/* binary
# fails with "No such file or directory" (its interpreter is the APEX linker).
adb shell "mkdir -p /apex/com.android.runtime/bin"
adb shell "ln -sf /system/bin/linker64 /apex/com.android.runtime/bin/linker64 2>/dev/null; true"
adb shell "mkdir -p /system/bin/bootstrap"
adb shell "ln -sf /system/bin/linker64 /system/bin/bootstrap/linker64 2>/dev/null; true"
adb shell "setprop apexd.status activated 2>/dev/null; true"

# Provide /apex/com.android.vintf so libvintf doesn't ELOOP on the APEX paths.
adb shell "mkdir -p /apex/com.android.vintf/etc/vintf"
adb shell "cp /vendor/etc/vintf/manifest.xml /apex/com.android.vintf/etc/vintf/"
echo "  ok apex vintf"

# ════════════════════════════════════════════════════════
echo ""
echo "━━━ [5] START THE STACK ━━━"
# ════════════════════════════════════════════════════════
LLP="/mnt_sys/system/lib64:/vendor/lib64"

# ── servicemanager ─────────────────────────────────────
adb shell "LD_PRELOAD=/tmp/sm_stab3.so LD_LIBRARY_PATH=$LLP \
    /mnt_sys/system/bin/servicemanager >/tmp/sm3.log 2>&1 &"
echo "  [SM] started"

# Wait until the SM becomes context manager (by LOG, not by prop — TWRP's init
# already sets servicemanager.ready, so the prop is a false signal here).
echo "  Waiting for 'became CM'..."
for i in $(seq 1 90); do
    adb shell 'grep -q "became CM" /tmp/sm3.log 2>/dev/null' && echo "  SM became CM in ${i}s" && break
    sleep 1
done
adb shell 'setprop servicemanager.ready true; setprop sys.boot_completed 1'

# ── hwservicemanager ────────────────────────────────────
adb shell "LD_PRELOAD=/tmp/hwsm_log.so LD_LIBRARY_PATH=$LLP \
    /system/bin/hwservicemanager >/tmp/hwsm.log 2>&1 &"
echo "  [hwSM] started"
sleep 2

# ── keymint ────────────────────────────────────────────
adb shell "LD_PRELOAD=/tmp/km_intercept3.so LD_LIBRARY_PATH=$LLP \
    /vendor/bin/hw/android.hardware.security.keymint@3.0-service.mitee \
    >/tmp/km.log 2>&1 &"
echo "  [KM] started"

# Wait until keymint has registered all 4 of its AIDL services.
echo "  Waiting for 4 addService from keymint..."
for i in $(seq 1 30); do
    count=$(adb shell 'grep -c "addService.*-> OK" /tmp/km.log 2>/dev/null' | tr -d '\r')
    [ "${count:-0}" -ge 4 ] && echo "  keymint: all 4 services in ${i}s" && break
    sleep 1
done
adb shell 'grep "addService" /tmp/km.log'

# Re-assert the props in case something reset them.
adb shell 'setprop servicemanager.ready true; setprop sys.boot_completed 1'

# ── gatekeeper (REQUIRED — without it TWRP crashes) ────
adb shell "LD_PRELOAD=/tmp/km_intercept3.so LD_LIBRARY_PATH=$LLP \
    /vendor/bin/hw/android.hardware.gatekeeper-service.mitee \
    >/tmp/gk.log 2>&1 &"
echo "  [GK] started"
sleep 3
adb shell 'setprop servicemanager.ready true; setprop sys.boot_completed 1'

# ── keystore2 ──────────────────────────────────────────
adb shell "kill -9 \$(pidof keystore2) 2>/dev/null; true"
adb shell "sleep 1"
adb shell "rm -rf /tmp/misc/keystore; mkdir -p /tmp/misc/keystore"
adb shell "LD_PRELOAD=/tmp/ks2_log.so LD_LIBRARY_PATH=$LLP \
    /mnt_sys/system/bin/keystore2 /tmp/misc/keystore \
    >/tmp/ks2_debug.log 2>&1 &"
echo "  [KS2] started"

echo ""
echo "Waiting 40 seconds..."
sleep 40

# ════════════════════════════════════════════════════════
echo ""
echo "━━━ SUMMARY ━━━"
# ════════════════════════════════════════════════════════

echo ""
echo "=== PROCESSES ==="
adb shell "ps -A | grep -E 'hwservice|keymint|keystore|servicemanager'"

echo ""
echo "=== SM: VINTF ==="
adb shell "grep -E 'FORCING|fetchDevice|BLOCKED' /tmp/sm3.log | head -5"

echo ""
echo "=== KS2: key events ==="
adb shell "grep -E 'KS2 ADD|ABORT|Cannot connect|panic|Keymint ready|registered' /tmp/ks2_debug.log"

echo ""
echo "=== Is the service registered? ==="
adb shell "service check android.system.keystore2 2>/dev/null || echo 'not found'"

echo ""
echo "=== Tail of ks2_debug.log ==="
adb shell "tail -20 /tmp/ks2_debug.log"
