# USB serial console for the loader

`usbfn.c` in this directory is the class side of the USB log channel: a **CDC ACM serial port** driven
through the firmware's `EFI_USBFN_IO_PROTOCOL` (provided by `unicornPkg/Drivers/UsbfnDwc3Dxe`, see
[docs/USB-SERIAL.md](../docs/USB-SERIAL.md)). With it, every `print()` in the loader is read on the PC as
text on a `COM` port, without root, without Android and without any vendor binary.

Why a new file instead of a patch to `usb_start()`: the loader's own USB path uses **Qualcomm's**
`EFI_USB_DEVICE_PROTOCOL`, which a blob-free `unicornPkg` does not provide — that is why nothing ever
enumerated. `EFI_USBFN_IO_PROTOCOL` is the *standard* EDK2 device-mode protocol, which the firmware can
provide from source.

## Where this file goes

The loader lives in the common repository (`hakios-xnu-kernel/xnuloader/`), not here. This directory is
the delivery: copy `usbfn.c` there and make the four edits below.

### 1. `xnuloader/build.sh` — compile and link it

```sh
clang ... -c loader.c -o loader.obj
clang ... -c usbfn.c -o usbfn.obj            # add
lld-link ... /out:loader.efi loader.obj usbfn.obj   # add usbfn.obj
```

### 2. `xnuloader/loader.c` — the three hooks

**(a)** after `#include "identity.h"`:

```c
/* USB serial console over the firmware's EFI_USBFN_IO_PROTOCOL (usbfn.c) */
int  usbfn_serial_start(void);
void usbfn_put_char(char c);
void usbfn_poll(void);
```

**(b)** at the end of `put_char()` — every character the loader prints, including from the framebuffer
console, also goes to the serial ring:

```c
	if (log_len < sizeof(log_buf) - 1) {
		log_buf[log_len++] = c;
	}
	usbfn_put_char(c);                      /* <-- add */
	if (ST->con_out != NULL && !screen_quiet) {
```

**(c)** in `usb_start()`, where the vendor protocol is missing — this is the line that has been printing
`usb: device protocol not found` on every boot so far:

```c
	if (EFI_ERROR(BS->locate_protocol(&usb_guid, NULL, (void **)&USB))) {
		print("usb: device protocol not found\n");
		/*
		 * This firmware has no Qualcomm device protocol (blob-free unicornPkg). Try the standard
		 * EFI_USBFN_IO_PROTOCOL: the firmware's UsbfnDwc3Dxe provides it and usbfn.c drives it as a
		 * CDC ACM serial port (docs/USB-SERIAL.md).
		 */
		if (!usbfn_serial_start()) {
			print("usb: no usbfn either, screen only\n");
			return 0;
		}
		for (uint64_t i = 0; i < log_len; i++) {
			usbfn_put_char(log_buf[i]);     /* replay what was printed before the port existed */
		}
		usbfn_poll();
		print("usb: serial console up (CDC ACM)\n");
		return 0;                               /* the fastboot loop still needs the vendor protocol */
	}
```

**(d)** next to the existing `usb_poll_command();` call in the main loop, add `usbfn_poll();` so the port
stays alive while the loader runs (it is what keeps enumerating the CDC data stage and re-arming the
receive endpoint).

## What the host should see

| Stage | Windows |
|---|---|
| firmware started the controller | a new USB device appears (no driver yet) — `tools/pc/boot-attempt.ps1` timestamps it |
| class answered enumeration | "USB Serial Device (COMx)" — the harness records serial ports in `serial-ports.txt` |
| first text | the loader's banner and its `[cpu] [display] [memmap] [payload]` lines, as plain text |

## Limits of this first cut, on purpose

- **Transmit only.** The console writes; whatever the host types is read and dropped (the bulk OUT
  endpoint is re-armed, its data ignored). A shell would need the other direction.
- **Bounded waits.** Every flush gives up after ~40 ms so a host that stops reading can never stall the
  loader; characters are dropped, not buffered without limit.
- **No notification endpoint traffic.** The interrupt IN endpoint exists in the descriptor set because
  the CDC ACM class needs one, but no serial-state notifications are sent yet.
- **The fastboot command loop is unchanged** and still needs Qualcomm's protocol; `oem log`/`get_staged`
  come later, once this port is proven (they can ride the same controller).

## Licence

`usbfn.c` is this repository's own work, MIT, like `tools/`. It talks to `EFI_USBFN_IO_PROTOCOL` as
`MdePkg/Include/Protocol/UsbFunctionIo.h` defines it (BSD-2-Clause-Patent) and re-implements nothing from
the GPL driver in the firmware.
