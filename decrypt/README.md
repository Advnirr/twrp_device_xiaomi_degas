# `decrypt/` — FBE decrypt research components

This directory contains the experimental part of the project: a set of small
`LD_PRELOAD` shims (one per process) and orchestration scripts that bring up the
Android keystore stack **by hand inside TWRP** in an attempt to decrypt FBE
`/data`. This file documents each component. For the *theory* — the overall
architecture, what works, where it's stuck and what to try next — see
[`../docs/RESEARCH.md`](../docs/RESEARCH.md).

> TL;DR to run everything: `./from_scratch.sh` then `adb shell 'twrp decrypt <pin>'`.

---

## The idea in one paragraph

We don't modify TWRP's C++ crypto code. Instead we run the **stock Android
binaries** (servicemanager + keystore2 from the `system_a` image, keymint +
gatekeeper from the `vendor` partition) inside recovery, and wrap each one with a
tiny `LD_PRELOAD` library that
overrides exactly the libc/binder symbols that misbehave under recovery — stub
out a failing check here, redirect a device node there, downgrade a fatal abort
into a thread exit, and log everything. Each shim is process-specific so it only
touches the symbols that matter for that binary.

---

## Shared techniques (read this first)

All shims are written the same deliberately‑minimal way, so once you understand
one you understand all of them:

- **Compiled `-nostdlib`.** No libc, no C runtime. We issue Linux syscalls
  directly with inline AArch64 assembly. Consequence: there is **no `errno`** —
  a raw syscall returns `-errno` in `x0`, which is why some shims re-issue a
  syscall just to read the error code.
- **`wr()` logger.** Every file has `static void wr(const char* s)` that writes a
  string to **fd 2 (stderr)** via `SYS_write` (`x8=64`). That's why each process
  is launched with `>/tmp/<name>.log 2>&1`. Helpers `wrint`/`wrhex` format
  numbers. All log lines are tagged (`[SM …]`, `[KS2 …]`, `[KM …]`, `[hwSM …]`,
  `[twrp_fix] …`) so you can grep them out of a shared log.
- **`LD_PRELOAD` + `dlsym(RTLD_NEXT, …)`.** A hook with the same name as a real
  function wins at link time; inside it we resolve the *real* implementation
  with `dlsym((void*)-1, "name")` (that's `RTLD_NEXT`) and call through. So most
  hooks are “log / tweak / forward”.
- **Mangled C++ names.** Some hooks target C++ symbols (e.g.
  `_ZN7android8internal9Stability…`). We declare them with their mangled name so
  the linker overrides the real method. ⚠️ Only hook C++ functions with
  **trivial** signatures — see the warning at the bottom of `RESEARCH.md` about
  hidden-return-pointer ABI crashes.
- **`/dev/binder` redirect.** Several shims rewrite `open()/openat()` paths
  `/dev/binder` and `/dev/binderfs/binder` → `/dev/newbfs/binder`, our private
  binderfs where our hand-started servicemanager is the context manager.
- **abort/exit policy.** `SYS_exit` (93) kills only the **calling thread**;
  `SYS_exit_group` (94) kills the **whole process**. Turning aborts into
  thread-only exits is what keeps decrypt alive — see `twrp_fix.c`.

---

## The shims

### `sm_stab3.c` → `servicemanager`
Makes the stock Service Manager come up on our private binderfs and become the
context manager. Hooks:
- `Stability::requiresVintfDeclaration` → `false`, `Stability::check` → `0`
  (let unblessed services register).
- `VintfObject::fetchDeviceHalManifest`: returns `ELOOP (-40)` in recovery
  because APEX VINTF overlays loop; we **force the result to 0** and run on the
  vendor manifest (which already lists keymint).
- `ioctl(BINDER_SET_CONTEXT_MGR)`: retry up to 120×500 ms (races init's dying
  SM), then immediately publish `servicemanager.ready=true`.
- `__system_property_set("servicemanager.ready","false")`: **blocked** — the SM
  tries to clear it at startup, which would make every client think no SM exists.
- `abort()` → spin (`wfi`).
- `open/openat/stat/lstat`: redirect `/dev/binder`, and log any `ELOOP` so you
  can find which path the VINTF loader chokes on.

### `hwsm_log.c` → `hwservicemanager`
The HIDL counterpart of the above (keymint pulls in HIDL deps). Retries
`BINDER_SET_CONTEXT_MGR` on `/dev/hwbinder` (60×100 ms), spins on `abort()`,
traces VINTF and interesting opens (incl. `/dev/hw*`) with precise errno.

### `km_intercept3.c` → keymint **and** gatekeeper HALs
Used for both:
`android.hardware.security.keymint@3.0-service.mitee` and
`android.hardware.gatekeeper-service.mitee`.
- Same `Stability::*` stubs (they load the system libbinder too).
- Traces `AServiceManager_addService` (expect **4 OK** from keymint).
- Traces `TEEC_OpenSession` / `TEEC_InvokeCommand` → confirms the MITEE trusted
  app responds (result `0x0`).
- `ABinderProcess_joinThreadPool`: the stock service only *joins*; we
  **start the pool first** so the HAL actually has binder threads (needed for
  `linkToDeath`).
- `abort()` → thread-only exit; `/dev/binder` redirect.

### `ks2_log.c` → `keystore2`
The service TWRP ultimately talks to. Without help it hangs forever on services
that don't exist in recovery.
- `is_nonexistent_in_recovery()`: `waitForService()` for **apexd / strongbox**
  returns **NULL immediately** (otherwise the worker thread blocks forever; also
  avoids a multi-minute watchdog freeze on `IApexService`).
- `AServiceManager_isDeclared` → **forced `true`** for keymint / sharedsecret /
  gatekeeper, so keystore2 actually tries to connect to the HALs we started.
- `forEachDeclaredInstance`: after the real call, actively `waitForService` the
  `"<keymint iface>/default"` and log it — this is the `[KS2 PROBE_WFS] -> FOUND`
  line that proves keystore2 can reach keymint.
- Traces `waitForService` / `checkService` / `getService` / `addService` and
  `AIBinder_associateClass` / `getDescriptor`.
- `abort()` → thread-only exit; `/dev/binder` redirect.

### `twrp_fix.c` → `recovery_real` (the TWRP binary) ★
The key shim. Pushed to `/system/bin/throw_logger.so` and preloaded into TWRP.
- **`std::terminate` / `abort` / `raise(SIGABRT)` / `kill(SIGABRT)` → exit the
  current thread only** (`SYS_exit` 93). A crashing display/worker thread no
  longer takes the whole decrypt process down. This single change is what lets
  decrypt progress past the partition scan. `std::terminate` also logs the
  return address (`RA=…`) so you can identify which library threw.
- `ioctl`: **fake-success** for F2FS ioctls (type `0xf5`) that fail in recovery;
  **trace** device-mapper ioctls (type `0xfd`) — the `[DM …]` lines tell you
  whether decrypt reached `dm-default-key`.
- Traces `AServiceManager_waitForService` / `_checkService` and successful
  `open()`s.

### `throw_logger_alt.c` → `recovery_real` (alternate)
An earlier variant of `twrp_fix.c`. The difference worth keeping: its `abort()`
walks the AArch64 frame-pointer chain (`x29`) and dumps up to 12 return
addresses **before** exiting — a cheap stack backtrace when you don't yet know
who called abort. Both compile to the same on-device `/system/bin/throw_logger.so`;
`twrp_fix.c` is the current/primary one — use this alt when you need the
backtrace. (Neither is deployed by `from_scratch.sh`; deploy it manually per
[`../docs/RESEARCH.md`](../docs/RESEARCH.md#next-steps), or let `patch_touch.sh`
bake your built `throw_logger.so` into the image.)

### `bind_mount.c`
A 5-line static helper (`mount(--bind)`) because TWRP's toybox `mount` lacks
`--bind` here. Used to splice `/dev/newbfs/binder` over `/dev/binderfs/binder`.

---

## Scripts

### `from_scratch.sh` ★ — full bring-up
The master script. Build the shims → ADB connect → push → set up mounts/binderfs
→ start the stack in order (servicemanager → hwservicemanager → keymint →
gatekeeper → keystore2) → print whether keystore2 registered. Run it, then
`adb shell 'twrp decrypt <pin>'`. Key ordering details (why it waits on log
lines instead of props, the mandatory APEX linker symlink, etc.) are commented
inline and explained in `RESEARCH.md`.

### `deploy.sh` — fast partial redeploy
Rebuilds and restarts **only** servicemanager + keymint, for iterating on those
two shims without the full bring-up.

### `adb`
Thin wrapper around the real `adb` that filters the noisy
`failed to find generated linker configuration` linker warning. Put this
directory on `PATH`, or call `./adb`, so the scripts' output stays readable.

---

## Building & deploying manually

```bash
CC=aarch64-linux-gnu-gcc
SO="-shared -fPIC -nostdlib -O0 -std=c11"
$CC $SO -o sm_stab3.so      sm_stab3.c
$CC $SO -o km_intercept3.so km_intercept3.c
$CC $SO -o ks2_log.so       ks2_log.c
$CC $SO -o hwsm_log.so      hwsm_log.c
$CC $SO -o twrp_fix.so      twrp_fix.c     # → push to /system/bin/throw_logger.so
$CC -static -O0 -o bind_mount bind_mount.c
```
The “noreturn function returns” warnings are expected and harmless. The
compiled `.so`/`bind_mount` are intentionally **not** committed (rebuild from
source); only the `.c`/scripts and the two captured device libs
(`libbinder_sys.so`, `libbinder_ndk.so`, kept for reference) are in git.

## Reading the logs
Each process logs to `/tmp/<name>.log`. Useful greps:
```bash
adb shell 'grep -E "became CM|FORCING|BLOCKED" /tmp/sm3.log'      # SM health
adb shell 'grep "addService" /tmp/km.log'                          # 4 keymint svcs
adb shell 'grep -E "KS2 ADD|PROBE_WFS|registered" /tmp/ks2_debug.log'
adb shell 'grep -E "\[twrp_fix\]|\[DM|\[TWRP" /tmp/recovery.log'   # decrypt path
```
