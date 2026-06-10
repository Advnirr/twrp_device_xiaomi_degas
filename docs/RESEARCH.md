# FBE decrypt research log — Xiaomi 14T (`degas`)

This is the detailed engineering log behind the `decrypt/` directory: how the
keystore stack is brought up inside recovery, what works, and exactly where —
and why — it ultimately hits a hardware wall. It is the English, cleaned‑up
version of the working notes I kept while reverse‑engineering the FBE decrypt
path.

**TL;DR:** the entire software chain works (binder context takeover →
servicemanager bridge → keystore2 → KeyMint), but the stored metadata key is
bound to the **Root of Trust** of normal boot. A custom recovery presents a
different RoT, so the TEE refuses the blob with `INVALID_KEY_BLOB`. This is
enforced in the secure world and cannot be bypassed from Android — see
[Why it can't work from recovery](#why-it-cant-work-from-recovery-root-of-trust).

If you only want the overview, read the main [`README.md`](../README.md) first.

---

## Goal

`twrp decrypt <pin>` → unwrap the FBE Class key and mount `/data` on Xiaomi 14T
(`degas`, `mt6897`, HyperOS 3 / Android 16). Userdata is F2FS with
`fileencryption=aes-256-xts` plus metadata encryption via `dm-default-key`
(`keydirectory=/metadata/vold/metadata_encryption`).

**Host:** Arch Linux (CachyOS), cross compiler `aarch64-linux-gnu-gcc`.
**On device:** everything runs from `/tmp/`.

---

## Solution architecture

We don't patch TWRP's C++ crypto. Instead we run the **stock Android binaries**
(servicemanager + keystore2 from the `system_a` image, keymint + gatekeeper from
the `vendor` partition) inside recovery, each wrapped with a small
`LD_PRELOAD` shim that stubs out the checks that fail under recovery and traces
everything. The chain we are trying to complete:

```
recovery_real
  → AServiceManager_waitForService("android.system.keystore2.IKeystoreService/default")
    → keystore2 (Rust, /mnt_sys/system/bin/keystore2)
      → keymint binder → TEE (MITEE: TEEC_OpenSession → 0x0, works)
      → keystore2 registers android.system.keystore2.IKeystoreService/default
  → gatekeeper.verify(pin) → auth token
  → CE key → dm-default-key → mount /data
```

**Binder devices**
- `/dev/binder` → symlink → `/dev/binderfs/binder` → bind‑mounted from our
  private `/dev/newbfs/binder`
- `/dev/hwbinder` → hwservicemanager
- `/dev/vndbinder` → vndservicemanager

---

## The shims (`decrypt/*.c`)

Each file's header comment documents its hooks; summary:

| Shim | Target process | Key patches |
|---|---|---|
| `sm_stab3.c` | `servicemanager` | VINTF stability → pass; `fetchDeviceHalManifest` ELOOP → force OK; retry `BINDER_SET_CONTEXT_MGR`; block `servicemanager.ready=false`; redirect `/dev/binder`→newbfs |
| `hwsm_log.c` | `hwservicemanager` | retry CM on `/dev/hwbinder`; trace VINTF; trace opens |
| `km_intercept3.c` | keymint + gatekeeper HALs | VINTF stability → pass; trace `addService` / TEEC; `joinThreadPool` → start pool first; abort → thread‑exit |
| `ks2_log.c` | `keystore2` | apexd/strongbox lookups → immediate NULL; `isDeclared` → force true for keymint/sharedsecret/gatekeeper; probe keymint via WFS; abort → thread‑exit |
| `twrp_fix.c` | `recovery_real` (TWRP) | terminate/abort/SIGABRT → **thread‑only** exit (SYS_exit 93, not exit_group 94); fake F2FS ioctls; trace DM ioctls + WFS |
| `throw_logger_alt.c` | `recovery_real` | alternate of `twrp_fix.c` that dumps a frame‑pointer backtrace from abort() |
| `bind_mount.c` | helper | static `mount(--bind)` binary |

Implementation notes that matter:
- Compiled `-nostdlib`: syscalls are issued directly; there is no errno.
- `abort()` hook uses **SYS_exit (93)** = kill only the calling thread;
  **SYS_exit_group (94)** would kill the whole process.
- F2FS ioctls have type byte `0xf5` (`(req>>8)&0xff`); DM ioctls type `0xfd`;
  HWComposer DRM ioctls type `0x64` (`'d'`).

---

## Runbook

The keystore-stack bring‑up is automated in
[`decrypt/from_scratch.sh`](../decrypt/from_scratch.sh). The steps **the script
actually performs**, in order:

1. **Build** the shims (`sm_stab3`, `km_intercept3`, `ks2_log`, `hwsm_log`) with
   `aarch64-linux-gnu-gcc -shared -fPIC -nostdlib -O0`, and `bind_mount` static;
   then push them to `/tmp`.
2. **Kill** any init‑spawned `servicemanager` / `keymint` / `keystore2` /
   `hwservicemanager` (the SM gets a triple‑kill to outrun init's auto‑restart).
3. **Mounts**:
   - `mount -t erofs -o ro /dev/block/mapper/system_a /mnt_sys` (note: the erofs
     image has a `system/` subdir, so binaries are at `/mnt_sys/system/bin/…`).
   - bind `/mnt_sys/system/etc/vintf`, `libbinder.so`, `libbinder_ndk.so`.
   - create private binderfs `/dev/newbfs`, then bind `/dev/newbfs/binder` over
     `/dev/binderfs/binder`.
   - **APEX linker symlink (mandatory):**
     `ln -sf /system/bin/linker64 /apex/com.android.runtime/bin/linker64`
     — without it every `/mnt_sys/system/bin/*` binary fails with “No such file
     or directory” (its interpreter is the APEX linker).
   - copy a VINTF manifest into `/apex/com.android.vintf/etc/vintf/`.
4. **Start servicemanager**, then **wait for `"became CM"` in the log** — *not*
   the `servicemanager.ready` prop, which TWRP's init already sets (false
   signal). Then set the props true yourself.
5. **hwservicemanager**, then **keymint** (wait for 4 `addService -> OK`), then
   **gatekeeper** (required — TWRP crashes without it), then **keystore2**, and
   print whether `keystore2` registered.

Then run `twrp decrypt <pin>` yourself.

Two parts of the broader procedure that `from_scratch.sh` does **not** do —
handle them separately:

- **Deploy `twrp_fix.so`** (the recovery shim). In the image‑baked flow it is
  preloaded automatically by the wrapper that `patch_touch.sh` installs (as
  `/system/bin/throw_logger.so`). To (re)deploy it live, you must replace the
  file *and* restart `recovery_real` so it reloads the preload:
  ```bash
  aarch64-linux-gnu-gcc -shared -fPIC -nostdlib -O0 -std=c11 -o twrp_fix.so twrp_fix.c
  adb shell 'stop recovery'; sleep 3
  adb push twrp_fix.so /system/bin/throw_logger.so
  adb shell 'start recovery'; sleep 8
  ```
  Bring the stack up with `from_scratch.sh` **before** the start‑up auto‑decrypt
  matters (see the note in [The wall](#the-wall--begin-is-rejected-by-the-tee--33-invalid_key_blob)).
- **Device‑mapper control node.** If `/dev/mapper/control` is missing, create it:
  `mknod /dev/mapper/control c $MAJ $MIN` with `MAJ:MIN` read from
  `/sys/class/misc/device-mapper/dev`.

### Props set during bring‑up
```
setprop servicemanager.ready true
setprop sys.boot_completed 1
setprop apexd.status activated
```

---

## What works ✓

1. **TEE (MITEE)**: `TEEC_OpenSession → 0x0`, `TEEC_InvokeCommand → 0x0`.
2. **servicemanager** comes up with the ELOOP→OK force for VINTF.
3. **All 4 keymint AIDL services** register in the SM.
4. **gatekeeper** (MITEE) registers.
5. **keystore2** registers 6 services including `IKeystoreService/default`.
6. **keystore2 ↔ keymint link**: PROBE_WFS → FOUND, ASSOC → true, KeyMint
   version 300.
7. **TWRP finds keystore2** via the servicemanager bridge (`[TWRP WFS] bridge
   FOUND`) — no NULL, no SIGABRT.
8. **The full unwrap chain runs**: `getSecurityLevel` → SecurityLevel binder
   recovered (`[RSB-FIX] recovered handle=2`) → `createOperation` → KeyMint
   `begin(DECRYPT, keymaster_key_blob, …)` is actually invoked.
9. **Fresh KeyMint keys work in recovery** — `keystore_cli_v2 generate
   --seclevel=tee` + `sign-verify` → *Sign 256 bytes, Verify OK*. The TEE is
   healthy; see [The wall](#the-wall--begin-is-rejected-by-the-tee--33-invalid_key_blob).
10. **device‑mapper** is supported by the kernel; `/dev/mapper/control` created.

---

## Key findings

1. **“Atomic commit failed ret=-22” is HWComposer, not FBE.** It comes from
   `/vendor/lib64/hw/hwcomposer.mtk_common.so` (a DRM display atomic commit), not
   F2FS or DM. The display thread was crashing and, via `exit_group`, killing the
   whole decrypt process. → fixed by switching the abort path to **thread‑only
   exit** in `twrp_fix.c`.
2. **thread‑exit instead of exit_group is the breakthrough.** With it, decrypt
   runs several steps further (spinner + partition scan), proving TWRP is doing
   real work inside.
3. **`/mnt_sys/system/bin/servicemanager` exists** (437 binaries in there); the
   “No such file or directory” was the missing **APEX linker symlink**, not a
   wrong path.
4. **`servicemanager.ready` is a false signal after reboot** — TWRP's init sets
   it. Wait for `"became CM"` in the SM log instead.
5. **keystore2 DB (86016 bytes) has empty tables.** `keyentry` and `blobentry`
   are present but empty — the FBE metadata key lives in `/metadata/vold/`, not
   in the keystore2 SQLite, so this is expected.
6. **Decrypt forks a `[recovery]` child.** With `exit_group` that child dies
   immediately; with thread‑exit it keeps working.

---

## The wall — `begin()` is rejected by the TEE (`-33 INVALID_KEY_BLOB`)

Once the whole stack is up (servicemanager → keymint → keystore2) **and TWRP is
restarted so its start‑up auto‑decrypt runs against our stack**, the metadata
decrypt path reaches all the way into KeyMint and dies there:

```
[TWRP WFS] bridge FOUND
[RSB-FIX] recovered handle=2
I:Unable to decrypt metadata encryption
I:FBE setup failed. Trying FDE...
```
and in `keystore2`:
```
security_level.rs:374: Failed to begin operation.
  0: security_level.rs:924: upgrade_keyblob_if_required_with(key_id=None)
  2: Error::Km(r#INVALID_KEY_BLOB)
```

`vold`'s `KeyStorage` unwraps the metadata key by running the 219‑byte
`keymaster_key_blob` through KeyMint `begin(DECRYPT, …)`. KeyMint returns
**ErrorCode `-33` = `INVALID_KEY_BLOB`**: the TEE cannot decrypt/authenticate the
blob at all. (Note this is *not* `-62 KEY_REQUIRES_UPGRADE`, which is what a pure
OS/patch‑level mismatch would produce.)

> The manual `twrp decrypt <pin>` CLI is a *cached* early‑bail after the first
> failure (`E:Error retrieving decrypted data block device.`, no service lookup).
> The real, full attempt is the one TWRP runs automatically at start‑up — read it
> from `/tmp/recovery.log` (`bridge FOUND` … `Unable to decrypt metadata`). So the
> stack must already be up *before* `start recovery`.

## Things that were ruled out

KeyMint derives a key blob's wrapping key (KEK) from
`f(device_master_key, APPLICATION_ID, APPLICATION_DATA, ROOT_OF_TRUST)`, plus a
version check on top. Each fixable input was tested and eliminated:

1. **KeyMint itself is healthy.** Using `keystore_cli_v2` in recovery,
   `generate --seclevel=tee` + `sign-verify` → *Sign 256 bytes, Verify OK*. A key
   **created in recovery works in recovery** — the TEE/MITEE trustlet is fully
   functional. Only the *stored* blob is rejected.
2. **OS version / OS patch‑level — not the cause.** Recovery's ramdisk reports
   stale props (`release=12`, `security_patch=2022-04-05`). We hooked
   `__system_property_find`/`_read_callback`/`_get` in `km_intercept3.c` to feed
   KeyMint the real values (`release=16`, `2026-02-01`); confirmed it read them.
   `-33` was unchanged. (And a patch‑level mismatch would be `-62`, not `-33`.)
3. **`APPLICATION_ID` — verified byte‑for‑byte correct.** We dumped the exact
   `begin()` parcel `keystore2` sends to KeyMint (ioctl hook in `ks2_log.c`,
   matched by the blob signature) and parsed it: it carries the raw 219‑byte blob
   and `APPLICATION_ID` (tag `0x90000259`) = a 64‑byte value. Computed
   independently, `vold`'s canonical scheme
   `SHA512( "Android secdiscardable SHA512".resize(128,'\0') + secdiscardable )`
   produces the **identical** 64 bytes. So `libfscrypttwrp` builds the app‑id
   exactly like the `vold` that created the key (same `secdiscardable` file →
   same value). `APPLICATION_DATA` is not used (also correct).
4. **The blob is forwarded unmodified.** The 219 bytes in the parcel match the
   on‑disk `keymaster_key_blob` byte‑for‑byte; `keystore2` does not mangle it.

## Why it can't work from recovery (Root of Trust)

With the device master key, `APPLICATION_ID`, `APPLICATION_DATA` and OS
patch‑level all accounted for, the **only** remaining KEK input that differs
between normal boot and TWRP is the **Root of Trust** —
`{verifiedBootKey, deviceLocked, verifiedBootState, verifiedBootHash}` — and, on
some MTK trustlets, the boot‑image‑derived `BOOT_PATCHLEVEL` folded into the KEK.

Both are latched into the TEE by the bootloader over a secure channel **before**
the HLOS starts, based on the image it actually booted. Booting a custom TWRP
image presents the trustlet a different Root of Trust than the stock boot image
does, so the metadata KEK — bound at creation time to the normal‑boot RoT — can
no longer be reproduced. The TEE therefore refuses the blob with
`INVALID_KEY_BLOB`. This is exactly why the *same* key unwraps fine in normal
boot (matching RoT) and is rejected in recovery.

This binding is enforced inside the secure world and is not reachable from the
HLOS. **Conclusion: on this device, decrypting `/data` from a custom recovery is
not achievable.** The entire software chain (binder context takeover, the
servicemanager bridge, keystore2, KeyMint, fresh‑key crypto) works; the wall is
purely the hardware‑enforced RoT binding. The only theoretical bypass would be
patching the version/RoT check *inside* the MITEE trustlet, which lives in the
secure world and cannot be touched from Android. The data itself remains
accessible the normal way: by booting the device normally (where it decrypts).

---

## Known system quirks

- `dm_default_key` is not exposed as a module (`/sys/module/dm_default_key`
  absent) — likely built into the kernel. Not verified with `dmsetup` (the tool
  isn't present).
- `xts(aes)` is listed in `/proc/crypto`, so the cipher is available.
- TWRP logs `Could not find obj_id = 39` / `= 59` — it queries the keystore2
  SQLite directly as a fallback; the tables are empty so it doesn't find them.
  Unclear whether this is fatal.
- keystore2 watchdog: a thread used to hang on `IApexService`. Fixed in
  `ks2_log.c` by returning NULL immediately for apexd/strongbox lookups.
- After many restarts you get duplicate `hwservicemanager`/keymint processes —
  `kill -9` the old PIDs before starting new ones (the scripts do this).

## Earlier-session reference data

`/metadata/vold/metadata_encryption/key/` contained:
`encrypted_key` (92 B), `keymaster_key_blob` (219 B), `secdiscardable`
(16384 B), `version = 0x31` (`'1'`).

**Do not hook the C++ `BpServiceManager::getService` / `ServiceManagerShim`
overloads.** They return non‑trivial types (`sp<IBinder>`, `binder::Status`)
through a hidden pointer in `x0`; getting the ABI wrong causes an immediate
crash loop. This was tried and reverted.
