# Firmware (not in this repository)

All of it is Qualcomm's or Xiaomi's. Take it from **your own** Xiaomi 12S Pro (unicorn) or the official
fastboot ROM for it. It goes into directories at the top of the HakiOS common repository, which
`.gitignore` keeps out of git; `userland/build.sh` copies it into the image.

| Directory | Files | Source |
|---|---|---|
| `fw-gpu/` | `a730_sqe.fw`, `gmu_gen70000.bin`, `a730_zap.mdt`, `a730_zap.b00`..`b02` | `/vendor/firmware/` on the phone |
| `fw-wifi/` | `amss20.bin`, `m3.bin`, **unicorn's board file**, `regdb_xiaomi.bin` | `image/qca6490/` inside the ROM's `NON-HLOS.bin` (`/vendor/firmware_mnt/image/qca6490/` on the phone) |
| `fw-audio/` | `adsp.mdt`, `adsp.b01`..`bNN`, the **CS35L41** tuning files | `adsp.*` from `NON-HLOS.bin`; the amplifier's files from `/vendor/firmware/` |
| `fw-bt/image/` | `hpbtfw21.tlv`, `hpnv21.*` | the ROM's `BTFM.bin` |

**Two names are not the 12S's and must be read out of this phone's own firmware**, because the 12S port
uses its own board data:

- the Wi-Fi board file — the Xiaomi 12S uses `bd_l3s.elf` ("l3s" is its panel family); unicorn's panel
  family is "l2s", so look for `bd_l2s*.elf` (or whatever the `qca6490` directory of *this* phone's
  `NON-HLOS.bin` carries) and pass/place that name;
- the audio calibration files — the 12S's `aw882xx_acf.bin` belongs to its Awinic AW88261 amplifiers;
  unicorn's are Cirrus CS35L41, whose firmware/tuning comes from `/vendor/firmware/` on this phone.

```sh
# from the fastboot ROM (7zz from Homebrew's sevenzip; p7zip is too old for these images)
7zz x -ofw-nonhlos images/NON-HLOS.bin
7zz x -ofw-btfm    images/BTFM.bin
mkdir -p fw-wifi fw-audio fw-bt
ls fw-nonhlos/image/qca6490/            # <- the board file's real name is in this listing
cp fw-nonhlos/image/qca6490/* fw-wifi/
cp fw-nonhlos/image/adsp.* fw-audio/
cp -r fw-btfm/image fw-bt/

# from Android with root (adb shell su)
adb shell su -c 'tar -cf - /vendor/firmware/a730_* /vendor/firmware/gmu_gen70000.bin' | tar -xf - -C /tmp/vfw
adb shell su -c 'ls /vendor/firmware/' | grep -i 'cs35\|cirrus'
```

The Wi-Fi **MAC address** is in the `persist` partition: `qca6490/wlan_mac.bin` (pass it as `wifi_mac=`).

The **UEFI vendor drivers** (`uefi/files/Binaries/unicorn/`) are not needed for the working, blob-free
boot. If you want them for experiments: WOA-Project UEFIReader on the ROM's `uefi.elf` (see the README in
that directory).

**Apple's iOS 16.5 (20F66) IPSW** is needed for the Darwin root and the cryptexes; HakiOS's `sysimage/`
tools unpack it. Never redistribute any of these files.
