# Contributing

**This project is looking for a new maintainer — and you don't have to take it
over to help.** Fork it, poke at it, push it one step further, and send a PR.
Every bit of progress is welcome, however small.

The original maintainer no longer owns the device, so the repo is handed to the
community. There's a real prize here: **nobody has a fully working FBE `/data`
decrypt in recovery for this SoC family yet.** The hard 90% is done and
documented — the keystore stack comes up, the TEE answers, `keystore2` registers
and TWRP reaches the decrypt code. It dies a couple of steps before
`dm-default-key`. Crack that and you've got the first working TWRP decrypt for
Xiaomi 14T (`degas`) and likely a template for its MediaTek siblings.

## Start here

1. Read the **[README](README.md)** for the big picture.
2. Read **[`docs/RESEARCH.md`](docs/RESEARCH.md)** — what works, the exact
   failure point, and three concrete hypotheses (A/B/C) for the current crash.
3. Read **[`decrypt/README.md`](decrypt/README.md)** — what every shim and
   script does and why. The "Shared techniques" section gets you reading the
   code in five minutes.

## Ways to help (pick your level)

**🟢 Easy / no device-internals needed**
- Build the device tree on a fresh setup and report what breaks — fix the docs
  where reality differs.
- Test the recovery on your own `degas` unit and file a short report (what
  works, screenshots, logs).
- Improve documentation, comments, or the build instructions.

**🟡 Medium — you have the device + ADB**
- Run `decrypt/from_scratch.sh` and attach the logs.
- **Capture the `RA=` (return address)** printed by `twrp_fix.c` on the second
  `std::terminate` — this single value identifies which library throws and is
  the most valuable thing you can contribute right now (see
  [Next steps](docs/RESEARCH.md#next-steps)).
- Confirm whether `[DM …]` log lines appear — i.e. whether decrypt reaches
  `dm-default-key` at all.

**🔴 Hard — the actual decrypt blocker**
- Chase down hypothesis **A** (HWComposer DRM ioctls), **B** (uncaught C++
  exception in `libfscrypttwrp.so` / `libvold.so`), or **C** (`dm-default-key`
  unavailable) from `docs/RESEARCH.md`.
- Get `dm-default-key` programmed and `/data` mounted. 🎉
- Re-enable MTP / USB-OTG without destabilising ADB (see why they're disabled in
  the README).
- Haptics, or porting the approach to a sibling MediaTek device.

## Working on the code

- **Build the shims:** `aarch64-linux-gnu-gcc -shared -fPIC -nostdlib -O0 -std=c11`
  (a `bash decrypt/from_scratch.sh` does this for you). The
  "noreturn function returns" warnings are expected.
- **Match the existing style.** The shims are deliberately minimal: raw AArch64
  syscalls, no libc, the shared `wr()` logger, hooks that "log → tweak →
  forward" via `dlsym(RTLD_NEXT, …)`. Keep new hooks in that idiom and tag your
  log lines (`[SM …]`, `[KS2 …]`, …) so they grep cleanly.
- **Document as you go.** If you add or change a hook, update the file's header
  comment and, if it changes the procedure, `docs/RESEARCH.md`.
- **Don't hook non-trivial C++ ABI symbols** (see the warning at the bottom of
  `RESEARCH.md`) — it crash-loops.

## Submitting a pull request

1. Fork the repo and branch off `main`.
2. Make your change; keep commits focused and the message explaining the *why*.
3. **Include evidence**: relevant log snippets, the command you ran, and what
   changed in behaviour (e.g. "decrypt now reaches `[DM] nr=0x…`").
4. Open the PR against `main`. Draft PRs for work-in-progress are very welcome —
   even a dead end is useful if it rules out a hypothesis.

No contribution is too small, and "I tried X and it didn't work, here's the log"
is a genuinely valuable PR or issue in a reverse-engineering project like this.

## A note on safety

This is experimental recovery software. Flashing a recovery or running these
scripts can brick a device or wipe data. Test on a unit you can afford to
recover, keep backups, and understand what a command does before running it.
Everything here is provided as-is, with no warranty (see [LICENSE](LICENSE)).

---
Questions or want to take the project further? Open an
[issue](../../issues) or a [discussion](../../discussions). Thanks for helping
push this over the finish line. 🙏
