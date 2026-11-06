# USB: why this phone has no log channel today, and how to build one

Stage B of docs/LOG-CHANNELS.md. Written 2026-11-06 from the common repository's loader source
(`xnuloader/loader.c`, 170 KB, read at `main`), this repository's firmware module list, this phone's own
device tree, and the live phone's sysfs (root-free reads). Nothing here was guessed; where a number is a
reference from another board it says so.

## 1. What the loader actually does with USB — and why nothing enumerates

`xnuloader/loader.c`, `usb_start()`:

```c
/* Ask the platform to bring the controller up in peripheral mode (as the stock fastboot does). */
BS->signal_event(... QCOM_USB_INIT_EVENT_GUID ...);
if (EFI_ERROR(BS->locate_protocol(&usb_guid /* QCOM_USB_DEVICE_GUID */, NULL, (void **)&USB))) {
    print("usb: device protocol not found\n");
    return 0;                       /* the loader carries on: framebuffer only */
}
set.device_descriptor = usb_dev_desc;   /* 18d1:d00d, "xnuloader", 2 bulk endpoints */
...
efi_status s = USB->start_ex(&set);
```

- The loader contains **no DWC3 code at all**. Everything goes through `EFI_USB_DEVICE_PROTOCOL`:
  `start_ex`, `allocate_transfer_buffer`, `send`, `handle_event`. That protocol is provided by
  **Qualcomm's firmware DXE stack** (`UsbfnDwc3Dxe` + `UsbDeviceDxe` + board glue), not by EDK II.
- `mkpayload.py --usb` does not add a transport either: it writes one `usb=1` entry into the payload, which
  only tells the loader to stay resident and take commands. The descriptors are compiled into the loader.
- **This firmware does not build that stack.** `uefi/files/Platforms/Xiaomi/unicornPkg/Include/DXE.inc`
  lists the USB **host** side only — `UsbBusDxe`, `UsbKbDxe`, `UsbMassStorageDxe` — and `unicorn.fdf`
  adds nothing vendor-side; the repository carries no `Binaries/` (blob-free by design, docs/FINDINGS.md).
  The 12S port is the same shape (its `mayfly.fdf` is 4607 bytes against this one's 4609, same module
  list), so this is **not** a unicorn regression.

**So the ~40 s of silence measured on 2026-11-06 is not a broken eUSB2 PHY or a bad repeater: the protocol
the loader asks for does not exist in this image, and the loader falls back to the screen.** The line to
look for on the screen is `usb: device protocol not found`. (Which is also the cheapest single test that
the whole chain BootShim → UEFI → payload → loader is alive: Stage A of docs/LOG-CHANNELS.md.)

## 2. Two routes, and the one this port takes

| | **B1 — give the firmware the vendor USB device stack** | **B2 — run DWC3 device mode inside the loader** |
|---|---|---|
| What it needs | `UsbfnDwc3Dxe`, `UsbDeviceDxe` and their dependencies: PCI root bridge + PCI I/O, IOMMU/SMMU, SMEM, clock, power/GDSC, TLMM, PMIC arbiter, RPMh, … | board power-on (GDSC, clocks, QUP, repeater, eUSB2 PHY), then DWC3 device mode, EP0 and a bulk transport, with the loader's own DMA buffers and SID 0 |
| Blobs | yes — another phone's Qualcomm DXE binaries, plus a redistribution question | no |
| Already measured | the earlier attempt at this found the stack asserting because the rest of the vendor platform is not there | — |
| Effect on the loader | `usb_start()` works unchanged | `usb_start()` grows a fallback that owns the controller itself |
| Verdict | not taken — earlier attempt in this project's previous lineage had to be reverted | superseded: see **docs/USB-SERIAL.md** (take the open `UsbfnDwc3Dxe` source, write only the class) |

Why B2: it is self-contained (nothing to license from Qualcomm, nothing that can assert in someone else's
DXE), it produces the earliest possible channel (the loader is the first code of ours that can be
interactive), and it is the same shape as UniXNU-pong's working `pong-usb` on identical silicon — so the
board sequence is known to work rather than hoped to.

## 3. This phone's board parameters (its own tree, and the live phone)

From `xiaomi-sm8475-common.dtsi` and from `/sys/bus/...`, `/sys/class/...` on the running Android (all
readable without root):

| What | unicorn | How it is known |
|---|---|---|
| DWC3 | `a600000.dwc3` (parent `a600000.ssusb`, 0x100000) | sysfs device list |
| Apps SMMU | `15000000.apps-smmu`, listed as a *supplier* of the DWC3 | `ls /sys/bus/platform/devices/a600000.dwc3/` → `supplier:platform:15000000.apps-smmu` |
| eUSB2 (HS) PHY | `88e3000.hsphy` — **not** the older FEMTO PHY at `0x88e2000` | sysfs device list |
| PHY's suppliers | `i2c:0-004f` (the repeater), `100000.clock-controller` (GCC), `17a00000.rsc:qcom,rpmhclk` (XO), `rpmh-regulator-ldob5`, `rpmh-regulator-ldoc10`, and two unnamed regulators for `S10B`/`L2B` | `ls /sys/bus/platform/devices/88e3000.hsphy/` — this settles the rail list, which no document in this repository had |
| NXP repeater | `nxp,eusb2-repeater`, `reg = <0x4f>`, on `&qupv3_se5_i2c` → **bus `994000.i2c`, i2c-0** | DT node; sysfs `0-004f -> …/994000.i2c/i2c-0/0-004f` |
| Repeater rails | `vdd18 = S10B`, `vdd3 = L2B` | DT |
| Repeater reset | `reset-gpio = <&pm8350c_gpios 7 GPIO_ACTIVE_HIGH>`, pinctrl `eusb2_reset_ctrl_default` | DT |
| Repeater parameters | `qcom,param-override-seq` = `0x40 0x06`, `0x22 0x07`, `0x63 0x08` → **register 6 ← 0x40, 7 ← 0x22, 8 ← 0x63** | DT; the pair order matches how the Linux `repeater-i2c-eusb2` driver reads `<value register>` |
| Other devices on the same I2C bus | `0x0c`, `0x0d`, `0x42` | sysfs — the bus is shared, so the SE5 ownership rules apply |

For reference, the same items on Nothing Phone (2) (`Chengdong-Yu/pong-usb`, `PongUsbPhy.h`): QUP **0x994000**,
eUSB2 PHY **0x088e3000**, repeater **0x4f**, DWC3 with a 32 KiB DMA buffer and **SID 0**, rails
`ldob5 880 mV` + `ldoc10 1200 mV` + XO level from the DFS table, NXP params `{6:0x40, 7:0x22, 8:0x64}`,
QUP5 pads GPIO206/207. **Every address agrees with this phone; the NXP parameter 8 differs by one bit
(0x63 here, 0x64 there) and the pads/rails must come from unicorn's own trees, not from Pong's.** That is
the whole of the transplant risk.

## 4. The sequence to implement (order matters more than the values)

The order below is a working SM8475 board's order, and it is also the order the hardware demands: no DWC3
read before its power domain and clocks are up, no PHY access before the PHY resets are released, no
repeater access before its bus is running.

1. **Rails + XO**: vote `ldob5`, `ldoc10` (and S10B/L2B for the repeater) at the voltages the PHY needs;
   take the XO level from the DFS table rather than guessing a perf index.
2. **USB30 GDSC** on (`GCC + 0x49004`), then the USB bus/ref/UTMI clocks (`0x49018`, `0x49020`, `0x49024`,
   `0x49084`, `0x49088`, `0x9c00c`) — each waits for its halt bit.
3. **QUP SE5** (the repeater's bus): clock branch vote (`0x62008`), the three QUP clock branches, then the
   GENI block at `0x994000`: refuse if a transaction is still active, check the firmware is the I2C
   protocol, own the two pads, set the DFS level and the SCL counters.
4. **Repeater**: read its ID (regs `0x13`–`0x16`) to prove the inherited `PM8350C GPIO7` reset has been
   released (do not force somebody else's GPIO), then write this phone's three parameters and read each
   one back.
5. **eUSB2 PHY** at `0x88e3000`: require `GCC 0x22000` bit 0 and `0x7a000` bit 0 clear (PHY/AHB resets
   released), then the 19.2 MHz program — 23 masked writes, each read back; leave the PHY enabled.
6. **DWC3 device mode**: DCTL run-stop, GCTL, DCFG at high speed, event buffer, TRBs and the 32 KiB DMA
   window from the loader's own memory with the apps-SMMU mapping for its SID, then EP0, `SET_ADDRESS`,
   descriptors (already in `loader.c`), then the two bulk endpoints and the fastboot command loop.

Every step must be *printed* to the framebuffer as it completes (`usb: gdsc ok`, `usb: qup ok`,
`usb: repeater id=%x:%x:%x`, `usb: phy ok`, `usb: dwc3 up`), because until step 6 the only channel is the
screen. A failure must print where it stopped and return, never spin.

## 5. Licence boundary — read this before writing code

`pong-usb`'s PHY file is **GPL-2.0-only** (`SPDX-License-Identifier: GPL-2.0-only`, adapted from
NothingOSS's GPL kernel drivers `phy-msn-snps-eusb2.c` / `repeater-i2c-eusb2.c`). The loader is HakiOS's
MIT/BSD-licensed code. Therefore:

- **Do not paste `PongUsbPhy.h` (or the NothingOSS drivers) into the loader.**
- What may cross the line is *fact*: addresses, register offsets, masks, values, and the required order —
  and each of them has to be re-derived or confirmed from this phone's own sources (DT nodes, sysfs
  supplier lists, the platform's public register documentation) before it goes in. Where a value can only
  come from that GPL code, say so in a comment and keep the file's provenance explicit.
- If we ever want the code shape itself, it has to live in a separate GPL-2.0-only component with its own
  build entry, not inside the MIT loader.

## 6. How each step is verified on the phone

| Step | What the PC sees | What the phone shows |
|---|---|---|
| 1-2 (rails, GDSC, clocks) | nothing | `usb: gdsc ok` … or the failure line |
| 3-5 (QUP, repeater, PHY) | nothing | `usb: repeater id=` with a real ID — an all-0 or all-FF ID means the reset was never released |
| 6a (DWC3 device mode up) | **a new USB device appears in the PnP watch**, even as "unknown"/no driver — that is already success | `usb: dwc3 up` |
| 6b (EP0 + descriptors) | Windows installs *Android Bootloader Interface* for `18d1:d00d` | ~~`usb: started`~~ (the loader's existing line) |
| 6c (bulk + fastboot) | `fastboot devices` → `xnuloader`; then `fastboot oem log`, `oem run`, `get_staged` | the command loop |

`tools/pc/boot-attempt.ps1` already records the 6a moment per 500 ms, which is why it prints every USB
device appearance instead of just polling `fastboot devices`.
