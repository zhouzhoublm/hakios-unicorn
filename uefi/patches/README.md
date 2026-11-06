# UEFI patches of this device

| File | What | Keep? |
|---|---|---|
| `XnuLoader.h.unicorn` | `XNU_PAYLOAD_BASE` is made overridable with `#ifndef`, and `unicorn.dsc` sets it to 0xB0000000 in its BuildOptions (0xF0000000 collides with this family's TZ apps, 0xEA000000-0xF1400000) | yes, belongs in hakios-uefi upstream |
| `mu-silicium-worktree.diff` | the 12S port's uncommitted Mu-Silicium changes: BootShim test blocks (`HAKIOS_FB_TEST`, `HAKIOS_HEARTBEAT`, `HAKIOS_DELAY_S`, `HAKIOS_WDT_PROBE`), a ResetSystem trap, serial counters at 0xA7001000, the BootManager/XnuLoader wiring | **reference only**: carried over unchanged so nothing is lost, but not applied by this port's build, and the debug blocks are the 12S's |
| `mu-basecore-worktree.diff` | the 12S port's AArch64 `CommonCExceptionHandler` change (a synchronous external abort returns all-ones and skips the instruction, which made `PmicDxe`'s SPMI abort survivable) | **reference only**: the blob-free boot does not need it; it is a Mu_Basecore change, not a device's |

This port's build applies **`hakios.patch`** from [oosexgang/hakios-uefi](https://github.com/oosexgang/hakios-uefi)
plus `uefi/files/`, and then this device's `uefi/files/` (the platform) — nothing else. The blob-free
recipe the 12S port settled on is what `unicornPkg` implements: no `Binaries/unicorn` lines in
APRIORI/DXE, `USE_CUSTOM_DISPLAY_DRIVER=0`, no RamManagerDxe in the FDF, `PcdDebugPropertyMask 0x03`,
the DXE heap at 0xC7000000/0x18000000 with the MTE region split around it, carve-outs which are
HobOnly, and the UFS and apps-SMMU register ranges in the memory map.

`tools/provenance-check.py` in the common repository fails a build that commits vendor binaries or a
UEFI image; `uefi/files/Binaries/unicorn/` in this repository holds a README and nothing else.
