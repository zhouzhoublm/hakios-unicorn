# Firmware (not in this repository)

All of it is Qualcomm's or Xiaomi's. Take it from **your own** phone or the official fastboot ROM for it
(tested with `mayfly_images_OS2.0.207.0.VLTCNXM_15.0`). It goes into directories at the top of
hakios-xnu-kernel, which `.gitignore` keeps out of git; `userland/build.sh` copies it into the image.

| Directory | Files | Source |
|---|---|---|
| `fw-gpu/` | `a730_sqe.fw`, `gmu_gen70000.bin`, `a730_zap.mdt`, `a730_zap.b00`..`b02` | Android `/vendor/firmware/` |
| `fw-wifi/` | `amss20.bin`, `m3.bin`, `bd_l3s.elf`, `regdb_xiaomi.bin` | `image/qca6490/` inside the ROM's `NON-HLOS.bin` (Android: `/vendor/firmware_mnt/image/qca6490/`) |
| `fw-audio/` | `adsp.mdt`, `adsp.b01`..`b25`, `aw882xx_acf.bin` | `adsp.*` from `NON-HLOS.bin`; `aw882xx_acf.bin` from `/vendor/firmware/` |
| `fw-bt/image/` | `hpbtfw21.tlv`, `hpnv21.*` (BTFW.HSP.2.0.1-00252) | the ROM's `BTFM.bin` |

```sh
# from the fastboot ROM (7zz from Homebrew's sevenzip; p7zip is too old for these images)
7zz x -ofw-nonhlos images/NON-HLOS.bin
7zz x -ofw-btfm    images/BTFM.bin
mkdir -p fw-wifi fw-audio fw-bt
cp fw-nonhlos/image/qca6490/{amss20.bin,m3.bin,bd_l3s.elf,regdb_xiaomi.bin} fw-wifi/   # names as on your image
cp fw-nonhlos/image/adsp.* fw-audio/
cp -r fw-btfm/image fw-bt/

# from Android with root (adb shell su)
adb shell su -c 'tar -cf - /vendor/firmware/a730_* /vendor/firmware/gmu_gen70000.bin /vendor/firmware/aw882xx_acf.bin' | tar -xf - -C /tmp/vfw
```

The Wi-Fi **MAC address** is in your `persist` partition: `qca6490/wlan_mac.bin` (pass it as `wifi_mac=`).

The **UEFI vendor drivers** (`uefi/files/Binaries/mayfly/`) are not needed for the working, blob-free boot.
If you want them for experiments: WOA-Project UEFIReader on the ROM's `uefi.elf` (see the README in that directory).

**Apple's iOS 16.5 (20F66) IPSW** is needed for the Darwin root and the cryptexes; HakiOS's `sysimage/` tools
unpack it. Never redistribute any of these files.
