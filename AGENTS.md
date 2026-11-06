# Instructions for AI coding agents (Claude Code, Codex, Cursor, ...)

You are working on the HakiOS port to the Xiaomi 12S Pro (unicorn, SM8475). Read this first, then
`docs/FINDINGS.md` (what this phone is, source by source) and `docs/OPEN-ISSUES.md` (what to work on).
The port is derived from [cotunjr/hakios-mayfly](https://github.com/cotunjr/hakios-mayfly) (the Xiaomi
12S, same SoC): that repository is the reference for *how*, never for *what this phone's addresses and
hardware are* — those come from this phone's own device tree.

## Layout

- This repository is checked out at `devices/unicorn/` inside the HakiOS common repository
  (`hakios-xnu-kernel`). Run builds from the **top** of that repository, not from here.
- The UEFI platform is `uefi/files/Platforms/Xiaomi/unicornPkg` (memory map, FDF module list, DSC
  settings, SMBIOS, framebuffer size), the boot shim's config is
  `uefi/files/Resources/Configs/unicorn.toml`, and the device tree is
  `uefi/files/Resources/DTBs/unicorn.dtb` (built from source — `docs/FINDINGS.md` has the command).
- Phone-specific kernel drivers, when they exist: `drivers/*/hakios.*.c` here, declared in
  `drivers/<subsystem>/conf`. Today `drivers/input/hakios.stfts.c` (touch) and the GPU, Wi-Fi and
  Bluetooth sources are the 12S port's, kept as the point to start from: each says in its `conf` what
  has to change for this phone, and none of them is switched on (`configs/unicorn.config`).
- Common code (loader `xnuloader/loader.c`, framebuffer `MiniIOMobileFramebuffer.cpp`, audio
  `drivers/audio/hakios.{adsp,sound,speaker}.c`, Wi-Fi `wifi/lx/`, userland `userland/src/`) is in the
  common repository.
- Device-specific branches in common drivers use `#ifdef HAKIOS_ADRENO7` (set when this device's GPU
  driver is built).

## Hard rules

- **Never flash** boot, vendor_boot, persist, modem, xbl, abl or any boot-chain partition. Boot only with
  `fastboot boot` (RAM). The only partition we write is `userdata` (Darwin root), and only with the
  owner's consent.
- **Never commit firmware** (fw-gpu, fw-wifi, fw-audio, fw-bt, `uefi/files/Binaries/*`), Apple files
  (IPSW, sysimage, *.apfs, *.dmg), boot images (`*.img`, `*.bin` payloads) or backups. `.gitignore`
  covers these; check `git status` before every commit.
- **Never commit personal data**: MAC addresses, Wi-Fi SSIDs/BSSIDs, serial numbers, IMEI, device names,
  local paths. Scrub logs before adding them (`logs/README.md`).
- **Never claim a part works because the 12S port says it does.** `device.conf` says `works` only after
  it has been seen on *this* phone; an inherited fact goes into `docs/FINDINGS.md` under "inherited
  from the 12S port", never into a status line.
- Keep test runs short (the owner waits for them); say when a run will end (`oem stop` ends everything
  and looks like a crash).
- Images **must** be packed with `mkpayload.py --usb` (or `tools/pack-image.py`, which does) or there is
  no USB log channel.
- Never commit the vendor UEFI drivers or a built `SILICIUM_UEFI.fd`.

## Build and run (from the top of the common repository)

```sh
uefi/apply.sh unicorn                                  # Mu-Silicium + HakiOS + this platform (docs/BUILD.md)
cd Mu-Silicium && python3 build_uefi.py -d unicorn -r DEBUG && cd ..
devices/unicorn/tools/pack-image.py --selftest -o xnu-unicorn-selftest.img   # UEFI + loader, no kernel
fastboot boot xnu-unicorn-selftest.img                 # phone in ABL fastboot (power + volume down)
fastboot oem log > log.txt                             # what the loader saw
```

With a kernel built for this phone, `devices/unicorn/tools/build-image.sh NAME "boot-args"` packs the
full image, and `devices/unicorn/tools/run.sh NAME 120` boots it, runs it and pulls the log.

## How to work

- Read before you write: every value in `unicornPkg` and in the docs says where it came from — this
  phone's device tree, or the 12S port's measurement. Do the same for anything new, and prefer the
  phone's own source (`reference/README.md` says how to pull it).
- One checked step at a time (`adreno7=N`-style boot arguments) for new hardware; reads before writes.
- When the phone resets, first make sure it was a reset (an unplugged cable looks the same).
- Compare with Android when stuck: dump the registers of the part while Android drives it.
- State what is proven and what is a guess. The 12S port's `docs/FINDINGS.md` is a list of "obvious"
  causes that turned out wrong.
- Commit messages, comments and docs are in English.
