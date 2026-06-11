# TWRP Device Tree for Xiaomi 14T (`degas`)

![Status: Beta](https://img.shields.io/badge/Status-Beta-yellow.svg)
![License: Apache-2.0](https://img.shields.io/badge/License-Apache--2.0-blue.svg)
![Platform: MT6897](https://img.shields.io/badge/Platform-MT6897-green.svg)

Unofficial TWRP (Team Win Recovery Project) device tree for the **Xiaomi 14T**
(code‑named **`degas`**, MediaTek Dimensity 8300‑Ultra `mt6897`, HyperOS 3 /
Android 16).

## 📌 Status: Beta

The recovery boots and is usable for day‑to‑day work: flashing, partition
backup/restore, wiping, and file transfer all work over a clean USB stack.

The one thing this recovery **cannot** do is decrypt `/data`. The device uses
File‑Based Encryption with metadata encryption (`dm-default-key`), and the
metadata key is bound to the TEE **Root‑of‑Trust**, which is different when a
custom recovery image is booted — so the key cannot be unwrapped from recovery.
This is a hardware/TEE limitation, not a missing feature. The full investigation
that established this is preserved in the
[`fbe-decryption-research`](../../tree/fbe-decryption-research) branch.

## 🔩 Device

| | |
|---|---|
| Model | Xiaomi 14T |
| Codename | `degas` |
| SoC | MediaTek Dimensity 8300‑Ultra (`mt6897`) |
| Architecture | arm64 (Cortex‑A715 + Cortex‑A510) |
| OS | HyperOS 3 (Android 16) |
| Partitions | A/B, dynamic (super), `vendor_boot` GKI v4 |
| Userdata FS | F2FS, FBE `aes-256-xts` + metadata encryption (`dm-default-key`) |
| Touch | Goodix (firmware + patched `xiaomi_touch_common.ko`) |

## 🛠️ Features & Hardware Support

* [x] **Core boot** — GKI v4 `vendor_boot`, prebuilt kernel/DTB.
* [x] **Display & GUI** — TWRP interface renders correctly.
* [x] **Touchscreen** — Goodix firmware + patched `xiaomi_touch_common.ko`.
* [x] **Partition mounting** — `xiaomi_dynamic_partitions` (super), `erofs` + `ext4` fallback.
* [x] **Backup** — raw image backup of `boot` / `init_boot` / `vendor_boot` / `dtbo` / `super` / `vbmeta*`.
* [x] **Restore**
* [x] **Wipe / Format Data**
* [x] **ADB** and **ADB sideload**
* [x] **MTP** — browse recovery storage over USB.
* [x] **USB‑OTG**
* [x] **A/B slot switching** — direct `misc` `bootloader_control` write (the stock boot‑HAL path hangs in standalone recovery).
* [x] **Other** (battery indicator)
* [ ] **FBE `/data` decryption** — not achievable from recovery (TEE Root‑of‑Trust binding; see the [`fbe-decryption-research`](../../tree/fbe-decryption-research) branch).

## 📂 Repository layout

```
device/xiaomi/degas/        TWRP device tree (the part you build)
  BoardConfig.mk            board / partition / battery / USB / AVB config
  device.mk                 product packages
  twrp_degas.mk             product definition (lunch target twrp_degas-eng)
  recovery.fstab            recovery fstab (logical partitions + backup entries)
  prebuilt/                 prebuilt boot.img, dtbo.img and dtb (build inputs)
  recovery/root/            files baked into the recovery ramdisk
                            (init.recovery.mt6897.rc: touch + USB ConfigFS + MTP,
                             Goodix firmware)
  sepolicy/                 device sepolicy

patch_touch_clean.sh        post-build repack: grafts the 215 vendor kernel
                            modules (+ patched Goodix touch driver) into the
                            freshly built vendor_boot
xiaomi_touch_common_patched.ko
tools/slot_switch.py        host utility: switch the active A/B slot by writing
                            the misc bootloader_control struct directly
```

> This tree is meant to be checked out inside a minimal TWRP build manifest. The
> `.gitignore` is a whitelist: it ignores the surrounding AOSP/TWRP tree and
> tracks only the files listed above. Built recovery images are not committed —
> rebuild from source or use Releases.

## ⚙️ Building

Use a minimal TWRP build environment, place this tree at `device/xiaomi/degas`,
then:

```bash
. build/envsetup.sh
lunch twrp_degas-eng
mka vendorbootimage
```

This device is `TARGET_NO_KERNEL` (it ships a prebuilt kernel), so the build
does not produce the vendor kernel modules. After the build, run the repack to
graft the stock modules (with the patched touch driver) into `vendor_boot`:

```bash
./patch_touch_clean.sh        # produces twrp_degas-vendor_boot-clean.img
fastboot flash vendor_boot twrp_degas-vendor_boot-clean.img
```

(Adjust the `WORKDIR`/input paths at the top of the script if your layout
differs. `init.recovery.mt6897.rc` and the Goodix firmware are baked into the
device tree; only the kernel modules are grafted by the repack.)

## 📸 Screenshots

<details>
<summary><b>Click to view</b></summary>

![TWRP Main Screen](screenshots/1.png)
![TWRP Partitions / Terminal](screenshots/2.png)

</details>

## ⚖️ License

Licensed under the Apache License 2.0 — see the [LICENSE](LICENSE) file.
Recovery/device‑tree pieces inherited from TWRP follow their upstream licenses
(GPL); the AOSP makefiles are Apache‑2.0.

---
*Maintained by Advnirr | [advnirr.org](https://advnirr.org)*
