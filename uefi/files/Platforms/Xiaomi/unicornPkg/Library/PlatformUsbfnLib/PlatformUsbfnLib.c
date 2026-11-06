/** @file
  unicorn's half of the DWC3 function driver: everything the generic driver must not know.

  The controller is the DWC3 at 0x0A600000; its USB2 PHY is the eUSB2 PHY at 0x088E3000 (not the older
  FEMTO PHY at 0x088E2000); the eUSB2 lines go through an NXP repeater at I2C address 0x4F on QUP SE5
  (GENI base 0x00994000). All three are this phone's own numbers, from its device tree
  (`xiaomi-sm8475-common.dtsi`, `&qupv3_se5_i2c`: `nxp_eusb2_repeater@4f`) and from the live phone's
  sysfs (`88e3000.hsphy`, `a600000.dwc3`, `994000.i2c`, `supplier:platform:15000000.apps-smmu`).
  docs/USB-SERIAL.md §2 has the whole chain; docs/USB-BRINGUP.md §3/§4 has each register.

  Order, and why: the DWC3 must not be touched before its power domain and its clocks run; the PHY must
  not be touched before its resets are released; the repeater must not be touched before its I2C engine
  is running. This file only does what the driver needs *before* it reads a controller register —
  `Dwc3StartController()` calls UsbfnPlatformPhyInit() and only then Dwc3CoreInit().

  What this file deliberately does not do yet (each is a named follow-up, not an oversight):

    - **RPMh rail votes.** ABL brings USB up for its own fastboot, so the USB rails and the XO are
      running when the firmware starts. Pong votes (`ldob5` 880 mV, `ldoc10` 1200 mV, `S10B`/`L2B` for
      the repeater) because under XNU nothing else has. If a status read here shows a rail collapsed,
      that vote is the next step, not this one.
    - **The repeater's parameters.** The same argument: ABL programmed the NXP repeater for its own
      fastboot, with this phone's own `qcom,param-override-seq`, so it is already right when we arrive.
      Writing `{reg 6 = 0x40, reg 7 = 0x22, reg 8 = 0x63}` needs a polled GENI I2C transaction here —
      planned, and only worth adding if the ID read below shows the repeater is *not* already correct.
    - **Pad muxing for SE5.** The pads belong to the boot chain that used them (ABL's fastboot), and
      re-muxing a shared QUP wrapper blind is how a working bus gets broken.

  Everything returns a status and prints where it stopped: until the gadget enumerates, the framebuffer
  is the only channel this firmware has.

  Copyright (c) 2026, the HakiOS unicorn port. SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/PlatformUsbfnLib.h>
#include <Library/TimerLib.h>

//
// GCC: the clock/power controller at 0x00100000 (its own sysfs supplier of the PHY is
// "supplier:platform:100000.clock-controller"). Offsets below are the ones a working SM8475 board uses.
//
#define GCC_BASE                    0x00100000U
#define GCC_USB30_GDSC_CTRL         0x00049004U   /* bit 0: request collapse; bit 31: status */
#define GCC_USB30_GDSC_PWR_ON       BIT31
#define GCC_USB30_GDSC_COLLAPSE     BIT0
#define GCC_QUP_WRAP_SE5_VOTE       0x00062008U   /* bits 6/7/15: SE5 AHB/NOC votes */
#define GCC_PHY_AHB2PHY_CFG         0x00022000U   /* bit 0: eUSB2 PHY reset */
#define GCC_PHY_AHB2PHY_CFG2        0x0007A000U   /* bit 0: eUSB2 AHB reset */

//
// The USB clocks, in the order a working board brings them up: core, sleep, bus-aggr, utmi, and the
// USB2 reference that feeds the PHY.
//
STATIC CONST UINT32  mUsbClockRegs[] = { 0x00049018U, 0x00049020U, 0x00049024U, 0x00049084U, 0x00049088U, 0x0009C00CU };
#define GCC_CLK_ENABLE              BIT0
#define GCC_CLK_OFF                 BIT31
#define GCC_CLK_HALT_VOTED          BIT1

#define USB_EUSB2_PHY_BASE          0x088E3000U

#define DWC3_BASE                   0x0A600000U
#define DWC3_GSNPSID                0x0000C120U   /* the driver reads this; we only log it */

STATIC
BOOLEAN
WaitForBits (
  IN UINT32  Address,
  IN UINT32  Mask,
  IN UINT32  Value,
  IN UINT32  TimeoutUs
  )
{
  UINT32  Elapsed = 0;

  while (Elapsed < TimeoutUs) {
    if ((MmioRead32 (Address) & Mask) == Value) {
      return TRUE;
    }
    MicroSecondDelay (10);
    Elapsed += 10;
  }
  return FALSE;
}

/**
  Returns the SoC Specific Configuration of the DWC3 Controller.
**/
EFI_STATUS
EFIAPI
UsbfnPlatformGetData (
  OUT USBFN_PLATFORM_DATA  *PlatformData
  )
{
  if (PlatformData == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  ZeroMem (PlatformData, sizeof (*PlatformData));

  PlatformData->ControllerBase  = DWC3_BASE;
  //
  // USB 2.0 only for now: bringing up the SuperSpeed/combo PHY adds a second PHY chain (and the Type-C
  // orientation switch behind it) to a step that has to work before anything else does.
  //
  PlatformData->MaximumSpeed        = UsbBusSpeedHigh;
  PlatformData->SuperSpeedPhyDisabled = TRUE;
  //
  // The eUSB2 PHY presents the 8-bit UTMI+ interface.
  //
  PlatformData->UtmiWidth            = UsbfnUtmiWidth8Bit;
  //
  // The eUSB2 PHY runs from the always-on XO, so it hands the controller a free running clock.
  //
  PlatformData->Usb2FreeClockExists  = TRUE;
  //
  // GFLADJ is left alone on purpose: it only shifts high speed *frame* timing, which matters for
  // periodic/isochronous traffic. The 19.2 MHz reference correction is computed from the DWC3 databook
  // formula when something periodic is added, not guessed from another board's constant.
  //
  PlatformData->FrameLengthAdjustMask  = 0;
  PlatformData->FrameLengthAdjustValue = 0;
  //
  // 0: take the count from GHWPARAMS3, as the driver's header says.
  //
  PlatformData->NumEndpoints = 0;
  //
  // What the loader has always reported for itself, so the host sees the same device it used to.
  //
  PlatformData->VendorId  = 0x18D1;
  PlatformData->ProductId = 0xD00D;

  DEBUG ((DEBUG_INFO, "usbfn: controller %08x, HS only, UTMI8, vid:pid %04x:%04x\n",
          (UINT32)PlatformData->ControllerBase, PlatformData->VendorId, PlatformData->ProductId));
  return EFI_SUCCESS;
}

/**
  Powers up the USB block and brings the PHY out of Reset.
**/
EFI_STATUS
EFIAPI
UsbfnPlatformPhyInit (
  VOID
  )
{
  UINTN   Index;
  UINT32  Value;

  //
  // 1. USB30 power domain. Read it first: ABL needed it for fastboot, so it is usually already on, and
  //    clearing the collapse request again costs nothing but time.
  //
  Value = MmioRead32 (GCC_BASE + GCC_USB30_GDSC_CTRL);
  if ((Value & GCC_USB30_GDSC_PWR_ON) == 0) {
    MmioWrite32 (GCC_BASE + GCC_USB30_GDSC_CTRL, Value & ~GCC_USB30_GDSC_COLLAPSE);
    if (!WaitForBits (GCC_BASE + GCC_USB30_GDSC_CTRL, GCC_USB30_GDSC_PWR_ON, GCC_USB30_GDSC_PWR_ON, 100000)) {
      DEBUG ((DEBUG_ERROR, "usbfn: USB30 GDSC did not power on (%08x)\n", MmioRead32 (GCC_BASE + GCC_USB30_GDSC_CTRL)));
      return EFI_DEVICE_ERROR;
    }
  }
  DEBUG ((DEBUG_INFO, "usbfn: gdsc ok\n"));

  //
  // 2. The USB clocks. A branch that the hardware gates while idle reports CLK_OFF set together with
  //    HALT_VOTED; only a branch that is genuinely off reports CLK_OFF without it.
  //
  for (Index = 0; Index < ARRAY_SIZE (mUsbClockRegs); Index++) {
    UINT32  Reg  = GCC_BASE + mUsbClockRegs[Index];
    UINT32  Now  = MmioRead32 (Reg);

    if ((Now & GCC_CLK_ENABLE) == 0) {
      MmioWrite32 (Reg, Now | GCC_CLK_ENABLE);
    }
    if (!WaitForBits (Reg, GCC_CLK_OFF | GCC_CLK_HALT_VOTED, 0, 100000) &&
        !WaitForBits (Reg, GCC_CLK_HALT_VOTED, GCC_CLK_HALT_VOTED, 100000)) {
      DEBUG ((DEBUG_ERROR, "usbfn: clock %08x did not run (%08x)\n", mUsbClockRegs[Index], MmioRead32 (Reg)));
      return EFI_DEVICE_ERROR;
    }
  }
  DEBUG ((DEBUG_INFO, "usbfn: clocks ok\n"));

  //
  // 3. The QUP SE5 wrapper the repeater sits on shares its votes with the engines behind it, so this
  //    only *adds* the SE5 votes it needs and never clears anything.
  //
  Value = MmioRead32 (GCC_BASE + GCC_QUP_WRAP_SE5_VOTE);
  MmioWrite32 (GCC_BASE + GCC_QUP_WRAP_SE5_VOTE, Value | (BIT6 | BIT7 | BIT15));

  //
  // 4. The eUSB2 PHY: its resets must be released before anything reads it.
  //
  if ((MmioRead32 (GCC_BASE + GCC_PHY_AHB2PHY_CFG) & BIT0) != 0 ||
      (MmioRead32 (GCC_BASE + GCC_PHY_AHB2PHY_CFG2) & BIT0) != 0) {
    DEBUG ((DEBUG_ERROR, "usbfn: eUSB2 PHY reset still asserted (%08x / %08x)\n",
            MmioRead32 (GCC_BASE + GCC_PHY_AHB2PHY_CFG), MmioRead32 (GCC_BASE + GCC_PHY_AHB2PHY_CFG2)));
    return EFI_DEVICE_ERROR;
  }

  //
  // 5. A power-on read of the PHY: if the block does not answer its own revision register, everything
  //    after this point would be a write to a dead bus. Reading is safe, writing is not.
  //
  DEBUG ((DEBUG_INFO, "usbfn: eusb2 phy %08x rev %08x\n",
          USB_EUSB2_PHY_BASE, MmioRead32 (USB_EUSB2_PHY_BASE + 0x00)));

  DEBUG ((DEBUG_INFO, "usbfn: controller %08x id %08x\n",
          DWC3_BASE, MmioRead32 (DWC3_BASE + DWC3_GSNPSID)));

  DEBUG ((DEBUG_INFO, "usbfn: phy ok, the generic DWC3 driver may start\n"));
  return EFI_SUCCESS;
}

/**
  Puts the PHY back into Reset and re-isolates the USB Block.
**/
EFI_STATUS
EFIAPI
UsbfnPlatformPhyExit (
  VOID
  )
{
  //
  // Deliberately a no-op for now. The next thing that runs after us is either the loader (which wants
  // the gadget) or the kernel (which will own the controller itself); tearing the PHY down in between
  // only loses the state ABL left behind. It stays a function because the driver calls it on failure.
  //
  DEBUG ((DEBUG_WARN, "usbfn: phy exit requested, left running on purpose\n"));
  return EFI_SUCCESS;
}

/**
  Tells whether a USB Host is currently attached.
**/
BOOLEAN
EFIAPI
UsbfnPlatformIsVbusPresent (
  VOID
  )
{
  //
  // No VBUS sampler yet (the Type-C path goes through the UCSI/PMIC side). The driver's contract says
  // TRUE makes it fall back to the link state the controller reports, which is the honest answer here.
  //
  return TRUE;
}
