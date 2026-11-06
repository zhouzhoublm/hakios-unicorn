# Open issues

Ordered the way they have to be worked. Nothing has been run on this phone yet (2026-11-06); item 1 is
the whole of the current state.

## 1. The first boot has not happened

`SILICIUM_UEFI.fd` for `unicornPkg` is built, and packs into a boot image v4 with the BootShim, the
loader and this phone's device tree (docs/BUILD.md). It has not been started.

The run to do, and what each outcome means:

```sh
fastboot boot xnu-unicorn-selftest.img      # RAM only
fastboot devices                            # expect a device called "xnuloader" if UEFI got that far
fastboot oem log > log.txt                  # the loader's own log, if it did
```

| What you see | What it means | Next |
|---|---|---|
| nothing, phone resets within a second or two | the FD is not where ABL expects it, or the payload base is wrong | try `FD_BASE` 0xA7000000 / 0xA7400000 (the 4 MiB region's other alignments) and `PAYLOAD_BASE` 0xB8000000-0x04000000 |
| nothing, phone stays alive | UEFI started but no framebuffer / no display | read `[display]` in the log; the 12S's framebuffer base (0xB8000000) may not be this phone's |
| logo on screen, no `xnuloader` | UEFI's BDS did not find the payload, or StartImage failed | check the payload magic at 0xB0000000 in the log (`[payload]`) |
| `xnuloader` shows up, `oem log` gives a log | UEFI + loader are alive on this phone | fix the memory map against `[memmap]`, then item 2 |

The log also answers three questions at once: the real **framebuffer base/size/stride** (this panel is
1440 wide, so a frame is 18.4 MB), the **memory map** UEFI was given, and the **core** the loader came
up on.

## 2. Numbers in this repository that the first log will correct

- `uefi/files/Resources/DTBs/unicorn.dtb` — built from source with the board overlay, without the
  Android display/audio overlays (FINDINGS.md). The phone's `/sys/firmware/fdt` is the real thing.
- "Display Reserved" 0xB8000000 + 0x2B00000 in the memory map: the 12S's measurement, inherited.
  1440x3200 needs 18.4 MB per frame, so the reservation has room, but the **address** has to be
  confirmed from the GOP in the log.
- The UEFI stack base 0xA760D000 and the "UEFI FD Reserved"/"Info Blk"/log-buffer offsets inside the
  UEFI region: the 12S's layout of a region this phone also has, but only a boot proves the FD can own
  all of it here.

## 3. The kernel has not been built for this phone

`configs/unicorn.config` leaves every phone driver out (stage 2 of docs/porting/index.md of the common
repository). The port's next step after the first boot is the ladder in that page, and for this phone
the order is:

1. `CPUFREQ`, `THERMAL`, `SOCINFO` — the SoC's, shared with the 12S port's knowledge but to be checked
   against unicorn's tree.
2. `BUTTONS`, then touch: `CONFIG_HAKIOS_STFTS` with **`fts,x-max/y-max` = 14400x32000** and the
   deadzone/edgezone tables scaled from the 12S's 1080x2400 to 1440x3200 (the phone's own tree has the
   tables; `unicorn-sm8475.dtsi`, `fts@0`).
3. `UFS` — expected to work as on the 12S (same host, same takeover).
4. `ADRENO7`/`GPU` — mayfly's `hakios.adreno7.c` carries mayfly's panel and Wi-Fi power-up as well as
   the GPU; it has to be split before it is unicorn's.
5. `ADSP`, then `BATTERY`; `SOUND`/`SPEAKER` need a **new driver for the Cirrus CS35L41** (I2C SE15,
   0x40 right / 0x43 top, reset gpio120/gpio1, speaker switch gpio52) — the 12S's AW88261 code does not
   apply.
6. `QMI`, `QCA6490`, `WIFI_STACK` — with this phone's own board file.
7. `QCABT`, `HAPTIC` (AW8697 on SE18, not the PMIC), `CAMERA`, `MODEM`.

## 4. Unknown, and worth knowing before believing anything

- **Does ABL on unicorn hand over a live framebuffer at all** at the 12S's address? The panel and the
  display controller are the same mdss, the panel is bigger, and this is the single most likely
  difference between the two phones.
- **The DWC3 side**: the 12S port's QSCRATCH overrides and SMMU SMR are addresses of the SoC, so they
  should hold, but eUSB2 here goes through an NXP repeater (I2C SE5, 0x4f) that the 12S does not
  document the same way; if the loader's USB never enumerates, that repeater's state is the first thing
  to look at.
- **The 4 MiB at 0xA7000000**: Android uses it for ramoops on this phone, which is strong evidence it
  is the UEFI region, but it also means UEFI's own log buffer (`XNU_LOG_PARTITION`, "logdump") and any
  persistent marker written there is overwritten by Android after a normal boot. Nothing depends on it
  surviving.
