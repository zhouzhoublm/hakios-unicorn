# UsbfnDwc3Dxe for unicorn

A DWC3 **function** (device mode) DXE driver producing `EFI_USBFN_IO_PROTOCOL` — the transport the
loader's fastboot and any USB serial class sit on. `MdeModulePkg/Bus/Usb` has only the **host** side, so
this is the piece that has to come from somewhere else.

## Where it came from

- Upstream of this file set: **Chengdong-Yu/uefi-pong**, `Silicon/Samsung/SamsungPkg/Drivers/UsbfnDwc3Dxe/`
  (the Nothing Phone (2) Mu-Silicium firmware — same SM8475, same eUSB2 + NXP repeater path as unicorn).
- Its own `Source.txt`, carried here unchanged, names the Linux origin:
  `LineageOS/android_kernel_samsung_universal9830`, `lineage-23.2`, `drivers/usb/dwc3/{core, gadget, ep0}`.
- Survey of the alternatives (U-Boot's `drivers/usb/dwc3` + `f_acm`, Little Kernel's MIT-licensed
  `dev/usb/class/cdcserial`, EDK2 mainline having no device-mode class driver) is in
  [docs/USB-SERIAL.md](../../../../../docs/USB-SERIAL.md).

## Licence

`SPDX-License-Identifier: GPL-2.0-only` for the four files taken from that tree (`UsbfnDwc3Dxe.c`,
`Dwc3Core.c`, `Dwc3Endpoint.c`, `UsbfnDwc3Dxe.h`) and `Include/Device/Dwc3.h`. They are kept in their own
directory, with their provenance written down, so the rest of this repository's licence is unaffected and
the boundary is visible in the tree. If that mix is ever unacceptable, this directory is the whole
component: it can be dropped from `unicorn.fdf`, and the loader falls back to
`usb: device protocol not found`.

## What was changed for unicorn

1. **`UsbfnDwc3Dxe.inf`**: `SamsungPkg` → `unicornPkg` in the package list; `gEfiChipInfoProtocolGuid`
   removed from `[Protocols]`.
2. **`Dwc3BuildSerialNumber()`**: no ChipInfo provider exists in this firmware, and the protocol was used
   for nothing but the serial-number string, so the function now keeps the built-in constant and says so.
3. Names: `mManufacturerName` / `mProductName` say HakiOS / Xiaomi 12S Pro (unicorn).

Nothing else — in particular `Dwc3Core.c`, `Dwc3Endpoint.c` and the EP0 state machine are untouched.

## Known compile-time risk, and the one-line fix

The sources use the **deprecated** debug constants `EFI_D_ERROR` / `EFI_D_WARN` / `EFI_D_INFO`, which a
recent EDK2 names `DEBUG_ERROR` / `DEBUG_WARN` / `DEBUG_INFO`. If Mu-Silicium at `uefi/BASE` no longer
defines the old names, the build fails in `UsbfnDwc3Dxe.c` and `Dwc3Endpoint.c` with "undeclared
identifier"; the fix is mechanical:

```sh
sed -i 's/EFI_D_/DEBUG_/g' Drivers/UsbfnDwc3Dxe/*.c
```

Not applied pre-emptively, because if the old names still exist the sed is noise in the diff of a file whose
provenance we want to keep readable.

## What it asks the SoC for

Four functions, in `Library/PlatformUsbfnLib` (ours, in this repository):

| Function | unicorn |
|---|---|
| `UsbfnPlatformGetData` | DWC3 at `0x0A600000`, high speed only, 8-bit UTMI+, VID:PID `18d1:d00d` |
| `UsbfnPlatformPhyInit` | USB30 GDSC → USB clocks → QUP SE5 votes → eUSB2 PHY reset check → power-on reads |
| `UsbfnPlatformPhyExit` | no-op on purpose (the next owner wants the PHY running) |
| `UsbfnPlatformIsVbusPresent` | `TRUE`, so the driver falls back to the controller's link state |

`Dwc3StartController()` calls `UsbfnPlatformPhyInit()` and only then touches a controller register, so the
board sequence cannot be reordered by accident.

## How to tell it worked

Nothing consumes `EFI_USBFN_IO_PROTOCOL` yet, so loading this driver changes nothing by itself — which is
exactly what makes it safe as a first step. It is proven when a consumer exists: the loader (see
`xnuloader/usbfn.c` in the common repository) calls `StartController()` and then `ConfigureEnableEndpoints()`,
and the PC sees a new USB device (`tools/pc/boot-attempt.ps1` logs that moment even if Windows has no driver
for it yet).
