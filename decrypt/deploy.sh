#!/bin/bash
# deploy.sh — quick redeploy of just servicemanager + keymint (a fast subset of
# from_scratch.sh, for iterating on those two shims without the full bring-up).
set -e
PRE="$(cd "$(dirname "$0")" && pwd)"
echo "[1/4] Push .so files..."
adb push "$PRE/sm_stab3.so"      /tmp/sm_stab3.so
adb push "$PRE/km_intercept3.so" /tmp/km_intercept3.so
adb push "$PRE/ks2_log.so"       /tmp/ks2_log.so
echo "[2/4] Setup on device..."
adb shell '
set -e
mkdir -p /apex/com.android.runtime/bin
ln -sf /system/bin/linker64 /apex/com.android.runtime/bin/linker64 2>/dev/null||true
mkdir -p /system/bin/bootstrap
ln -sf /system/bin/linker64 /system/bin/bootstrap/linker64 2>/dev/null||true

# Kill old instances
stop servicemanager 2>/dev/null||true
sleep 1
kill -9 $(pidof servicemanager) 2>/dev/null||true
kill -9 $(cat /tmp/sm2.pid 2>/dev/null) 2>/dev/null||true
kill -9 $(pidof android.hardware.security.keymint@3.0-service.mitee) 2>/dev/null||true
sleep 3

setprop apexd.status activated 2>/dev/null||true

echo "[3/4] Start System SM..."
LD_PRELOAD=/tmp/sm_stab3.so \
  LD_LIBRARY_PATH=/mnt_sys/system/lib64:/vendor/lib64 \
  /mnt_sys/system/bin/servicemanager >/tmp/sm3.log 2>&1 &
echo $! > /tmp/sm2.pid
sleep 5

echo "[4/4] Start keymint..."
LD_PRELOAD=/tmp/km_intercept3.so \
  LD_LIBRARY_PATH=/mnt_sys/system/lib64:/vendor/lib64 \
  /vendor/bin/hw/android.hardware.security.keymint@3.0-service.mitee \
  >/tmp/km.log 2>&1 &
sleep 8

echo "=== SM log ==="
cat /tmp/sm3.log
echo "=== KM log ==="
grep -E "addService|MITEE|error|Error" /tmp/km.log | head -10
'
