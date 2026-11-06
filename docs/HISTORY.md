# How this repository was made

The 12S Pro port starts from the Xiaomi 12S port, because it is the same SoC and the same Qualcomm
platform: [cotunjr/hakios-mayfly](https://github.com/cotunjr/hakios-mayfly). This is the order the work
was done in, so that the next person can redo it or disagree with it.

## 1. The template, taken as it is

`hakios-mayfly` was copied whole into `devices/unicorn/` and every name changed (`mayfly` → `unicorn`,
`mayflyPkg` → `unicornPkg`, `configs/mayfly*.config` → `configs/unicorn*.config`, the UEFI platform's
`.dec/.dsc/.fdf`, `Resources/Configs/unicorn.toml`, `Resources/DTBs/unicorn.dtb`, the device's
`XnuLoader.h.unicorn`). Nothing else was carried over from any other port: **no memory address, no
framebuffer base, no payload base and no panel number comes from anywhere but this phone's own device
tree or the platform it runs**, and each one is argued in docs/FINDINGS.md.

What was thrown away rather than renamed, because it is another phone's data and would be a lie here:

- `reference/android/*` — the 12S's `dmesg`, `iomem`, `partitions`, live device tree and `getprop`
  dumps. The collectors that produced them are kept (`tools/android/`), and `reference/README.md` says
  what to pull from this phone.
- `logs/*.log` — kernel and iOS logs from the 12S.
- `uefi/files/Binaries/mayfly/README.md` — kept as `Binaries/unicorn/README.md`, which is the note
  saying no vendor driver is in the repository.

## 2. What was read out of this phone's own source

The Xiaomi SM8450 devicetree release (the `unicorn-*` board files plus the cape/waipio platform trees
they include) answered every question the 12S template could not:

| Question | Answer for unicorn | Where |
|---|---|---|
| Does it have the same touch controller? | yes — ST FTS, SPI SE4, gpio20/21, but 14400x32000, not 10800x24000 | `unicorn-sm8475.dtsi`, `&qupv3_se4_spi` |
| Same amplifiers? | **no** — 2x Cirrus CS35L41 on SE15 at 0x40/0x43 | `unicorn-sm8475.dtsi`, `&qupv3_se15_i2c` |
| Same haptics? | **no** — Awinic AW8697 on I2C SE18, the PMIC's hv-haptics disabled | `unicorn-sm8475.dtsi` |
| Panel? | WQHD+ 1440x3200, "l2s", DSC command mode, 1-120 Hz | `unicorn-sde-display-mtp.dtsi` |
| Wi-Fi/BT chip? | QCA6490, as the cape platform defines it | `cape.dtsi`, `bt_qca6490` / `cnss-qca6490` |
| UFS? | 0x1d84000, cape PHY v4 | `cape.dtsi`, `xiaomi-sm8475-common.dtsi` |
| Memory carve-outs? | the cape platform's table, and the 4 MiB from 0xA7000000 Android uses for ramoops | `cape-reserved-memory.dtsi`, `xiaomi-sm8475-common.dtsi` |

## 3. The UEFI platform

`unicornPkg` is `mayflyPkg` with:

- the framebuffer **1440x3200** (was 1080x2400),
- SMBIOS **Xiaomi 12S Pro / unicorn / Xiaomi_12S_Pro_unicorn**,
- the **memory map unchanged** — it is the SM8475 platform's, and this phone's own tree agrees with the
  entries it names (FINDINGS.md has the table),
- the FD at **0xA7100000 / 0x300000** — and here the phone's own tree is the evidence: Android's
  ramoops on unicorn occupies 0xA7000000 + 4 MiB, the region the bootloader hands a UEFI image,
- `-DXNU_PAYLOAD_BASE=0xB0000000ULL` unchanged: this family's TZ apps live at 0xEA000000, which is why
  HakiOS's default 0xF0000000 cannot be used on any phone of this family.

## 4. The device tree

Built from source (see FINDINGS.md): the cape platform tree with unicorn's board overlay applied, and
the two Android overlays (DSI panels, ASoC graph) left out because they do not apply to the reference
base and HakiOS needs neither to start. The command is in FINDINGS.md; the phone's own
`/sys/firmware/fdt` is the upgrade.

## 5. The image

`SILICIUM_UEFI.fd` for `unicornPkg`, the BootShim, the loader and (later) the kernel are packed into an
Android boot image v4 exactly as the 12S port does it — the recipe and the numbers are in
[docs/BUILD.md](BUILD.md).

## 6. What is not done

Everything that needs the phone: the first boot, the framebuffer's real base, the panel, the touch
ranges, the amplifiers, haptics, Wi-Fi, Bluetooth. docs/OPEN-ISSUES.md orders them.
