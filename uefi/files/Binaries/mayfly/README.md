# Binaries/mayfly: the Xiaomi 12S's own UEFI drivers (not in this repository)

Everything in this directory except this file is Qualcomm's and Xiaomi's, extracted from the phone's `uefi` partition
image and never committed (the common repository's `.gitignore` and `tools/provenance-check.py` keep it out).

Made from: `uefi.elf` of the fastboot ROM `mayfly_images_OS2.0.207.0.VLTCNXM_15.0` (HyperOS 2, Android 15, China),
UEFI build `BOOT.MXF.2.0-01018.3-WAIPIO-1`, with WOA-Project's UEFIReader:

    UEFIReader images/uefi.elf out/        # then copy out/BOOT.MXF.2.0-01018.3-WAIPIO-1/. here

UEFIReader as of 2026-10-31 stops with "File Conflict Detected" on this image; skipping a module that appears twice
with identical contents (and keeping a differing second copy under another name) lets it finish. No differing copy was
reported for this image.

Another firmware version may carry other drivers: `Include/DXE.inc` and `Include/APRIORI.inc` must only name files
that are here.
