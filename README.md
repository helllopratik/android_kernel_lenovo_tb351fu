<p align="center">
  <img src="assets/readme-banner.svg" alt="Lenovo Tab Plus TB351FU kernel tree banner" />
</p>

# Android Kernel Tree for Lenovo Tab Plus TB351FU

This repository hosts the Linux kernel source tree currently being used for the Lenovo Tab Plus `TB351FU` bring-up work.

It is based on Lenovo's published open-source kernel release and is being reworked for custom development, testing, and Android 16 compatibility work around the `TB351FU` platform. The current focus is practical device bring-up, not a final production-ready Android 16 kernel release.

> [!NOTE]
> This tree is shared for educational and development use. Credit for the initial platform source belongs to Lenovo and the original upstream Linux / Android kernel contributors whose work this tree builds on.

## Current Status

- Base source: Lenovo open-source release for the `TB351FU` platform
- Kernel version: `5.10.177`
- Primary defconfig: `arch/arm64/configs/t808aa_defconfig`
- Toolchain direction in-tree: Clang / LLVM (`LLVM=1`, `LLVM_IAS=1`)
- Ongoing work: cleanup, bring-up, and Android 16 compatibility adjustments for the TB351FU custom ROM stack

## Device Reference

This kernel is paired with the Lenovo Tab Plus `TB351FU` custom ROM effort. The values below come from the kernel tree itself and the matching public bring-up trees used with it.

| Item | Value |
| --- | --- |
| Device | Lenovo Tab Plus `TB351FU` |
| Board | `t808aa` |
| Platform family | MediaTek `MT6789` / `MT8781` bring-up target |
| Architecture | `arm64` primary, `arm` secondary compatibility |
| Kernel base | Linux `5.10.177` |
| Boot setup | Boot header v4, DTB included in boot image |
| ROM direction | Android 16 / LineageOS bring-up |
| Related hardware features in the public trees | Dolby hooks, Lenovo pen support, virtual A/B, AVB-enabled layout |

## What Is In This Tree

- Lenovo-sourced kernel base for the TB351FU platform
- MediaTek platform support for the active bring-up target
- OEM-facing drivers and platform-specific integration points used by the stock software base
- The `t808aa_defconfig` currently referenced by the matching device tree

## Build Reference

This tree is not trying to replace the full Android build environment documentation. The commands below are a practical starting point for people already working inside an AOSP or LineageOS environment with the required toolchains available.

```bash
export ARCH=arm64
export SUBARCH=arm64

make O=out LLVM=1 LLVM_IAS=1 t808aa_defconfig
make -j"$(nproc)" O=out LLVM=1 LLVM_IAS=1
```

If you are building through a full Android tree, use the same kernel path and defconfig that the paired device tree expects:

- `TARGET_KERNEL_SOURCE := kernel/lenovo/TB351FU`
- `TARGET_KERNEL_CONFIG := t808aa_defconfig`

## Repository Pointers

- [arch/arm64/configs/t808aa_defconfig](arch/arm64/configs/t808aa_defconfig): active defconfig used by the matching device tree
- [build.config.mtk.aarch64](build.config.mtk.aarch64): Mediatek-oriented build configuration reference
- [android/abi_gki_aarch64_lenovo](android/abi_gki_aarch64_lenovo): ABI symbol list used for Lenovo-oriented GKI module compatibility work

## Scope And Intent

The goal of this repository is to make the published Lenovo kernel source more useful for community development on the `TB351FU`, especially for:

- recovery bring-up
- LineageOS / Android 16 experimentation
- debugging boot and hardware initialization issues
- educational study of the platform kernel layout

This does **not** mean every subsystem is already validated for daily-driver use on Android 16. Expect ongoing changes as bring-up progresses.

## Credits

- Lenovo, for publishing the original open-source kernel base used here
- Linux kernel contributors and Android kernel maintainers upstream
- LineageOS and the Android aftermarket development community for bring-up patterns, tooling, and debugging workflows

## Related Repositories

- Device tree: <https://github.com/helllopratik/android_device_lenovo_tb351fu>
- Vendor tree: <https://github.com/helllopratik/android_vendor_lenovo_tb351fu>
- Recovery tree: <https://github.com/helllopratik/twrp_device_lenovo_TB351FU>
- TB351FU dev page: <https://helllopratik.github.io/tb351fu/>
