# /data decryption on Xiaomi 14T (`degas`)

Notes on how FBE decryption works in this recovery, why the first attempt
failed, and why its conclusion was wrong.

Status: the stack below is in the tree since v1.1. It comes from
[sigmapro555eshkere's OrangeFox port](https://github.com/notnotshuts/ofox_device_xiaomi_degas),
where decryption is reported working (OrangeFox, fox_16.0). This tree builds it
on TWRP twrp-14.1 and has **not been tested on a device yet**.

## Encryption on this device

- `userdata`: F2FS, FBE v2 (`aes-256-xts:aes-256-cts:v2+inlinecrypt_optimized`).
- Metadata encryption through `dm-default-key`, key directory
  `/metadata/vold/metadata_encryption`. The key dir holds `encrypted_key` (92 B),
  `keymaster_key_blob` (219 B), `secdiscardable` (16384 B) and `version`.
- Keys are wrapped by KeyMint 3.0 running as a trusted app in MiTEE (Xiaomi's
  TEE on MT6897). User credential (CE) keys also go through Gatekeeper and
  Weaver; Weaver lives in the NXP eSE.

## How it works now

Everything runs from the recovery ramdisk, stock binaries from HyperOS
`OS3.0.306.0.WNEEUXM`, started by `init.recovery.mt6897.rc`:

1. `on fs`: mount `persist` **read-write** at `/mnt/vendor/persist`, as stock
   does. MiTEE keeps its REE-FS store there (`/mnt/vendor/persist/data`:
   `dirf.db` and objects, including KeyMint's secure deletion secrets). Fix
   owners of `/dev/tee0`, `/dev/teepriv0`, `/dev/rpmb0` and the UFS RPMB nodes.
   Set `vendor.teefs_state=ready`.
2. When TWRP sees `ro.crypto.state=encrypted`, `degas_prepdecrypt.sh` mounts
   the active slot's system and vendor read-only and copies
   `ro.build.version.release`, `ro.build.version.security_patch` and
   `ro.vendor.build.security_patch` from their build.prop with `resetprop`. Then
   it sets `crypto.ready=1`. If anything fails, the TEE stack is not started.
3. On `crypto.ready=1`: `tee-supplicant`, KeyMint
   (`android.hardware.security.keymint@3.0-service.mitee`), Gatekeeper one
   second later (it races KeyMint over the HAT key otherwise, error `-26` on
   duchamp).
4. Weaver chain for CE keys: `insmod nxp_i2c.ko p73.ko` (eSE driver, normally in
   vendor_dlkm), the Xiaomi secure element HAL, `se_omapi_recovery` (a small
   native OMAPI service in `omapi/`, standing in for the Java SecureElement
   app), and `android.hardware.weaver-service.nxp`.
5. TWRP's own vold/keystore2 code (`TW_INCLUDE_CRYPTO_FBE`,
   `TW_INCLUDE_FBE_METADATA_DECRYPT`) does the rest.
6. Once all users are decrypted the services are stopped.

Other details that matter:

- `/metadata` is mounted read-only. If KeyMint asks for a key upgrade, vold
  would rewrite the blob, and Android could lose access to it.
- That is also why the patch levels come from the installed system and
  `PLATFORM_SECURITY_PATCH` is not faked. A level higher than Android's makes
  KeyMint reject the blob or request an upgrade.
- `TW_SKIP_ADDITIONAL_FSTAB`: otherwise TWRP reads the vendor fstab and its
  `/metadata` options (rw) override ours.
- TWRP gets its own read-only mount of persist (`/mnt/vendor/persist_bak`) for
  backups, so it never remounts the TEE store.
- logd/logcat are included: crypto pulls in libsysutils, and keystore2 logs only
  to logd.
- The eSE HAL needs `libclang_rt.ubsan_standalone`, which is copied from the
  build's clang.
- Stock kernel modules (`vendor_ramdisk/`) are shipped into the vendor ramdisk
  directly, so the build output is flashable without a repack.

## The first attempt and why it stopped

v1.0 did not use TWRP's crypto. Instead the stock `servicemanager` and
`keystore2` from `system_a`, plus KeyMint and Gatekeeper from vendor, were run
inside recovery under `LD_PRELOAD` shims that patched VINTF checks, binder
context manager races, apexd lookups and aborts. That got the whole chain up:
keystore2 registered, TWRP found it, and KeyMint `begin()` was called on the
metadata key blob. The TEE answered `-33 INVALID_KEY_BLOB`.

What was ruled out back then:

- KeyMint itself was fine: a key generated in recovery could sign and verify.
- OS version and patch level were fed from the real system (a mismatch would
  give `-62 KEY_REQUIRES_UPGRADE` anyway).
- `APPLICATION_ID` was dumped from the parcel and matched vold's
  `SHA512("Android secdiscardable SHA512" padded to 128 + secdiscardable)`.
- The 219-byte blob reached KeyMint unchanged.

The conclusion was that the key is bound to the Root of Trust of normal boot
and that a custom recovery presents a different one, so decryption is
impossible. **That was wrong.**

## What was actually missing

`tee-supplicant` was never started, and `persist` was not mounted where the TEE
expects it. `TEEC_OpenSession` worked, so the TEE looked healthy, but the
KeyMint TA had no access to its storage on disk.

That explains every observation:

- A new key gets created without a secure deletion slot when storage is not
  available, so fresh keys worked.
- The metadata key was created in Android with a slot. Its KEK depends on the
  secret stored in that slot, so without the secrets file the TA cannot unwrap
  it and returns `INVALID_KEY_BLOB`.

The exact TA logic is inferred from this behaviour and from the OrangeFox tree,
not from reversing the TA.

The Root of Trust theory should have been dropped on its own merits. On this
device recovery is a ramdisk inside `vendor_boot`. Normal boot and recovery
boot go through the same boot, vendor_boot and vbmeta images, and with an
unlocked bootloader the state is orange in both cases. The bootloader has
nothing different to report to the TEE.

Lesson: before calling something a hardware wall, diff the software
environment against stock. Here `ps | grep tee` and `strings` on the stock
`tee-supplicant` would have found it.

## Other notes from the first attempt

- `Atomic commit failed ret=-22` in the log came from the MTK HWComposer, not
  from F2FS or device-mapper.
- The keystore2 database has empty `keyentry`/`blobentry` tables. The metadata
  key lives in `/metadata/vold`, not in keystore2.
- `servicemanager.ready` is set by recovery init and is not a reliable signal.
- System binaries need `/apex/com.android.runtime/bin/linker64`; without it they
  fail with "No such file or directory".

## Testing

1. Back up data, `metadata`, `persist` and both `vendor_boot` slots.
2. Check the firmware: `getprop ro.build.version.incremental` should be
   `OS3.0.306.0.WNEEUXM`, or the modules/blobs need replacing.
3. Flash to the current slot, boot recovery, enter the PIN.
4. Check `/tmp/recovery.log` for `degas_prepdecrypt` values and decrypt
   messages, and `logcat` for `INVALID_KEY_BLOB` or `REQUIRES_UPGRADE`. If an
   upgrade shows up, reboot right away.
5. `ls /data/media/0` should show normal names.
6. Reboot to Android and make sure it still unlocks. That is the real test.

## Credits

- [sigmapro555eshkere](https://github.com/notnotshuts/ofox_device_xiaomi_degas):
  the OrangeFox port and the degas-specific findings (persist as the TEE store,
  eSE/Weaver bring-up, OMAPI service, 306 modules).
- [Dip184](https://github.com/Dip184/android_device_xiaomi_duchamp/tree/orangefox-14.1):
  the MiTEE decryption method on duchamp.
