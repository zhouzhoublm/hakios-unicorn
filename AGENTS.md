# Instructions for AI coding agents (Claude Code, Codex, Cursor, ...)

You are working on the HakiOS port to the Xiaomi 12S (mayfly, SM8475). Read this first, then
`docs/FINDINGS.md` (hardware facts and traps) and `docs/OPEN-ISSUES.md` (what to work on).

## Layout

- This repository is checked out at `devices/mayfly/` inside the HakiOS common repository
  (`hakios-xnu-kernel`). Run builds from the **top** of that repository, not from here.
- Phone-specific kernel drivers: `drivers/*/hakios.*.c` here. They are copied into `xnu-src/` at build time;
  `make kernel` sometimes misses a change: copy `drivers/gpu/hakios.adreno7.c` to
  `xnu-src/xnu-8796.121.2/bsd/dev/` by hand if a change does not show up.
- Common code (loader `xnuloader/loader.c`, framebuffer `MiniIOMobileFramebuffer.cpp`, audio
  `drivers/audio/hakios.{adsp,sound,speaker}.c`, Wi-Fi `wifi/lx/`, userland `userland/src/`) is in the common repo.
  Kernel sources are edited in `xnu-src/`; run `xnu/export.sh` before committing so `xnu/hakios.patch` and
  `xnu/files/` match.
- Device-specific branches in common drivers use `#ifdef HAKIOS_ADRENO7` (set when the mayfly GPU driver is built).

## Hard rules

- **Never flash** boot, vendor_boot, persist, modem, xbl, abl or any boot-chain partition. Boot only with
  `fastboot boot` (RAM). The only partition we write is `userdata` (Darwin root), and only with the owner's consent.
- **Never commit firmware** (fw-gpu, fw-wifi, fw-audio, fw-bt, `uefi/files/Binaries/*`), Apple files (IPSW,
  sysimage, *.apfs, *.dmg), boot images (`*.img`, `*.bin` payloads) or backups. `.gitignore` covers these; check
  `git status` before every commit.
- **Never commit personal data**: MAC addresses, Wi-Fi SSIDs/BSSIDs, serial numbers, IMEI, device names, local
  paths. Scrub logs before adding them (see `logs/README.md` for the conventions).
- Keep test runs short (the owner waits for them): `tools/run.sh NAME 120` (iOS needs ~70 s to reach the home
  screen). Tell the human when a run will end: `oem stop` stops everything and looks like a crash.
- Let the phone cool between runs; it resets under heavy load near 90 °C.
- Images **must** be packed with `mkpayload.py --usb` (build-image.sh does) or there is no USB log channel.

## Build and run (from the top of hakios-xnu-kernel)

```sh
export PATH=/opt/homebrew/opt/llvm/bin:$PWD/xnu-deps/host/bin:$PWD/tools/macos-bin:/opt/homebrew/opt/bison/bin:$PATH
LX_CHIP=qca6490 tools/mk-ath11k.sh        # never without LX_CHIP (else WCN6750 and no Wi-Fi)
make kernel
OSLOG="" ADSP=1 BT=1 CRYPTEXES=flash FLASHBOOT=1 REST=1 GL=1 userland/build.sh
LOADER_CFLAGS=-DPAYLOAD_BASE=0xB0000000ULL xnuloader/build.sh   # only if the loader changed
devices/mayfly/tools/build-image.sh NAME "boot-args"
devices/mayfly/tools/run.sh NAME 120      # phone in ABL fastboot first (power + volume down)
```

Logs land in `$LOGDIR/NAME/{live,final}` (default `~/hakios-mayfly-logs`). Read them with
`LC_ALL=C tr -d '\0' < final | grep -a 'sound: \|lx: \|qcabt\|adreno7'`. The buffer wraps at 1.4 MB: early lines
of long runs are lost (adsp.c re-prints its notes at stop).

## How to work

- Read before you write: every register access in these drivers says where its value comes from (Android
  dumps in `reference/android/`, LineageOS DTS, upstream Linux). Do the same for new ones.
- Prefer reads before writes and one checked step at a time (`adreno7=N`-style step boot-args) for new hardware.
- When the phone resets, first make sure it was a reset (an unplugged cable looks the same).
- Compare with Android when stuck: the amplifier fix came from dumping its registers while Android played.
- State what is proven and what is a guess. Several "obvious" causes in FINDINGS.md turned out wrong.
- Commit messages, comments and docs are in English.
