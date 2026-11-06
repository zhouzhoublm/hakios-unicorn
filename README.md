# HakiOS on the Xiaomi 12S Pro (unicorn, Snapdragon 8+ Gen 1)

iOS on Apple's XNU kernel, booted on a Xiaomi 12S Pro, with no Linux underneath. This is the HakiOS
device repository for **unicorn** (SM8475, "cape", waipio family): its UEFI platform, the drivers that
belong to this phone, its SoC layer, and what we learn bringing it up.

> **Status, 2026-11-06 — stage 0, first boots done.** This repository was made by deriving it from
> [cotunjr/hakios-mayfly](https://github.com/cotunjr/hakios-mayfly) (the Xiaomi 12S port, the same SoC),
> and its UEFI for unicorn is built and packs into a boot image. **It has now been started on the phone
> three times**: ABL accepts and starts the image every time (0.43 s), and **nothing our code does reaches
> USB** — no `xnuloader`, no device at all — before the phone resets itself and Android comes back at
> ~60 s. So the port is blind until the loader's USB enumerates; docs/LOG-CHANNELS.md is the analysis of
> which log channels this phone does and does not have, and the order to build them in. Every `status`
> line in `device.conf` still says `missing` or `partial` and means it.

## What this phone is (from its own device tree, not from the 12S)

| | |
|---|---|
| SoC | SM8475, 8 cores (0-3 A510, 4-6 A710, 7 X2), Adreno 730, GICv3 at 0x17100000 |
| Display | WQHD+ **1440x3200** DSC command-mode panel, panel family "l2s", 1-120 Hz LTPO, DCS brightness (max 2047) |
| Touch | **ST FingerTip (FTS)** on SPI QUP SE4, irq gpio21, reset gpio20, 16-bit super-resolution (14400x32000) |
| Amplifiers | **2x Cirrus CS35L41** on I2C QUP SE15, 0x40 (right) and 0x43 (top), speaker switch gpio52 |
| Haptics | Awinic AW8697 on I2C QUP SE18 (0x5a), reset gpio89, irq gpio90 |
| Wi-Fi/BT | QCA6490: WLAN on PCIe, Bluetooth on UART QUP SE20, BT_EN gpio81 |
| Storage | UFS host 0x1d84000 (cape PHY v4) |
| USB | eUSB2 with an NXP repeater on I2C QUP SE5 (0x4f) |

Every line comes from the phone's own source: `unicorn-sm8475.dtsi`, `unicorn-pinctrl.dtsi`,
`unicorn-sde-display-mtp.dtsi`, `xiaomi-sm8475-common.dtsi` and the cape/waipio platform trees of the
Xiaomi SM8450 devicetree release. docs/FINDINGS.md quotes each one with its file and node.

## What differs from the Xiaomi 12S (mayfly) port this came from

| Part | mayfly (12S) | unicorn (12S Pro) | Consequence |
|---|---|---|---|
| Panel | 1080x2400, "l3s" | **1440x3200**, "l2s" | `PcdFrameBufferWidth/Height` 1440x3200 in `unicorn.dsc` |
| Touch | ST FTS, SE4, gpio20/21 | **same chip, same bus, same pins** | mayfly's `hakios.stfts.c` fits, its coordinate ranges (10800x24000) do not |
| Amplifiers | Awinic AW88261 x2 at 0x34/0x35 | **Cirrus CS35L41** x2 at 0x40/0x43 | the speaker driver must be written for unicorn; `CONFIG_HAKIOS_SPEAKER` stays off |
| Haptics | PM8350B hv-haptics (SPMI) | **Awinic AW8697** (I2C SE18) | another driver |
| Fingerprint | — | Goodix (gpio40/41) | out of scope for now |
| Memory map | SM8475 platform | **same platform** | `MemoryMapLib.c` is the platform's map, unchanged |
| UEFI FD | 0xA7100000 / 0x300000 | **same** | the phone's own `ramoops@0xa7000000` (4 MiB) occupies exactly the region the bootloader hands to a UEFI image |
| Payload base | 0xB0000000 | **same** | 0xF0000000 (HakiOS's default) is inside this family's TZ apps region and resets the phone |
| SoC layer | `sm8450` | `sm8450` (unchanged) | `soc.args` is the family's; the addresses were re-checked against unicorn's tree |

## How it boots

```
 ABL (Xiaomi's bootloader, unlocked)
   +- fastboot boot xnu-NAME.img          RAM only, nothing is flashed
       +- Mu-Silicium UEFI (unicornPkg)   blob-free, SimpleFbDxe on the framebuffer ABL leaves live
           +- xnuloader (loader.efi)      HakiOS's loader at PAYLOAD_BASE 0xB0000000
               +- XNU (hakios-kernel) + the Darwin root on userdata
```

The UEFI is built from Mu-Silicium at the commit in `uefi/BASE` of
[oosexgang/hakios-uefi](https://github.com/oosexgang/hakios-uefi), with HakiOS's changes and this
repository's `uefi/files` (the platform in `uefi/files/Platforms/Xiaomi/unicornPkg`, the boot shim
config in `uefi/files/Resources/Configs/unicorn.toml`, the device tree in
`uefi/files/Resources/DTBs/unicorn.dtb`).

## Build

```sh
# inside a checkout of the HakiOS common repository, with this repository at devices/unicorn/
uefi/apply.sh unicorn                  # Mu-Silicium + HakiOS's changes + unicorn's platform
cd Mu-Silicium && python3 build_uefi.py -d unicorn -r DEBUG
# -> Build/unicornPkg/DEBUG_CLANGPDB/FV/SILICIUM_UEFI.fd
```

The kernels (`make kernel`, `userland/build.sh`) are the common repository's; see
[docs/porting/index.md](https://github.com/oosexgang/hakios-xnu-kernel) there. The full recipe used for
this port, including a Linux-only path that needs no Mac, is
[`docs/BUILD.md`](docs/BUILD.md).

## Run

```sh
fastboot boot xnu-unicorn.img      # phone in fastboot (power + volume down); RAM only
```

Then read the loader's log over its own USB device (`fastboot oem log`, see `tools/run.sh`). Going back
to Android: hold power + volume down; nothing in the boot chain was touched.

## Documentation

| File | What |
|---|---|
| [docs/FINDINGS.md](docs/FINDINGS.md) | what this phone is, node by node, and where each value came from |
| [docs/BUILD.md](docs/BUILD.md) | how the UEFI and the boot image are built here (no Mac needed for the image) |
| [docs/HISTORY.md](docs/HISTORY.md) | how this repository was derived from the 12S port, step by step |
| [docs/BOOT-ARGS.md](docs/BOOT-ARGS.md) | the boot arguments, and which of them this phone has actually been given |
| [docs/FIRMWARE.md](docs/FIRMWARE.md) | which firmware files this port needs and where they come from |
| [docs/OPEN-ISSUES.md](docs/OPEN-ISSUES.md) | what is broken or unknown, with the best lead for each |
| [docs/LOG-CHANNELS.md](docs/LOG-CHANNELS.md) | what this phone can and cannot tell us when it fails to boot, measured, and the order to build the missing channels (tools/pc/boot-attempt.ps1 runs the loop) |
| [docs/USB-BRINGUP.md](docs/USB-BRINGUP.md) | why nothing enumerates today (the loader asks the firmware for a USB device protocol this blob-free firmware does not build), the two routes to a channel, and this phone's own DWC3/eUSB2/NXP-repeater parameters |
| [docs/USB-SERIAL.md](docs/USB-SERIAL.md) | the USB serial log: what uefi-pong / U-Boot / lk2nd already do on this class of hardware, and the stack chosen for unicorn (open DWC3 device DXE + four board functions + our class driver) |
| [reference/](reference/) | what to pull from the phone, and how |
| [tools/](tools/) | image build, run-and-collect-log, Android data collectors |
| [xnuloader/](xnuloader/) | the USB serial console for the loader (`usbfn.c`, CDC ACM over `EFI_USBFN_IO_PROTOCOL`) and the four edits it needs in the common repository's `loader.c` |

## Legal

No firmware, no Apple software and no vendor binaries are in this repository. Kernel drivers here are
derivative works of XNU under the APSL 2.0 (see `LICENSE.APSL`); everything else is MIT (`LICENSE`).
Not affiliated with Apple, Xiaomi or Qualcomm. Nothing here is flashed: the boot is RAM only, but the
Darwin root on `userdata` would replace Android's data.
