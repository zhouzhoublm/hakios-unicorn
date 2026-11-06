# Binaries/unicorn: the Xiaomi 12S Pro's own UEFI drivers (not in this repository)

Everything in this directory except this file would be Qualcomm's and Xiaomi's: the display, storage, USB,
secure-boot and other DXE drivers of your phone, extracted from *your* phone's UEFI image. They are
proprietary and are never committed (the common repository's `.gitignore` and `tools/provenance-check.py`
keep them out).

**This port does not need them.** The boot it is built for is blob-free — no vendor driver at all, the
UEFI only brings up the CPU, the GIC, the timers and a simple framebuffer on what ABL left, and the
loader and the kernel drive the hardware themselves (docs/FINDINGS.md, "What the UEFI of this port is").

If you want them anyway, for experiments:

1. Get the phone's UEFI image: `uefi.elf` from the fastboot ROM for the Xiaomi 12S Pro (unicorn), or the
   phone's `xbl`/`uefi` partition with root. Write the build string down here when you do it — another
   firmware version may carry other drivers.
2. Extract with [WOA-Project/UEFIReader](https://github.com/WOA-Project/UEFIReader):
   `UEFIReader images/uefi.elf out/`, then copy `out/<build string>/.` here.
3. `Include/DXE.inc` and `Include/APRIORI.inc` may only name files that are actually here, so add the
   lines for what you extracted (the port's own lists name none, which is what makes it blob-free).

Nothing extracted this way is redistributable, and none of it is needed by the working boot.
