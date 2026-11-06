# Xiaomi 12S (mayfly) — hardware findings

Everything here was measured on a real phone (model 2206123SC, HyperOS 2 / OS2.0.207.0.VLTCNXM, Android 15)
unless it says otherwise. Addresses are physical. "Android" means values read from the stock system with root.

## The phone

| | |
|---|---|
| SoC | SM8475 (Snapdragon 8+ Gen 1, "cape"), waipio family; Linux treats it as sm8450 |
| CPU | 8 cores: 0-3 little, 4-6 big, 7 prime; MPIDR aff1; PSCI 1.1; 16K pages work |
| GIC | GICv3 at 0x17100000; **redistributor stride is GICv4-sized** (see gic_v3_irq.c); Gunyah aborts accesses to other CPUs' GICR frames, so a non-zero boot CPU must go straight to its own frame |
| Memory | low DDR + "RAM High" 0x840000000, ~9 GB free above it |
| Display | 1080x2400 panel "l3s", DSC **command mode**, ABL leaves a live framebuffer at 0xB8000000, stride 4320 (`Display Reserved` 0xB8000000/0x2B00000) |
| Touch | ST FingerTip FTS (FTM5, "SALIXP"), SPI on QUP SE4 — **not** Goodix, not FocalTech |
| GPU | Adreno 730 v3, chip id 0x07030002 (Mesa calls it FD725), a7xx |
| Wi-Fi/BT | QCA6490: Wi-Fi on PCIe (17cb:1103), BT on UART QUP SE20 |
| Speakers | 2× Awinic AW88261 on I2C SE15 (gpio56/57), addresses 0x34 (top), 0x35 (bottom) |
| Haptics | PM8350B `qcom,hv-haptics@f000` (SPMI), f0 = 173 Hz |
| UFS | controller 0x1D84000 |

## Boot chain

- ABL accepts our boot image only as **header v4** and with `os_version`/`os_patch_level` = 0.
- The payload must be low: **0xB0000000** (in the "Kernel" region). The HakiOS default 0xF0000000 is inside
  waipio's TZ apps (0xEA000000-0xF1400000): writing there resets the phone. Writing 0x840000000 from the shim also resets.
- `fastboot boot` only, RAM boot. The loader cannot return the phone to ABL: hold power + volume down.
- Phone-reset timing is a usable measuring channel: Android's adb appears exactly 14 s after a reset.

## UEFI

- Vendor DXE drivers (from `uefi.elf`, `BOOT.MXF.2.0-01018.3-WAIPIO-1`, via UEFIReader patched for duplicate
  modules) were a dead end:
  - `ClockDxe` stops the panel scanning out (log freezes but CPUs keep running). Fast logs make it look like a
    later driver (HALIOMMU) hangs because the next lines fit in one frame.
  - `PmicDxe` takes a synchronous external abort at FAR 0x0C717008 (SPMI channel 0x217) through SPMI.dll.
  - After those, a self-reset at ~29 s that is not ResetSystem.
- **What works is blob-free** (like the K60 / uefi-mondrian ports): no vendor drivers at all, SimpleFbDxe on
  ABL's framebuffer. Recipe in `uefi/patches/README.md`. The kernel and loader drive the hardware themselves.
- ArmCpu's "CPU Vectors" region breaks the boot. Gunyah's watchdog SMC returns an error (harmless).
- `xnuloader/mkidentity.sh` must be run for the real IPSW identity; `make loader` does not refresh the placeholder.

## Storage (UFS)

- ABL leaves the controller up (HCE=1) but **removes SID 0xE0 from the apps SMMU**: any UFS DMA faults
  (USFCFG=1) and the phone resets ~20 s later.
- Fix (loader, `mayfly_ufs_probe`): install a free SMR → the last context bank (74), CBAR 0x10000, CBA2R 1,
  SCTLR 0 (bypass) — the same takeover the K60 port does.
- The device is asleep: the first READ returns status 2. Send START STOP UNIT to LUN 0xD0 first.
- The loader keeps its own UTP list at 0xA9000000 and serves `userdata` to the kernel as the Darwin disk.

## USB (loader only)

- ABL leaves GDSC, clocks and the eUSB2 PHY up, the DWC3 controller stopped.
- Needed: QSCRATCH VBUS override (`0xa6f8810 |= bit20|bit28`, `0xa6f8830 |= bit24`), an apps SMMU SMR for
  SID 0 → bypass bank, and DEPSTARTCFG's resource index in DEPCMD[22:16].
- Images **must** be packed with `xnuloader/mkpayload.py --usb`, or the loader never shows up as `xnuloader`.
- macOS asks once to allow the accessory "xnuloader".

## CPU

- EPSS perf-state registers at 0x17d91000 / 0x17d92000 / 0x17d93000 + 0x320. The loader writes the top
  non-boost LUT entry: 2016 / 2649 / 2995 MHz. `cpu_mhz=N` puts a ceiling on it.
- Governor (kernel, adreno7.c): 30 s continuously above 90 °C → one step down, never above `cpu_mhz`.
- TSENS controllers at 0xc263000 / 0xc265000, sensors at +0xa0 + 4n.
- DDR bandwidth vote `ddr_vote` (default 0x3fff = max) is accepted (in stfts.c, `hakios_mayfly_memory_fast`).

## Touch

- TZ loads GENI firmware on SE4 with the FIFO disabled (`GENI_IF_DISABLE_RO=1`; Android uses GPI DMA), so
  SPI is **bit-banged on gpio16-19**, `touch_bitbang_ticks=4`.
- Chip ID 36 40. Coordinates are **super-resolution 16-bit**: x = e3<<8 | e2, y = e5<<8 | e4, range 10800x24000
  (verified by tapping corners; 12-bit is wrong).

## Buttons

- PMIC arbiter v7: core 0xc400000/0x3000, cnfg 0xc42d000, observer 0xc440000 (+0x8000·ee + 0x20·apid),
  map 0x2000 + 4n, owner cnfg + 4n.

## Display

- Panel measured at 121 Hz with `panel_hz=120 fb_hz=120 ca_vbl=1`: autorefresh on every tear signal plus the
  panel's own 120 Hz switch commands via DSI command DMA.
- **Tearing fix (`fb_flip=1`)**: a second buffer at fb + 0xa00000 inside Display Reserved. The MDP pipe is found
  by SRC0_ADDR (+0x14) == framebuffer PA among mdss 0xae00000 + 0x5000..0x2b000 (it is vig0, ctl 0); flip by
  writing SRC0_ADDR and CTL_FLUSH (+0x18) on control paths that have layers; the flip is taken when the bit
  clears (~1 ms). 63 → 94 frames/s.
- Brightness: DCS 0x51, max 0x7ff.
- iOS renders at 1125x2436 (the D22 identity) and is copied/scaled to 1080x2400; copy by runs takes 0.7 ms.

## GPU (Adreno 730)

18 steps, all pass (`adreno7=18 adreno7_spin=1`):
CX on → GMU clocks → GMU alive → GPU SMMU 0x3da0000 writable (through the hypervisor) → **TZ accepts
`a730_zap.mdt`** (give it the whole mdt = header + hash segment 2) → GMU firmware `gmu_gen70000.bin` → RSCC
sleep-then-wake like Linux (a plain wake gets ack 0) → HFI tables, perf/bw tables → OOB GPU_SET powers the
3D core → CP start + `a7xx_cp_init` → zap at 0x8b91a000 → out of secure mode → GPU writes memory on command.

Traps:
- After `PAS_AUTH_AND_RESET` a blocking sleep of our thread never returned; spin waits work.
- Map zap memory with `ml_io_map` and **never** `ml_io_unmap` it (permanent I/O maps panic, 16K pages).
- Frequencies: 900/862/815/765/710/645/580/515/439/364/324/285/220 MHz; Android caps at 645. We default to
  **580 MHz** (heat). `adreno7_speed` index 4 = 580, 5 = 645, 6 = 765, 7 = 816.
- Mesa 25.0.7 needs patches in `tools/mesa/` (a725 cmdbuf start, a7xx fbfetch batch), `gmem_base=0`.
- GPU time per frame is ~0.8 ms: the UI is **not** GPU-bound.

## Wi-Fi (QCA6490)

- PCIe Gen3 x1, 17cb:1103. SMMU context 73, stream 0x1c01 for the device.
- MHI: BHI, MSI into RAM, BHIe, M0 → mission mode; QMI target memory must be **16 MB aligned** for pieces
  > 2 MB, otherwise "memory ready" never comes (and aligning every piece runs the ring pool out).
- ath11k must be built as WCN6855 hw2.1: `LX_CHIP=qca6490 tools/mk-ath11k.sh` (check the log for
  "wcn6855 hw2.1"; the default is redwood's WCN6750).
- `lx_bus.c`: `ab->mem_ce = ab->mem` or HTC never comes up.
- The MAC must be given (`wifi_mac=`) from `persist:qca6490/wlan_mac.bin`.
- Firmware: `amss20.bin`, `m3.bin`, `bd_l3s.elf`, `regdb_xiaomi.bin`.
- About half of starts get no QMI answer or reset at bus start; a retry works (suspected race with the ADSP).

## Bluetooth (QCA6490)

- UART on QUP SE20 at 0x894000: gpio76 CTS, 77 RTS, 78 TX, 79 RX; BT_EN gpio81, SW_CTRL gpio82; same rails as Wi-Fi.
- Polled GENI FIFO, no IRQ/DMA. Rampatch `hpbtfw21.tlv` (14 s at 115200), NVM `hpnv21.bin`. SoC 0x400c0210, ROM 0x201.
- The chip sends **in-band sleep bytes** (0xfd repeated until the host answers 0xfc) even with the NVM sleep
  bits cleared; unanswered, every event is blocked.
- The address reads the NVM placeholder 00:00:00:00:5a:ad; set a real one (vendor 0xFC14) before pairing.
- 115200 baud is not enough for A2DP: switch to 3 Mbaud.
- iOS side: the bluetooth device-tree node **must be at the root** (GenericARMPE only makes nubs for root children;
  under /arm-io bluetoothd says "No bluetooth"). A launchd job labelled `com.apple.*` running a non-platform
  binary panics launchd: the BlueTool stand-in is `com.hakios.bluetool` with Mach service `com.apple.BlueTool`.
- Audio needs Apple's `BTAudioHALPlugin.driver` where init binds the HAL plug-in directory, plus the
  bluetoothuserd, BTAvrcp and BTServer.le jobs.

## Audio

- ADSP firmware from the ROM's `NON-HLOS.bin` (`adsp.mdt` + b01..b25), linked at 0x86300000, relocated to the DT
  `adsp_region` 0x9fd00000/0x3100000. IPCC 0xed18000, RSC 0x17a20000.
- mayfly is **AudioReach / GPR** over glink "adsp_apps" (not APR like redwood). Battery through PMIC_RTR_ADSP_APPS (battmgr).
- PRM grants LPASS core + clocks; APM graph: WR_SHARED_MEM_EP → PCM_DEC → PCM_CNV → sink.
- **The speaker fix:** the amplifiers need **TDM**, not I2S. MODULE_ID_TDM_SINK 0x0700100E, PARAM_ID_TDM_INTF_CFG
  0x0800101B: tertiary, 4 slots × 32 bit, slot mask 3, internal sync, data out on, sync mode 1 (long), invert 1,
  data delay 1; clock TER_TDM_IBIT 0x204 at 12.288 MHz (96 kHz). With I2S the amps lock their PLL, unmute, but
  status stays 0x0011 forever (no SWS/BSTS: the output stage never switches).
- Pins: tertiary MI2S/TDM gpio121 SCK, 123 WS, 124 data out (`sound_sd=2`), 122 is data **in** (amp feedback).
  gpio52 (speaker switch) must be **LOW** for the speakers (high = handset path).
- Android while playing: amp 01=0311, 23 (PVDD)=0x170, 0x27=1fc0. Idle defaults (21=0263, 22=0019, 23=0263,
  28=1708, 29=0802) are what you read when the amp has not started: VBAT 0x263 is a reset value, not a measurement.
- AW88261 I2C needs 25 kHz and two agreeing reads for config registers (not SYSST, not 0x20-0x29 which change while
  playing); faults must be seen twice. Reads of 0x34 sometimes returned 0x35's values at higher speed.
- `sound_gain=0`: iOS scales samples itself and the D22 chain is near full scale; +12 dB clipped (distorted sound).
- Android's path: `TDM-LPAIF-RX-TERTIARY`, S32_LE, 96 kHz, 3 channels on the PCM. We run 48 kHz on the bus with
  the amp at `speaker_sr=8`.

## User space

- `vmd` (visual voicemail) ate a whole core: denied in `mkjobs.py`.
- 120 Hz in apps: inject `libhakios.fps` (sets `CADisableMinimumFrameDurationOnPhone`) into apps through the
  kernel's `exec_inject "@apps"`, because the flashed root is Apple's and `mkhighrate.py` only changes system.apfs.
- Dynamic Island (`island=1`) — all attempts failed, **do not ship island=1** (SpringBoard boot loop): interposing
  MobileGestalt misses shared-cache calls; the MG cache file does not exist; inline-patching MG → SIGILL;
  `SBUIIsSystemApertureEnabled=YES` via `dyld_dynamic_interpose` → SIGSEGV loop.

## Build traps

- `make kernel` does not always copy `devices/mayfly/drivers/gpu/hakios.adreno7.c` or headers under `xnu/files`
  into `xnu-src`: copy by hand when in doubt.
- `xnu/files/iokit/Kernel/MiniIOMobileFramebuffer.cpp` is an export copy; edit `xnu-src/.../iokit/Kernel/` and run
  `xnu/export.sh` before committing.
- `IO_MAP_SIZE` had to grow to 64 MB (`osfmk/arm/io_map.c`).
- Build PATH needs `/opt/homebrew/opt/llvm/bin` (llvm-nm/objcopy) and brew `bison` 3.8 (Mesa). `unpack` needs
  `7zz` (p7zip is too old); `dmg.py` needs python `lzfse`.
