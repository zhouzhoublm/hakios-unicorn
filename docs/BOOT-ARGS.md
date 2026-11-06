# Boot arguments on unicorn

`tools/build-image.sh NAME "ARGS"` (from the top of the HakiOS common repository) always adds the base
set, which is the 12S port's and is kept here because every argument in it is hardware this phone shares
(the SoC, the touch controller, the panel's command mode) — **none of it has been tried on this phone
yet**. What is written and not yet seen is listed as such.

```
loader_return_after=300 root_apfs=ufs_darwin ufs=2 bootcpu=7 cpus=7 touchscreen=1 touch_bitbang_ticks=4
gpu=1 panel_hz=120 fb_hz=120 ca_vbl=1 fb_flip=1
```

| Argument | Meaning | On unicorn |
|---|---|---|
| `root_apfs=ufs_darwin ufs=2` | root from the Darwin image on `userdata`, served by the loader | expected: the same UFS host, the same takeover |
| `bootcpu=7 cpus=7` | boot on the prime core, use 7 cores | expected: same SM8475 topology (0-3 / 4-6 / 7) |
| `touchscreen=1 touch_bitbang_ticks=4` | ST FTS touch and the PMIC buttons; the SPI is bit-banged because TZ leaves SE4's FIFO disabled | the chip, bus and pins are this phone's too (FINDINGS.md); the coordinate ranges in the driver must change to 14400x32000 first |
| `gpu=1` | keep the Adreno 730 on for the window server | same GPU; driver not yet ported |
| `panel_hz=120 fb_hz=120 ca_vbl=1` | 120 Hz panel and vsync to Core Animation | this panel does 1-120 Hz LTPO; the command sequences to switch it are the driver's own work |
| `fb_flip=1` | double-buffered page flip on the MDP pipe (no tearing) | expected: same mdss |
| `fb_hud=1` | overlay: fps, hottest TSENS, CPU MHz, free memory | diagnostics, harmless |
| `cpu_mhz=N` | CPU frequency ceiling (2400 is a good daily value on the 12S) | the LUT is the SoC's; the ceiling is a choice |
| `ddr_vote=0xNNNN` | DDR bandwidth vote (default 0x3fff = max) | as is |
| `panel_bright=N` | panel brightness in ‰ at boot | the panel's DCS scale goes to 2047 here |
| `adreno7=N adreno7_spin=1` | run GPU bring-up steps 1..N (debug) | after the driver is ported |
| `adreno7_speed=I` | GPU level index: 3 = 515, 4 = 580 (default), 5 = 645, 6 = 765, 7 = 816 MHz | the same Adreno 730 frequency list |
| `adsp=1 adsp_loud=1` | start the audio DSP (also needed for the battery's level) | after the DSP is up; the amplifiers are Cirrus CS35L41 here |
| `battmgr=2` | dump the battery manager's properties | diagnostics |
| `sound=2/3/4`, `sound_sd`, `sound_below`, `sound_gain`, `speaker_sr`, `speaker_spksw`, `speaker_only`, `sound_matrix` | the **12S's** Awinic AW88261 path (AW88261 at 0x34/0x35, TDM on gpio124, switch gpio52) | **not applicable**: unicorn's amplifiers are CS35L41 at 0x40/0x43. gpio52 is the speaker switch on both |
| `qcabt=6` | Bluetooth up to the iOS `bluetoothd` bridge | after the QCA6490 driver is ported |
| `wifi_pcie=12 wifi_lx=2 wifi_station=1 wifi_mac=..` | Wi-Fi bring-up through ath11k; `wifi_mac` is required and comes from `persist:qca6490/wlan_mac.bin` | after the Wi-Fi driver is ported; unicorn's board file (`bd_*.elf`) comes from its own firmware |
| `wifi_scan=1`, `cpu_sample=1`, `glprof=500`, `gllog=1` | profiling and one-shot diagnostics | as is |
| `loader_return_after=S` | the kernel returns to the loader after S seconds (test runs) | as is |

The first boot of this port carries none of the device-specific ones: it is the loader's own self test
(`payload_mode=selftest`), whose job is to prove the UEFI, the framebuffer, the memory map and the
payload all reach the phone, and to print the `[memmap]`, `[display]` and `[cpu]` dumps this
repository's numbers get corrected against.
