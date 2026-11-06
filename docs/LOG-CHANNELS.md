# How a boot on this phone is observed

Written 2026-11-06 after the first two `fastboot boot` attempts on the phone itself (previous documents in this
repository were written before anything had been started). Everything in the first section was measured
tonight; the rest is the decision that follows from it.

The question this file answers: *when this phone fails to boot, what can we actually read, and in which
order do we build the missing channels?* It is the working document for LOG-CHANNELS work; docs/OPEN-ISSUES.md
keeps the hardware ladder.

## 1. Measured (Xiaomi 12S Pro, `unicorn`, 2206122SC, Android 13 V14.0.11.0)

| # | Fact | How it was seen |
|---|---|---|
| 1 | Bootloader is unlocked and `fastboot boot` accepts our images | `fastboot getvar unlocked` → `yes`; `fastboot getvar all` → `verifiedbootstate=orange`, `secure:yes`, `kernel:uefi`, `socid:530`; `fastboot boot xnu-unicorn-selftest.img` → `Sending 'boot.img' (948 KB) OKAY [0.022s] / Booting OKAY [0.288s] / Finished. Total time: 0.428s` |
| 2 | **Nothing our image does reaches USB** | A 0.5 s poll of every USB PnP device for 65 s after the handoff: the ABL interface (`18d1:d00d`) disappears at t+4.4 s and **no device appears at all** — no `xnuloader`, no unknown device, no descriptor failure — until Android's adb interface returns at t+63.4 s (i.e. the phone hung and was reset, then booted Android) |
| 3 | ABL has **no** log channel: no OEM commands at all | `fastboot oem help` → `FAILED (remote: 'unknown command')`; `fastboot oem log` → same. `oem run`, `oem log`, `get_staged`, `oem stop` in `tools/run.sh` are the **loader's** fastboot commands (`xnuloader`), not ABL's — they exist only once the loader is up |
| 4 | **Nothing on Android can read pstore on this build** | `ls /sys/fs/pstore` → `Permission denied`; `which su` → nothing; `getenforce` → `Enforcing`; `dmesg` → `klogctl: Permission denied`; `cat /proc/cmdline` → `Permission denied`. Even `incidentd` — the system service that is supposed to carry pstore into a bugreport — is refused: `avc: denied { search } for name="/" dev="pstore" tclass=dir scontext=u:r:incidentd:s0 tcontext=u:object_r:pstorefs:s0`, `GZipSection failed to open file /sys/fs/pstore/console-ramoops-0`. So `adb bugreport` is **not** a read path for pstore here |
| 4b | Android's own crash log is flash-backed, not RAM-backed | its kernel cmdline: `block2mtd.block2mtd=/dev/block/sda15,2097152 mtdoops.mtddev=0 mtdoops.record_size=2097152 mtdoops.dump_oops=0 printk.always_kmsg_dump=1 pstore.compress=none log_buf_len=2M console=ttyMSM0,115200n8` — i.e. Xiaomi dumps panics through `mtdoops` into a 2 MiB record on a flash partition, plus the 4 MiB ramoops; both reachable only from root, and `console=ttyMSM0` has no UART behind it on a phone |
| 5 | The phone's own ramoops node is write-by-us-able | `xiaomi-sm8475-common.dtsi`: `ramoops@0xa7000000`, `reg = <0 0xa7000000 0 0x400000>`, `console-size = <0x200000>`, `pmsg-size = <0x200000>` — **no `record-size`, no ECC**: plain Linux `persistent_ram` zones, no CRC to fake |
| 6 | **The UEFI image owns that memory** | `MemoryMapLib.c`: `FD Reserved 0xA7000000+1MiB`, `UEFI FD 0xA7100000+3MiB`, `FD Reserved 0xA7400000+2MiB` — `0xA7000000 + 4 MiB`, the whole ramoops region, is handed to the firmware and reloaded from the boot image on every attempt |
| 7 | After a failed attempt the phone returns to **Android**, not to fastboot | Both attempts; `ro.boot.bootreason=bootloader`, `slot-successful:a=yes`, `slot-retry-count:a=7` unchanged |
| 8 | Partitions that look like log sinks, for later | `logfs` 8 MiB, `oops` 16 MiB, `crash_history` 58 MiB, `minidump` 96 MiB, `rawdump` 300 MiB, `dbg`, `devinfo`, `misc` (all raw, all root-only from Android) |

Three attempts, the same outcome each time, reproduced by `tools/pc/boot-attempt.ps1`:

```
23:01:43  handoff: fastboot boot out/xnu-unicorn-selftest.img
23:01:47  +   3.8s  GONE    USB\VID_18D1&PID_D00D   # ABL's fastboot has handed over
23:02:45  +  62.0s  APPEAR  USB\VID_18D1&PID_4EE7   # Android's adbd: nothing of ours ever enumerated
```

**The port is blind from BootShim's first instruction until the loader's USB comes up** — and the same
run to Android also costs the RAM contents, which is why §5 Stage C starts with a reset target we choose.

## 2. What each channel can and cannot do

| Channel | Alive from | Read by | Verdict for this port |
|---|---|---|---|
| ABL fastboot (`getvar`, `download`, `flash`) | ABL | the PC | No logs at all on this ABL (fact 3). Useful only to *load* images |
| **Loader fastboot** (`xnuloader`: `oem log`/`oem run`/`get_staged`/`oem stop`) | loader start | the PC, over the same cable | **The target channel.** Rich, real-time, works from this PC, no root, no Android. Needs DWC3 gadget up in the loader — not reached yet |
| Screen text / QR (Advanced Logger) | first pixel written by UEFI | a camera + OCR | Works even when everything else is dead; needs eyes. The 12S port's firmware already draws text and QR codes on this phone's screen and it decodes reliably |
| `ramoops` → Android `/sys/fs/pstore` | a writer that is alive at the failure point | root only (fact 4) | Good for the *kernel* stage (stage 4+), not for stage 0-2 (see §3) |
| RAM read-back: EDL (9008) + firehose | a reset target our code can set, or the key combo | host tool + signed `prog_firehose_ddr.elf` for SM8475 | Full-fidelity post-mortem of *any* failure, including a hung CPU. Highest value, highest one-time setup cost |
| RAM read-back: RAM-booted recovery (TWRP) with root adb | recovery kernel | the PC | Gives root for `/sys/fs/pstore`, raw partitions and `/dev/mem`; RAM only, nothing flashed |
| Our own next loader run (it owns UFS) | loader | the PC via loader fastboot | Lets a log **persist across attempts** (write to `logfs`, read it back next time) — but it still needs the loader channel to deliver it |

## 3. Why "dump to /sys/fs/pstore" is not the first move on this phone

Four independent reasons, in order of how fatal they are:

1. **The writer must be alive.** Today's failure is *before* any console exists: no USB, no text of ours
   reaching anything. Code that cannot bring up USB is not obviously better at writing a valid
   `persistent_ram` record *and* arranging the reset. A dump helps the stage where the kernel runs but its
   console does not — that is stage 4, not stage 0.
2. **The reader must have root.** `/sys/fs/pstore` is 0600 root and SELinux blocks the shell (fact 4). So even
   a perfect dump needs one of: a RAM-booted recovery with root adb, EDL, or `dumpstate`/`bugreport`
   carrying the file. That is a decision plus a one-time asset, not a code change.
3. **The region is already taken.** `0xA7000000 + 4 MiB` is the UEFI's own memory in this port (fact 6): every
   attempt overwrites the ramoops region before Android can own it again. Using it means moving the FD or the
   dump, i.e. changing the memory map and the `.toml` first.
4. **Android owns it after the reset.** The phone comes back to Android (fact 7), whose kernel is the ramoops
   owner and appends console data to the same 2 MiB zone. A record written by our firmware is a guest in a ring
   buffer that Android is about to write into.

**Conclusion: pstore is a stage-4 channel.** It belongs with the kernel breadcrumbs, not with the bring-up.

## 4. Decision

**USB first, and loader-side USB specifically** — that is the only channel that is rich, real-time, root-free
and AI-operable.

What that concretely costs was settled after this file was first written, by reading the common repository's
loader: `usb_start()` asks the firmware for `QCOM_USB_DEVICE_PROTOCOL` and gives up with
`usb: device protocol not found` when it is absent — and this firmware builds only the USB **host** stack, so
it is absent. **The loader contains no DWC3 transport of its own**, and the 12S port's firmware is the same.
So "a channel" means putting a device-mode stack somewhere. docs/USB-BRINGUP.md lists the board facts and the
two original routes, and **docs/USB-SERIAL.md supersedes them with the third**: take the *open source* DWC3
device-mode DXE driver that already runs on this SoC (`uefi-pong`'s `UsbfnDwc3Dxe`, four board functions
isolated in `PlatformUsbfnLib`), implement only the class/serial side ourselves, and the board sequence we
need is known to work on identical silicon:

- UniXNU-pong is a Nothing Phone (2) — **the same SM8475, and the same `eUSB2 + NXP repeater on the QUP SE5
  bus`** that this phone's tree documents (`xiaomi-sm8475-common.dtsi`, `qupv3_se5_i2c`, `0x4f`), and the
  live phone's sysfs confirms every address (`994000.i2c`, `88e3000.hsphy`, `a600000.dwc3`,
  `15000000.apps-smmu`, `GCC 100000`, rails `ldob5`/`ldoc10`).
- Its [`pong-usb`](https://github.com/Chengdong-Yu/pong-usb) is a working DWC3 device stack on exactly that
  board; its PHY/repeater order and values are the reference — **as facts to re-derive, not as code to
  copy: that file is GPL-2.0-only and this loader is not** (docs/USB-BRINGUP.md §5).
- Its [`pong-boot-capture`](https://github.com/Chengdong-Yu/pong-boot-capture) is the bounded-diagnostic-reset
  piece: SM8475 PMK8350, SID0, `SDAM7100`, `restart@48` `bits<1 7>`, SPMI-v7 — i.e. a known way to make the
  phone reset *to a target we choose* (recovery / fastboot / EDL) instead of to Android. MIT.

That last point matters more than it looks: today a failed attempt costs a full Android boot and loses the RAM
contents (fact 7). A chosen reset target is what makes a post-mortem possible at all.

## 5. Staged plan

**Stage A — stop being blind (no new hardware, one build).**
Make the firmware and the loader *say* where they are on the channels that exist before USB: solid full-screen
colour blocks per milestone (BootShim reached / DXE done / payload found / payload entered), plus one line of
text. Then the same information can be read by a camera and OCR, and it costs one boot attempt per bisection.
This is the only way to answer the first question — *does the UEFI reach the payload at all?* — without waiting
for USB.

**Stage B — loader USB (the target channel).**
The loader has no transport of its own today: it asks the firmware for `QCOM_USB_DEVICE_PROTOCOL`, which a
blob-free `unicornPkg` does not build, so the order inside the loader is: rails and XO → USB30 GDSC and clocks
→ the QUP SE5 bus the NXP repeater sits on → the repeater's ID and this phone's three parameters → the eUSB2
PHY at `0x88e3000` → DWC3 device mode with the loader's own 32 KiB DMA and SID 0 → EP0/descriptors (already in
`loader.c`) → bulk + fastboot. Nothing else in the loader may run before the gadget enumerates, and every step
prints to the framebuffer as it passes, because the screen is the only channel until the last step. The
evidence, the two routes and the licence boundary are in docs/USB-BRINGUP.md.

**Stage C — post-mortem that survives a reset (for stage 4 and for hard hangs).**
A fixed physical ring buffer written by the loader and by early kernel breadcrumbs, plus a bounded, one-shot
reset to a chosen target (pong-boot-capture's PMIC route). Read back by the next loader run over stage B, and —
if a root-capable reader exists — mirrored into `persistent_ram` format at `0xA7000000` so Android's own pstore
or a RAM-booted recovery shows it. This is where the pstore idea lands, with its three preconditions (writer
alive, reader rooted, region free) written down instead of assumed.

**Stage D** — the kernel ladder in docs/OPEN-ISSUES.md, on top of a working stage B.

## 6. The loop this is for

| Step | Where | Command |
|---|---|---|
| build | GitHub Actions (`build-uefi.yml`), green as of run #23 | `gh workflow run …` / dispatch, then download the artifact |
| boot + observe | this PC, one command | `tools/pc/boot-attempt.ps1 -Image out/xnu-unicorn-NAME.img` |
| judge | the log directory it writes | `usbwatch.log`, `fastboot-*.log`, `oem-log.bin`, and (stage A) the screen capture |

`tools/pc/boot-attempt.ps1` always returns the phone to Android, so an attempt is safe to run unattended and
leaves the phone usable. Sample of what it records for a failed attempt:

```
+   4.4s  GONE    USB\VID_18D1&PID_D00D\5F6D7677        # image started, ABL's fastboot gone
+  63.4s  APPEAR  USB\VID_18D1&PID_4EE7\5F6D7677        # Android came back: nothing of ours ever enumerated
```
