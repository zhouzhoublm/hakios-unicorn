# Bring-up history

Two days, in order. Image names (`xnu-NAME.img`) match the log directories some of these came from.

## 2026-10-31

| Time | Milestone |
|---|---|
| morning | Device facts from Android with temporary root (`fastboot boot` of a Magisk-patched stock boot.img, nothing flashed): live DTB, dmesg, iomem, vendor firmware. Touch confirmed as ST FTS, Wi-Fi as QCA6490 PCIe. |
| | UEFI blobs extracted with UEFIReader; mayflyPkg started from ingres (SM8450), then b4q (SM8475) memory map. |
| 15:00 | ABL accepts header v4 with os_version 0; payload must sit at 0xB0000000; DEBUG UEFI logs on screen. |
| 16:00 | "DXE hangs at random drivers" disproved with a CPU7 heartbeat that resets the phone at a chosen time: ClockDxe kills scan-out, PmicDxe aborts in SPMI, then an unexplained reset at ~29 s. |
| 16:40 | **XNU boots to pid 1** with a blob-free UEFI. |
| 17:05 | Native UFS read from the loader (SMMU stream takeover, START STOP UNIT). |
| 17:40 | **Apple logo**: Darwin root on `userdata`, real IPSW identity, resident flashboot volume. |
| 18:28 | **Touch works in iOS** (bit-banged SPI, 16-bit coordinates). |
| 19:12 | **All 18 Adreno 730 steps pass**: zap authenticated, GPU writes memory on command. |
| 19:48 | **USB in the loader**: own DWC3 driver; `fastboot oem run/log`, 1.4 MB log in 0.03 s. |
| evening | Panel at 120 Hz; Wi-Fi powers up and trains PCIe; Mesa renders on the GPU. |
| 21:43 | Wi-Fi reaches MHI mission mode. |
| 23:05 | **Buttons work.** Wi-Fi firmware takes ini/cfg and goes to mission mode. |

## 2026-11-01

| Time | Milestone |
|---|---|
| 08:10 | Wi-Fi: 16 MB-aligned QMI memory → "memory ready"; ADSP firmware extracted from NON-HLOS.bin. |
| morning | ADSP up; **battery level in iOS**; brightness sliders work. |
| 09:10 | **Wi-Fi internet works** (134 MB received; join from Settings). |
| 09:10 | AudioReach answers over GPR; PRM grants clocks. |
| 11:00 | **Bluetooth HCI up**: rampatch, NVM, HCI reset, BLE scan hears advertisers. |
| 11:50 | **Stock iOS bluetoothd** runs on the chip; a Mac sees the phone. |
| 12:10 | **Bluetooth audio** to a speaker. |
| 12:25 | Android comparison of the AW88261 registers while playing → the bus format is the difference. |
| 12:42 | **Internal speakers play** a tone: TDM sink instead of I2S. |
| 13:27 | **iOS sound through the speakers**, clean (no clipping with `sound_gain=0`). |
| 13:37 | **Tearing fixed** with the double-buffered flip (`fb_flip=1`), 63 → 94 fps. |
| 14:40 | Smoothness work: vmd denied, faster copies, HUD (`fb_hud=1`), 120 Hz for apps, thermal governor. Dynamic Island parked. |
| 14:50 | Daily image `xnu-daily1.img`; open issues in [OPEN-ISSUES.md](OPEN-ISSUES.md). |
