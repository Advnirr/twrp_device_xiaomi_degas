# TWRP device tree for Xiaomi 14T (`degas`)

![Status: Beta](https://img.shields.io/badge/Status-Beta-yellow.svg)
![License: Apache-2.0](https://img.shields.io/badge/License-Apache--2.0-blue.svg)
![Platform: MT6897](https://img.shields.io/badge/Platform-MT6897-green.svg)
![Decryption: working](https://img.shields.io/badge/Decryption-working-brightgreen.svg)

Unofficial TWRP 3.7.1 (twrp-14.1) for the Xiaomi 14T (`degas`, MediaTek
Dimensity 8300-Ultra `mt6897`, HyperOS 3 / Android 16). The recovery lives in
`vendor_boot`.

## Status

TWRP 3.7.1 (twrp-14.1) boots to the UI and decrypts `/data`, PIN included.
Images are on the [Releases](https://github.com/nyxianglyph/twrp_device_xiaomi_degas/releases) page.

Tested on a Xiaomi 14T, HyperOS `OS3.0.305.0.WNEIDXM`, FocalTech touch panel:

| | |
|---|---|
| TWRP UI, touch, ADB | ✅ |
| Metadata decryption + DE keys (automatic at boot) | ✅ |
| PIN decryption (Gatekeeper + AIDL Weaver) | ✅ `/sdcard` readable |
| Android afterwards | ✅ boots, SELinux enforcing, PIN still unlocks |
| MTP | ✅ read/write/delete from a PC |
| Zip install, ADB sideload | ✅ ADB and MTP are back ~4 s after a sideload |
| Partition backup + restore | ✅ `boot` restored byte-identical |
| Data backup | ✅ FBE policies stored. Restore not tested |
| Slot switch | ✅ |
| fastbootd | ✅ USB, `current-slot`, `reboot recovery` |
| OTG | ⚠️ not tested |
| Goodix-panel units | ⚠️ not tested |

Decryption uses the stock MiTEE stack inside recovery: tee-supplicant,
KeyMint 3.0, Gatekeeper, and NXP Weaver over the eSE. The method comes from
[sigmapro555eshkere's OrangeFox tree](https://github.com/notnotshuts/ofox_device_xiaomi_degas).
[docs/RESEARCH.md](docs/RESEARCH.md) explains how the stack works.

TWRP never writes upgraded key blobs back to the device: they only go to
`/tmp`, `/metadata` is mounted read-only, and keystore2 uses
`/tmp/misc/keystore`.

The bundled kernel modules and TEE blobs are from HyperOS
`OS3.0.306.0.WNEEUXM`. On a 305 IN unit (`OS3.0.305.0.WNEIDXM`) all 215
vendor_boot modules and all 43 TEE/eSE blobs are byte-identical. On other
firmware, compare them before flashing.

### Known issues

- fastbootd cannot open logical partitions (`system_a`, `vendor_a`, …):
  TWRP keeps them mapped. Flash them from the bootloader or with a zip.
- `persist_bak` (TWRP's persist backup mount) fails with "busy", because
  `persist` is mounted read-write for the TEE.
- The ROM's APEX images cannot be loop-mounted ("Value too large for defined
  data type"). Zips that run the ROM's own binaries may fail.
- The backup folder is named `serialno`: TWRP cannot read the serial number.
- tee-supplicant logs harmless `/dev/kmsg` permission warnings.

## Device

| | |
|---|---|
| Model | Xiaomi 14T |
| Codename | `degas` |
| SoC | MediaTek Dimensity 8300-Ultra (`mt6897`) |
| Partitions | A/B, virtual A/B, dynamic (super), `vendor_boot` GKI v4 |
| Userdata | F2FS, FBE v2 `aes-256-xts` + metadata encryption (`dm-default-key`) |
| Touch | Goodix (firmware + patched `xiaomi_touch_common.ko`) or FocalTech (`focaltech_touch_rothko`, raw mode turned off) |

## Layout

```
device/xiaomi/degas/
  BoardConfig.mk, device.mk, twrp_degas.mk
  prebuilt/dtb/              stock dtb
  vendor_ramdisk/           stock 306 kernel modules + first-stage fstab
  recovery/root/            recovery ramdisk: init.recovery.mt6897.rc, fstab,
                            Goodix firmware, MiTEE/Weaver/eSE blobs, prepdecrypt
  recovery/tee-blobs.sha256 checksums of the bundled TEE/eSE binaries
  omapi/                    native OMAPI service for the Weaver HAL
  sepolicy/
patches/                    patches for bootable/recovery (A/B slot switch, splash,
                            twrp-14.1 build and decryption fixes) and
                            system/vold (AIDL Weaver)
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

Get the image from
[Releases](https://github.com/nyxianglyph/twrp_device_xiaomi_degas/releases).

```bash
fastboot getvar current-slot
fastboot flash vendor_boot_<slot> twrp-...-vendor_boot.img
fastboot reboot recovery
```

Flash only the active slot. To go back, flash your stock `vendor_boot` backup
to it.

- Do not relock the bootloader with a custom `vendor_boot`.
- Do not flash right after an OTA, before its background merge has finished.

## Credits

- [Advnirr](https://github.com/Advnirr/twrp_device_xiaomi_degas): the
  original TWRP tree for degas this fork is based on.
- [sigmapro555eshkere](https://github.com/notnotshuts/ofox_device_xiaomi_degas):
  OrangeFox port of this tree with working decryption. The decryption stack,
  stock 306 modules, eSE/Weaver bring-up and native OMAPI service are taken
  from it.
- [Dip184](https://github.com/Dip184/android_device_xiaomi_duchamp/tree/orangefox-14.1):
  the MiTEE decryption method on duchamp that the port is based on.
- [OrangeFox](https://gitlab.com/OrangeFox): the AIDL Weaver hunks of
  `patch-vold-fox_14.1`, used in `patches/system/vold/0001`.

## License

Apache License 2.0, see [LICENSE](LICENSE). Parts inherited from TWRP and AOSP
keep their upstream licenses. Stock vendor binaries belong to their owners.

---
*Maintained by nyxianglyph — fork of [Advnirr/twrp_device_xiaomi_degas](https://github.com/Advnirr/twrp_device_xiaomi_degas)*
