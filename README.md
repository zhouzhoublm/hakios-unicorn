# HakiOS on the Xiaomi 12S (mayfly, Snapdragon 8+ Gen 1)

iOS 16 user space on Apple's XNU kernel, booted on a Xiaomi 12S, with no Linux underneath. This is the
[HakiOS](https://github.com/oosexgang/hakios-xnu-kernel) device repository for **mayfly** (SM8475, "cape",
waipio family): the UEFI platform, the drivers that belong to this phone, its SoC layer, and everything we
learned bringing it up — notes, register values, logs and scripts — so the next person (or the next AI agent)
does not have to find it all again.

> **Status, 2026-11-01:** iOS boots to the home screen and is usable as a daily test device:
> 120 Hz display, touch, buttons, GPU-accelerated UI, Wi-Fi internet, Bluetooth audio, internal speakers and
> battery level all work. See the table below for what does not.

## What works

| Part | State | Notes |
|---|---|---|
| Display | works | 1080x2400 DSC command-mode panel, **120 Hz**, double-buffered (no tearing), brightness via DCS 0x51 |
| Touch | works | ST FingerTip (FTS) on SPI, bit-banged because TZ disables the GENI FIFO on SE4 |
| Buttons | works | power, volume up/down through the PMIC arbiter (v7) |
| CPU | works | 7 cores, EPSS performance states, thermal governor (90 °C for 30 s → one step down) |
| GPU | works | Adreno 730: GMU + HFI, zap shader authenticated by TZ, CP running; OpenGL ES via Mesa freedreno |
| Storage | works | UFS through the loader's own driver; the Darwin root lives on `userdata` |
| Wi-Fi | works | QCA6490 over PCIe, Linux ath11k inside XNU: scan, join, DHCP, Safari, speed tests |
| Bluetooth | works | QCA6490 BT over UART, Apple's own `bluetoothd`, pairing, A2DP to a speaker |
| Speakers | works | 2× AW88261 via the ADSP (AudioReach graph, tertiary **TDM** sink) |
| Battery | partial | level shown in iOS (ADSP battmgr); Battery Health page spins; no charge control |
| Thermal | partial | TSENS read; the phone sometimes resets under heavy load near 86–94 °C (cause unproven) |
| USB | partial | only the loader's own fastboot-style device (for logs and control), nothing in iOS |
| Microphone, camera, haptics, cellular, sensors | missing | not started |

`device.conf` holds the same table in machine-readable form.

## How it works

```
 ABL (Xiaomi's bootloader, unlocked)
   └─ fastboot boot xnu-NAME.img           RAM only, nothing is flashed to the boot chain
       └─ Mu-Silicium UEFI (mayflyPkg)     blob-free: no Qualcomm DXE drivers, framebuffer ABL left at 0xB8000000
           └─ xnuloader (loader.efi)       HakiOS's loader, at PAYLOAD_BASE 0xB0000000
               ├─ takes over UFS from ABL (own SMMU stream mapping, own command list) and serves the disk
               ├─ brings up its own DWC3 USB device: `fastboot oem run / oem log / get_staged` for logs
               ├─ sets CPU performance states (EPSS), builds the Apple device tree from the real IPSW identity
               └─ XNU (hakios-kernel) + Darwin ramdisk + resident flashboot volume
                   └─ launchd → iOS 16.5 (20F66) root on userdata: SpringBoard, backboardd, bluetoothd, wifid ...
```

The phone-specific kernel drivers are in `drivers/` here and are copied into the XNU tree at build time:

| Driver | What |
|---|---|
| `drivers/gpu/hakios.adreno7.c` | Adreno 730 bring-up in 18 checked steps (`adreno7=N`); also hosts the mayfly PCIe/Wi-Fi power-up (`wifi_pcie=N`), TSENS, CPU ceiling/governor, panel brightness |
| `drivers/input/hakios.stfts.c` | ST FTS touch on bit-banged SPI (gpio16-19), super-resolution 16-bit coordinates; DDR bandwidth vote |
| `drivers/wifi/hakios.qca6490.c` | QCA6490 MHI / QMI / WLFW bring-up before ath11k takes over |
| `drivers/bluetooth/hakios.qca6490bt.c` | QCA6490 BT: BT_EN, polled GENI UART on SE20, rampatch + NVM download, in-band sleep |
| `drivers/bluetooth/hakios.qcabt_h4.c`, `hakios.qcabt_shim.c` | H4 transport to iOS `bluetoothd` and vendor-command shim (adapted from iOSPlus10, MIT) |

The common-repository side of the port (loader, framebuffer, audio, Wi-Fi stack, userland hooks) is in
HakiOS itself: [oosexgang/hakios-xnu-kernel#47](https://github.com/oosexgang/hakios-xnu-kernel/pull/47)
(until it is merged, use the `mayfly-port` branch of [cotunjr/hakios-xnu-kernel](https://github.com/cotunjr/hakios-xnu-kernel/tree/mayfly-port)).

## Building and running

You need: an arm64 Mac (what this was built on) with Xcode and Docker, `fastboot`, the HakiOS common
repository, and **your own** phone's firmware and Apple's IPSW (neither is in any repository).

```sh
git clone -b mayfly-port https://github.com/cotunjr/hakios-xnu-kernel && cd hakios-xnu-kernel   # upstream once #47 is merged
git clone https://github.com/cotunjr/hakios-mayfly devices/mayfly     # or: tools/hakios-device.py sync, once it is in the manifest
# firmware: docs/FIRMWARE.md (fw-gpu/, fw-wifi/, fw-audio/, fw-bt/ at the top, never committed)

export PATH=/opt/homebrew/opt/llvm/bin:$PWD/xnu-deps/host/bin:$PWD/tools/macos-bin:/opt/homebrew/opt/bison/bin:$PATH
make mayfly_defconfig
LX_CHIP=qca6490 tools/mk-ath11k.sh          # REQUIRED: without LX_CHIP you get redwood's WCN6750 build and no Wi-Fi
make kernel                                 # "exit status 2 is the kext symbol-set checks" is expected
OSLOG="" ADSP=1 BT=1 CRYPTEXES=flash FLASHBOOT=1 REST=1 GL=1 userland/build.sh
LOADER_CFLAGS=-DPAYLOAD_BASE=0xB0000000ULL xnuloader/build.sh   # only when the loader changed; without it UEFI aborts at FAR 0xF0000000

devices/mayfly/tools/build-image.sh daily "fb_hud=1 cpu_mhz=2400 adsp=1 sound=4 sound_sd=2 sound_below=80 sound_gain=0 \
    speaker_sr=8 qcabt=6 wifi_pcie=12 wifi_lx=2 wifi_station=1 wifi_read_ms=0 wifi_wait_ms=10000 wifi_mac=<your MAC>"
# phone in fastboot (power + volume down), then:
devices/mayfly/tools/run.sh daily 120        # boots, runs 120 s, pulls the kernel log over USB
```

The UEFI is built from Mu-Silicium with `uefi/files` (see `uefi/` and the HakiOS docs on Silicium); the
`Binaries/mayfly` directory of vendor DXE drivers is **not** used by the working boot (blob-free) and is never
committed.

**The Darwin root** (`rootfs.apfs` + cryptexes from IPSW 20F66) is written to the `userdata` partition. That
erases Android's data. Going back to Android = `fastboot -w` or flashing the stock ROM. The boot chain,
`persist`, `modem` and friends are never touched.

`wifi_mac`: the address is in your phone's `persist` partition (`qca6490/wlan_mac.bin`); without it ath11k stops
with "IT HAS NO ADDRESS".

## Documentation in this repository

| File | What |
|---|---|
| [docs/FINDINGS.md](docs/FINDINGS.md) | Hardware facts and register-level findings, subsystem by subsystem, with the traps we fell into |
| [docs/HISTORY.md](docs/HISTORY.md) | The bring-up in order, from the first UEFI hang to iOS audio over Bluetooth |
| [docs/BOOT-ARGS.md](docs/BOOT-ARGS.md) | The boot arguments that matter on mayfly |
| [docs/FIRMWARE.md](docs/FIRMWARE.md) | Which firmware files are needed and where to get them from your own phone/ROM |
| [docs/OPEN-ISSUES.md](docs/OPEN-ISSUES.md) | What is broken or unknown, with the best current lead for each |
| [logs/](logs/) | Real kernel + iOS logs from the phone (scrubbed of MACs, SSIDs and serials) |
| [reference/android/](reference/android/) | Android-side data from the same phone: dmesg, live device tree, iomem, partitions |
| [tools/](tools/) | Image build, run-and-collect-log scripts, Android data collectors |
| [AGENTS.md](AGENTS.md) | Instructions for AI coding agents working on this port |

## Legal

No firmware, no Apple software and no vendor binaries are in this repository. You extract them from your own
phone and your own IPSW. Kernel drivers here are derivative works of XNU under the APSL 2.0; everything else is
MIT (see `LICENSE`). The Bluetooth H4/shim code is adapted from iOSPlus10 (MIT, `drivers/bluetooth/THIRD-PARTY-NOTICE`).
Not affiliated with Apple, Xiaomi or Qualcomm. You can brick nothing by following this (RAM boot only), but you
will lose Android's userdata.

## Credits

- [HakiOS](https://github.com/oosexgang/hakios-xnu-kernel) and its redwood port, which this follows
- [Project Silicium / Mu-Silicium](https://github.com/Project-Aloha/mu_aloha_platforms) for the UEFI base
- NicholasXiaoBai/mayfly-hackios (same phone, measured framebuffer/memory layout), XTOM0706/ios-sweet-k60,
  Emilelu/uefi-mondrian (SM8475 blob-free UEFI recipe), iOSPlus10 (QCA6490 Bluetooth on iOS)
- LineageOS `android_device_xiaomi_mayfly` / `sm8450-common` and the Linux kernel (ath11k, AudioReach, aw88261)
