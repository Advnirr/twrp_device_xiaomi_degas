# TWRP device tree for Xiaomi 14T (`degas`)

![Status: Beta](https://img.shields.io/badge/Status-Beta-yellow.svg)
![License: Apache-2.0](https://img.shields.io/badge/License-Apache--2.0-blue.svg)
![Platform: MT6897](https://img.shields.io/badge/Platform-MT6897-green.svg)

Unofficial TWRP 3.7.1 (twrp-14.1) for the Xiaomi 14T (`degas`, MediaTek
Dimensity 8300-Ultra `mt6897`, HyperOS 3 / Android 16). The recovery lives in
`vendor_boot`.

## Status

Beta. Flashing, raw partition backup/restore, wipe, ADB, sideload, MTP, OTG and
A/B slot switching work.

`/data` decryption is on `main` but **not released and not tested on a device yet**.
It uses the stock MiTEE stack (tee-supplicant, KeyMint 3.0, Gatekeeper, NXP
Weaver) inside recovery. The method comes from
[sigmapro555eshkere's OrangeFox tree](https://github.com/notnotshuts/ofox_device_xiaomi_degas),
which reports working decryption. Why the v1.0 conclusion ("impossible, Root of
Trust") was wrong and how the stack works is in
[docs/RESEARCH.md](docs/RESEARCH.md).

The bundled kernel modules and TEE blobs are from HyperOS
`OS3.0.306.0.WNEEUXM`. On other firmware the modules may not match your kernel.

## Device

| | |
|---|---|
| Model | Xiaomi 14T |
| Codename | `degas` |
| SoC | MediaTek Dimensity 8300-Ultra (`mt6897`) |
| Partitions | A/B, virtual A/B, dynamic (super), `vendor_boot` GKI v4 |
| Userdata | F2FS, FBE v2 `aes-256-xts` + metadata encryption (`dm-default-key`) |
| Touch | Goodix (firmware + patched `xiaomi_touch_common.ko`) |

## Layout

```
device/xiaomi/degas/
  BoardConfig.mk, device.mk, twrp_degas.mk
  prebuilt/                 stock boot.img, dtbo.img, dtb
  vendor_ramdisk/           stock 306 kernel modules + first-stage fstab
  recovery/root/            recovery ramdisk: init.recovery.mt6897.rc, fstab,
                            Goodix firmware, MiTEE/Weaver/eSE blobs, prepdecrypt
  recovery/tee-blobs.sha256 checksums of the bundled TEE/eSE binaries
  omapi/                    native OMAPI service for the Weaver HAL
  sepolicy/
patches/                    patches for bootable/recovery (A/B slot switch, splash)
tools/slot_switch.py        host tool: switch A/B slot via misc bootloader_control
docs/RESEARCH.md            decryption notes
```

The repo is meant to be checked out at the top of a TWRP build tree; the
`.gitignore` is a whitelist.

## Building

GitHub Actions: **Actions → Build TWRP → Run workflow**. It syncs twrp-14.1,
applies `patches/`, builds and uploads `vendor_boot.img`. With `release_tag`
set it also publishes a pre-release.

Locally:

```bash
repo init --depth=1 -u https://github.com/minimal-manifest-twrp/platform_manifest_twrp_aosp.git -b twrp-14.1
repo sync -c
# put this repo's device/ and patches/ at the build top
./patches/apply_patches.sh
. build/envsetup.sh
lunch twrp_degas-ap2a-eng
mka vendorbootimage
```

The output `out/target/product/degas/vendor_boot.img` is flashable as is: the
stock modules are built into the vendor ramdisk, so the old repack script is
gone.

## Flashing

Unlocked bootloader required. Back up `vendor_boot_a`/`vendor_boot_b`,
`metadata`, `persist` and your data first.

```bash
fastboot getvar current-slot
fastboot flash vendor_boot_<slot> twrp-...-vendor_boot.img
fastboot reboot recovery
```

Do not relock the bootloader with a custom `vendor_boot`.

## Credits

- [sigmapro555eshkere](https://github.com/notnotshuts/ofox_device_xiaomi_degas):
  OrangeFox port of this tree with working decryption. The decryption stack,
  stock 306 modules, eSE/Weaver bring-up and native OMAPI service are taken
  from it.
- [Dip184](https://github.com/Dip184/android_device_xiaomi_duchamp/tree/orangefox-14.1):
  the MiTEE decryption method on duchamp that the port is based on.

## License

Apache License 2.0, see [LICENSE](LICENSE). Parts inherited from TWRP and AOSP
keep their upstream licenses. Stock vendor binaries belong to their owners.

---
*Maintained by Advnirr | [advnirr.org](https://advnirr.org)*
