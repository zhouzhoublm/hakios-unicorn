# UEFI working-tree diffs (reference, not yet clean)

The boot that works today was built from Mu-Silicium with `../files` (mayflyPkg) **plus** these uncommitted
changes in the Silicium checkout. They are published as they are so nothing is lost; some of it is debug
scaffolding that should not go upstream.

| File | What | Keep? |
|---|---|---|
| `mu-silicium-worktree.diff` | BootShim test blocks (`HAKIOS_FB_TEST`, `HAKIOS_HEARTBEAT`, `HAKIOS_DELAY_S`, `HAKIOS_WDT_PROBE`), ResetSystem trap, serial counters at 0xA7001000, BootManager/XnuLoader wiring | test blocks: debug only |
| `mu-basecore-worktree.diff` | AArch64 `CommonCExceptionHandler`: a synchronous external abort returns all-ones and skips the instruction (made PmicDxe's SPMI abort at FAR 0x0C717008 survivable) | debug only; the blob-free boot does not need it |
| `XnuLoader.h.mayfly` | `XNU_PAYLOAD_BASE` is overridable; mayfly sets 0xB0000000 in its DSC BuildOptions (0xF0000000 collides with waipio TZ apps 0xEA000000-0xF1400000) | yes, belongs in hakios-uefi |

The blob-free recipe (see `../../docs/FINDINGS.md`, "UEFI"): no `Binaries/mayfly` lines in APRIORI/DXE,
`USE_CUSTOM_DISPLAY_DRIVER=0`, RamManagerDxe removed from the FDF, `PcdDebugPropertyMask 0x03`, DXE heap
0xC7000000/0x18000000 with the MTE region split around it, carve-outs HobOnly, "RAM High" 0x840000000/0x240000000,
UFS_MEM 0x1D80000 and the apps SMMU in the memory map.
