# USB serial log: what already works on this class of hardware, and the path for unicorn

The requirement, stated plainly: **a serial port on the PC that carries our own text out of this phone, with no
root on the phone, no Android underneath, and as early in boot as possible.** This file is the survey of
projects that already do it (fetched 2026-11-06) and the stack chosen from them. It supersedes the "B2" choice
in docs/USB-BRINGUP.md §2, for the reason in §3 below.

## 1. What each project actually contributes

| Project | What is in it | Licence | Fit for us |
|---|---|---|---|
| **[Chengdong-Yu/uefi-pong](https://github.com/Chengdong-Yu/uefi-pong)** — Nothing Phone (2), **SM8475** | `Silicon/Samsung/SamsungPkg/Drivers/UsbfnDwc3Dxe/` — a **source** DXE driver: `UsbfnDwc3Dxe.c` 24 KB, `Dwc3Core.c` 9.4 KB, `Dwc3Endpoint.c` 22.7 KB, `Include/Device/Dwc3.h` 13.9 KB. It produces the standard `EFI_USBFN_IO_PROTOCOL` (`DetectPort`, `ConfigureEnableEndpoints`, `Transfer`, `EventHandler`, `AllocateTransferBuffer`, `StartController`, …). Its `Source.txt` names its provenance: **LineageOS `android_kernel_samsung_universal9830`, `drivers/usb/dwc3/{core.c,gadget.c,ep0.c,core.h,gadget.h}`**. Plus `Include/Library/PlatformUsbfnLib.h`: a **four-function board contract**. Its firmware also ships Qualcomm's `UsbDeviceDxe`/`UsbConfigDxe` to turn `UsbFnIo` into the device protocol its DarwinBoot loader speaks | GPL-2.0 derivative published in-tree, with per-file SPDX and `Source.txt` | **The best fit that exists**: same SoC, same eUSB2 + NXP repeater, and *all* board specifics are already isolated into four functions |
| **[U-Boot](https://github.com/u-boot/u-boot)** | `drivers/usb/dwc3/` device mode (`core.c` 31 KB, `ep0.c` 27 KB, `gadget.c` 69 KB, `gadget.h`, `core.h`), `drivers/usb/gadget/f_acm.c` 16.5 KB (**CDC ACM function**), `f_fastboot.c` 15.5 KB. Runs on DragonBoard 845c / OnePlus 6 — the same DWC3 IP | GPL-2.0+ | The authoritative reference for what device mode must do, and the shape of a CDC ACM function |
| **[Little Kernel / lk2nd](https://github.com/littlekernel/lk)** | `dev/usb/usb.c` (gadget core), `dev/usb/class/cdcserial/cdcserial.c` (10 KB, **CDC serial class**), `dev/usb/class/bulktest` (the fastboot bulk transport). Production bootloader on many Qualcomm phones | **MIT** | **Licence-clean** reference for the class side — this is the one we may read and re-express freely |
| **[Chengdong-Yu/pong-usb](https://github.com/Chengdong-Yu/pong-usb)** | The board power sequence for eUSB2 + NXP repeater + DWC3 (32 KiB DMA, SID 0), and the PMIC `SDAM7100` bounded reset | GPL-2.0-only | Facts only — see docs/USB-BRINGUP.md §5 |
| **EDK2 mainline** | Checked `MdeModulePkg/Bus/Usb`: `UsbBusDxe`, `UsbKbDxe`, `UsbMassStorageDxe`, `UsbMouseDxe`, `UsbBotPei`, `UsbBusPei`, `UsbNetwork` — **all host side; there is no device-mode class driver** | BSD-2-Patent | Means the class/EP0 layer is either Qualcomm's binary or ours to write |

## 2. The shape those projects agree on

```
        our text (Print / DebugLib / loader log)
                      │
        class driver: EP0 standard requests + endpoints      ← ours (lk2nd's cdcserial as design ref)
                      │  EFI_USBFN_IO_PROTOCOL
        UsbfnDwc3Dxe: DWC3 device mode (TRBs, GEVT, DCFG)     ← uefi-pong, source, in-tree
                      │  four board functions
        PlatformUsbfnLib: power, clocks, QUP, repeater, PHY   ← ours, unicorn's numbers
                      │
        DWC3 0xa600000 · QUP 0x994000 · repeater 0x4f · eUSB2 PHY 0x88e3000
```

The four functions `PlatformUsbfnLib.h` asks for, and what unicorn puts in each:

| Function | unicorn |
|---|---|
| `UsbfnPlatformGetData` | `ControllerBase 0xa600000`; `MaximumSpeed = UsbBusSpeedHigh` (the SS/combo PHY is not brought up, `SuperSpeedPhyDisabled = TRUE`); `UtmiWidth = 8`; free-running UTMI clock **to be confirmed by register read, not assumed**; `FrameLengthAdjustMask/Value` derived for our 19.2 MHz reference (not copied from another board); `NumEndpoints = 0` → read from `GHWPARAMS3`; `VendorId 0x18d1`, `ProductId 0xd00d` (what the loader already reports as `xnuloader`) |
| `UsbfnPlatformPhyInit` | docs/USB-BRINGUP.md §4 steps 1–5, with this phone's values: rails `ldob5`/`ldoc10` + `S10B`/`L2B`, USB30 GDSC `GCC+0x49004`, USB clocks, QUP SE5 at `0x994000` (pads, GENI I2C firmware check), NXP repeater `0x4f` (ID read, then `{6:0x40, 7:0x22, 8:0x63}`), eUSB2 PHY `0x88e3000` 19.2 MHz program |
| `UsbfnPlatformPhyExit` | reverse, best-effort, never fatal |
| `UsbfnPlatformIsVbusPresent` | `TRUE` for now (no VBUS sampler yet); the driver then falls back to the controller's link state |

## 3. Decision: B3 — take the open DWC3 device driver, write only the class

So the stack is: **uefi-pong's `UsbfnDwc3Dxe` (source) + our `PlatformUsbfnLib` + our class driver**, with the
loader as the first client (it already has descriptors and a command loop).

- **no vendor blobs** — the DWC3 code is source, and its provenance is written down
- **board specifics stay in four functions**, with numbers already verified from this phone's DT and sysfs
- the loader only changes five primitives (`start_ex`/`send`/`handle_event`/`allocate_transfer_buffer`/stop):
  they move from Qualcomm's `EFI_USB_DEVICE_PROTOCOL` to `UsbFnIo`, which is standard EDK2
- the same transport carries a **CDC ACM interface**, which is the actual "USB serial log", for the firmware
  *and* the loader

Why not the earlier options: **B1** (vendor DXE stack: `UsbDeviceDxe`/`UsbConfigDxe`/`UsbfnDwc3Dxe` binaries)
was already attempted in this project's earlier lineage and had to be **reverted** — the "USBSERIAL STACK"
block was pulled out of the FDF to make the phone bootable again. **B2** (write DWC3 from the databook
ourselves) is unnecessary work now that a working open implementation for this exact SoC exists.

## 4. Concrete work list

**Status 2026-11-06: written, not yet built.** Everything below exists in this repository; the next step is
a CI run of `build_uefi.py` and then one `fastboot boot`.

In `unicornPkg` (firmware) — done:
1. `Drivers/UsbfnDwc3Dxe/` — the three sources + `.inf` + `Source.txt` + `README.md` (provenance and
   licence stated there; the only changes are the package list and the dropped ChipInfo dependency).
   Listed in `Include/DXE.inc` and `unicorn.dsc` `[Components]`.
2. `Library/PlatformUsbfnLib/` — our four functions, `unicorn.dsc` `[LibraryClasses]`.
   `Include/Device/Dwc3.h` and `Include/Library/PlatformUsbfnLib.h` are published through the package
   description, which had to be **renamed `unicorn.dec` → `unicornPkg.dec`** and given `[Includes] Include`:
   EDK2 finds a package's description as `<dir>/<dir>.dec`, and CI run #24 failed on exactly that
   (`UsbfnDwc3Dxe.inf(29): File/directory not found in workspace`).
3. `Drivers/UsbSerialDxe/` — the class: a CDC ACM console on top of `EFI_USBFN_IO_PROTOCOL`, installed as
   `EFI_SERIAL_IO_PROTOCOL` and serviced from a periodic timer. This is the piece that actually produces
   the serial port, and it lives in the firmware on purpose: the loader's own USB path needs Qualcomm's
   device protocol, while this needs nothing but what `UsbfnDwc3Dxe` provides.
4. `USE_CUSTOM_DISPLAY_DRIVER = 0` and `SimpleFbDxe` unchanged: the screen stays the second channel.

In the loader (common repository) — written, to be copied in:
4. `xnuloader/usbfn.c` (delivered in this repository) — CDC ACM class over `EFI_USBFN_IO_PROTOCOL`:
   descriptors, the class requests, the bulk data path and a bounded, non-blocking `put_char()`.
5. Three edits to `loader.c` (README in `xnuloader/` has them verbatim): declare the three functions, tee
   `put_char()` into the serial, and make `usb_start()` fall back to `usbfn_serial_start()` when the vendor
   protocol is absent — replaying the log buffer that was printed before the port existed. Plus
   `usbfn_poll()` in the main loop and `usbfn.c` in `build.sh`.

## 5. Verification ladder (each rung is one boot, ~2 minutes)

| Rung | Expected on the PC |
|---|---|
| firmware links with `UsbfnDwc3Dxe` | `dxe` module list in the FD contains `UsbfnDwc3Dxe` (parsed from the FD, as docs/BUILD.md already does) |
| `UsbfnPlatformPhyInit` succeeds | nothing yet — but the screen shows the loader / next stage, and the phone no longer resets at ~40 s unless something else fails |
| controller started | **a USB device appears in the PnP watch** — `tools/pc/boot-attempt.ps1` logs this moment even if Windows has no driver for it yet |
| class + descriptors | Windows binds "USB Serial Device" → `\\.\COMx` (or at least shows `18d1:d00d` in the device tree) |
| text | the loader's first `print()` arrives on the COM port — the log channel is real |

The harness needs one addition, and it is small: after each attempt, also list serial ports
(`Get-CimInstance Win32_PnPEntity | ? { $_.Name -match 'COM\d+' }`) so the serial device is recorded even when
`adb`/`fastboot` show nothing.

## 6. Open questions, stated rather than hidden

- Does our Mu build provide `gEfiChipInfoProtocolGuid`? `UsbfnDwc3Dxe.inf` consumes it and declares
  `Depex = TRUE`; if it is missing the driver loads and asserts. Check in the CI log before flashing anything.
- The `GFLADJ` value for a 19.2 MHz reference clock has to be derived from the DWC3 databook formula; a wrong
  value shows up as drifting frame timing, not as a boot failure.
- `UsbfnDwc3Dxe` must not touch the QUP or the repeater itself — `PhyInit` owns them. Confirm by reading its
  `Dwc3Core.c` before enabling.
- **Licence**: the three `UsbfnDwc3Dxe` files are a GPL-2.0 derivative. They stay in their own directory with
  their SPDX headers and `Source.txt`, so the rest of the firmware is unaffected and the boundary is visible in
  the tree. If that mix is unacceptable for this repository, the same driver can live in a separate component
  that only the FDF pulls in — the loader side does not care either way.
