# Xiaomi 12S Pro (unicorn) — hardware findings

Two kinds of statement are in this file, and they are kept apart on purpose:

- **from the phone's own source** — read out of the Xiaomi SM8450 devicetree release (the `unicorn-*`
  files and the cape/waipio platform trees they include), quoted with file and node. This is what the
  phone is, as far as the Android that ships on it is concerned.
- **inherited from the 12S port** — measured on a Xiaomi 12S (mayfly), the same SM8475/cape platform, by
  [cotunjr/hakios-mayfly](https://github.com/cotunjr/hakios-mayfly). It is a strong expectation, not a
  measurement of this phone: each item says what would confirm it here.

Nothing has been run on this phone yet (2026-11-06).

## The phone, from its own source

| | |
|---|---|
| SoC | SM8475 (Snapdragon 8+ Gen 1, "cape"), waipio family; Linux treats it as sm8450. `unicorn-sm8475-pm8008-overlay.dts`: `compatible = "qcom,cape-mtp", "qcom,cape", "qcom,mtp"`, `qcom,msm-id = <530 0x10000>, <531 0x10000>, <540 0x10000>`, `qcom,board-id = <0x10008 0>`, `xiaomi,miboard-id = <0x8 0>` |
| CPU | 8 cores in MPIDR's second affinity field: 0-3 little, 4-6 big, 7 prime (cape platform tree) |
| GIC | GICv3 at **0x17100000**, its MSI controller at 0x17140000 (`cape.dtsi`, `interrupt-controller@17100000`) |
| Display | **WQHD+ 1440x3200** DSC **command mode**, panel family **"l2s"** — `unicorn-sde-display-mtp.dtsi` names two builds, `dsi_l2s_38_0c_0a_dsc_cmd` and `dsi_l2s_42_02_0b_dsc_cmd`, both with WQHD timings at 1/10/24/30/40/60/90/120 Hz and `qcom,display-topology = <2 2 1>` |
| Panel rails | vci 3.0 V on gpio75, vddio 1.8 V on gpio74, vddd 1.5 V on gpio25, reset on gpio0 (`unicorn-sde-display-mtp.dtsi`, `display_panel_v*`) |
| Brightness | DCS (`bl_ctrl_dcs`), 8..2047, inverted DBV, init 307 |
| Touch | **ST FingerTip (FTS)**, `st,spi`, on **SPI QUP SE4**: irq gpio21, reset gpio20, `fts,x-max = <14400>`, `fts,y-max = <32000>` (16-bit super-resolution), 12 MHz, firmware `st_fts_l1.ftb`, avdd L9C 3.224 V, vdd L2C 1.8 V, panel vdd rail switched from gpio97 (`unicorn-sm8475.dtsi`, `&qupv3_se4_spi` / `fts@0`) |
| Amplifiers | **2x Cirrus CS35L41** on I2C **SE15**: `cs35l41@40` (right channel, reset gpio120, irq gpio117) and `cs35l41@43` (top, reset gpio1, speaker switch gpio52, irq gpio88) (`unicorn-sm8475.dtsi`, `&qupv3_se15_i2c`) |
| Haptics | Awinic **AW8697** on I2C **SE18**, 0x5a, reset gpio89, irq gpio90; the PMIC's own hv-haptics is disabled (`unicorn-sm8475.dtsi`) |
| Fingerprint | Goodix, reset gpio41, irq gpio40, 3.008 V from L11C |
| Wi-Fi | **QCA6490** on PCIe, `qcom,cnss-qca6490@b0000000` (`cape.dtsi`) |
| Bluetooth | **QCA6490** on UART **QUP SE20** (`hsuart0 = &qupv3_se20_4uart`), `bt-reset-gpio = <&tlmm 81>` = BT_EN (`cape.dtsi`, `bt_qca6490`) |
| Storage | UFS host **0x1d84000**, `qcom,ufs-phy-qmp-v4-cape` PHY, rails L7B/L9B/L5B/L10C (`cape.dtsi`, `xiaomi-sm8475-common.dtsi`) |
| USB | DWC3 at 0xa600000, eUSB2 with an **NXP repeater on I2C SE5 (0x4f)**, `param-override-seq` 0x62/0x08 0x00/0x07 (device), 0x63/0x08 0x22/0x07 (host) (`xiaomi-sm8475-common.dtsi`) |
| NFC | ST St25 on I2C SE9 (0x28), irq gpio46, ven gpio34 (`xiaomi-sm8475-common.dtsi`) |
| SD card | `sdhc_2` with card detect on gpio92 |

### The memory the bootloader reserves (cape platform, and so this phone)

`cape-reserved-memory.dtsi` and `xiaomi-sm8475-common.dtsi` give the carve-outs; the UEFI's own map
(`uefi/files/Platforms/Xiaomi/unicornPkg/Library/MemoryMapLib/MemoryMapLib.c`) was checked against them:

| Region | Address | Size | Node |
|---|---|---|---|
| HYP | 0x80000000 | 0x600000 | `hyp_region@80000000` |
| DT BLOB | 0x80600000 | 0x40000 | `xbl_dtlog_region@80600000` (where the bootloader's DT log goes; the DT itself is at 0x80600000 for Silicium) |
| AOP | 0x80800000 | 0xA0000 | `aop_image_region` + `aop_cmd_db_region` (0x80860000) + `aop_config_region` (0x80880000) |
| SMEM | 0x80900000 | 0x200000 | `smem_region@80900000` |
| CPUC{P} | 0x80b00000 | 0x100000 | `cpucp_fw_region@80b00000` |
| modem + remote processors | 0x85200000 | 0x1C200000 | the PIL carve-outs patchily: video 0x85700000, SLPI 0x88000000, CDSP 0x89900000, IPA 0x8b900000, SPSS 0x8ba00000, MPSS 0x8bc00000 + 0x13200000, CVP 0x9ee00000, camera 0x9f500000, **ADSP 0x9fd00000 + 0x3100000** |
| xbl ramdump / xbl_sc / global sync | 0xa6b80000 / 0xa6e00000 / 0xa6f00000 | 0x280000 / 0x40000 / 0x100000 | `xbl_ramdump_region`, `xbl_sc_region`, `global_sync_region` |
| **ramoops (the UEFI region, reused by apps)** | **0xa7000000** | **0x400000** | `ramoops@0xa7000000` in `xiaomi-sm8475-common.dtsi`; the tree marks the UEFI region "can be reused by apps" |
| the Android side's QTEE/Gunyah block | 0xe0000000 … | | `qheebsp_reserved_region`, `cpusys_vm_region`, `hyp_reserved_region`, `trust_ui_vm_*` |
| TZ stats / tags / QTEE | 0xe8800000 / 0xe8900000 / 0xe9b00000 | 0x100000 / 0x1200000 / 0x500000 | `tz_stat_region`, `tags_region`, `qtee_region` |

Two consequences the port uses:

- **A UEFI image goes at 0xA7100000, 3 MiB** (`Resources/Configs/unicorn.toml`). The 4 MiB from
  0xA7000000 is the region the bootloader loads a UEFI into and hands back to the system — Android puts
  its ramoops there. That is the same base the 12S port uses, and it is *this* phone's own tree that
  says the region is 4 MiB from 0xA7000000.
- **The "XNUP" payload goes at 0xB0000000** (`unicornPkg/unicorn.dsc`, `-DXNU_PAYLOAD_BASE`), inside
  the platform's "Kernel" region (see below).

### The phone's own device tree, as this repository carries it

`uefi/files/Resources/DTBs/unicorn.dtb` is **built from source, not pulled from the phone**:

```sh
# the Xiaomi SM8450 devicetree release (qcom/ + bindings/), and the kernel's dt-bindings headers
cpp -nostdinc -undef -D__DTS__ -x assembler-with-cpp -I qcom -I . -I qcom/include -I <kernel>/include \
    -I <kernel>/include/uapi qcom/cape.dts -o cape.dts && dtc -@ -I dts -O dtb -o cape.dtb cape.dts
cpp ... qcom/unicorn-sm8475-pm8008-overlay.dts -o u.dts && dtc -@ -I dts -O dtb -o u.dtbo u.dts
fdtoverlay -i cape.dtb -o unicorn.dtb u.dtbo
```

It is the cape platform tree with **unicorn's own board overlay** applied: touch, both amplifiers, the
haptics, the PMIC rails and the thermal zones are in it, with `model = "Cape SoC"` and an empty
`/memory` (the bootloader fills it — the Xiaomi 12S's own DT has the same placeholder, so this is the
shape a boot DT has here, not a defect).

Two Android overlays are **not** applied: `unicorn-sde-display-mtp-overlay.dtbo` (the DSI panels, the
SDE and the panel rails) and `unicorn-audio.dts` (the ALSA/ASoC graph). The display overlay does not
apply to the reference base (`fdtoverlap: FDT_ERR_NOTFOUND` — it targets nodes only a DSI-enabled base
carries), and neither is needed to start: HakiOS drives the panel and the DSP itself, and the UEFI draws
on the framebuffer ABL leaves live. When the phone is rooted the real tree is two commands away and
should replace this one:

```sh
adb pull /sys/firmware/fdt unicorn-live.dtb     # the running Android's tree: what the phone really is
```

## Inherited from the 12S port (same SoC and platform) — to be confirmed here

Each of these was measured on mayfly and is expected to hold on unicorn because the ABL, the TZ and the
SoC are the same. The confirmation for each is the same: **one boot, and the loader's log**.

- **Boot image**: ABL accepts header **v4** with `os_version`/`os_patch_level` = 0; the payload must be
  low. `0xF0000000` — HakiOS's default — is inside this family's TZ apps region (0xEA000000-0xF1400000
  by the platform's carve-outs), where a write resets the phone; `0xB0000000` does not.
- **GIC**: the redistributor stride is GICv4-sized, and Gunyah aborts accesses to another CPU's GICR
  frame, so a non-zero boot CPU has to go straight to its own frame.
- **Storage**: after ABL, the UFS controller is up (HCE=1) but SID 0xE0 is gone from the apps SMMU —
  any DMA faults and the phone resets ~20 s later. The loader installs its own SMR/bank instead (the
  same takeover the Redmi K60 port does) and serves `userdata` to the kernel.
- **USB (loader only)**: ABL leaves the GDSC, clocks and the eUSB2 PHY up and the DWC3 stopped; the
  loader needs the QSCRATCH VBUS overrides, an SMMU SMR for SID 0, and the DEPSTARTCFG resource index.
  Images must be packed with `mkpayload.py --usb` or the loader never appears as `xnuloader`.
- **CPU**: EPSS perf-state registers at 0x17d91000/0x17d92000/0x17d93000 + 0x320; the loader writes the
  top non-boost LUT entry (2016/2649/2995 MHz). TSENS at 0xc263000/0xc265000, sensors at +0xa0 + 4n.
- **Buttons**: PMIC arbiter v7 — core 0xc400000/0x3000, cnfg 0xc42d000, observer 0xc440000, map
  0x2000 + 4n, owner cnfg + 4n.
- **Display**: ABL leaves a live framebuffer at **0xB8000000** ("Display Reserved", 0x2B00000 in the
  12S's map). Unicorn's panel is 1440 wide, so a frame is 1440x4 bytes/line x 3200 = 18.4 MB: one frame
  fits the same reservation twice over, but **where ABL leaves the framebuffer on this phone, and how
  big the reservation is, has to be read from the first log** (GOP: `fb_base`, `width`, `height`,
  `stride`).
- **UI composition**: the 12S port finds the MDP pipe whose SRC0_ADDR is the framebuffer among mdss
  0xae00000 + 0x5000..0x2b000 and flips it by rewriting SRC0_ADDR; the same addresses are the cape
  platform's.
- **GPU**: Adreno 730 v3, chip id 0x07030002; TZ authenticates `a730_zap.mdt`; the GMU firmware is
  `gmu_gen70000.bin`; the zap lands at 0x8b91a000 (which this phone's tree has as
  `gpu_microcode_region@8b91a000`, 0x2000).
- **Wi-Fi**: QCA6490 as `wcn6855 hw2.1` (`LX_CHIP=qca6490`), SMMU context 73 / stream 0x1c01, firmware
  `amss20.bin`, `m3.bin`, `bd_l3s.elf`, `regdb_xiaomi.bin`, MAC from `persist:qca6490/wlan_mac.bin`.
  **`bd_l3s.elf` is the 12S's board file** — unicorn's board data has to come out of its own firmware,
  so this is one of the places where the 12S port cannot be copied as is.
- **Bluetooth**: QCA6490 over the polled GENI UART on SE20, BT_EN gpio81, SW_CTRL gpio82 (this phone's
  tree confirms 81; 82 is to be checked), rampatch `hpbtfw21.tlv`, NVM `hpnv21.bin`, in-band sleep.
- **Audio**: ADSP firmware from the ROM's `NON-HLOS.bin`, relocated to `adsp_region` 0x9FD00000 +
  0x3100000 (this phone's tree confirms the region); AudioReach/GPR over glink "adsp_apps"; IPCC
  0xed18000, RSC 0x17a20000. **The amplifiers are the difference**: the 12S's AW88261 take I2S/TDM at
  0x34/0x35 on SE15 with the switch on gpio52; unicorn's CS35L41 sit at 0x40/0x43 on the same bus and
  also use gpio52 as the speaker switch. A driver of unicorn's own is needed.

## What the UEFI of this port is

Derived from the 12S platform, and blob-free (no Qualcomm DXE drivers), as that port found necessary:

- `MemoryMapLib.c` — the SM8475 platform map (above).
- `unicorn.fdf` / `Include/*.inc` — Silicium's modules only; SimpleFbDxe on ABL's framebuffer
  (`USE_CUSTOM_DISPLAY_DRIVER = 0`), no RamManagerDxe.
- `unicorn.dsc` — DDR base 0x80000000, UEFI stack at 0xA760D000 (inside the reserved UEFI region),
  SMBIOS says Xiaomi 12S Pro / unicorn, framebuffer 1440x3200, `PcdDebugPropertyMask 0x03` so an ASSERT
  prints and carries on, and `-DXNU_PAYLOAD_BASE=0xB0000000ULL`.
- `Resources/DTBs/unicorn.dtb` — the tree described above; `Resources/Configs/unicorn.toml` — FD base
  and size, boot-image header v4, gzip, DTB appended.
