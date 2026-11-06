# reference/: what to pull from this phone

The 12S port kept, next to its drivers, the Android-side facts those drivers were written against: its
`dmesg`, `iomem`, partition table, live device tree and `getprop` dump. **They are not here**, because
they are another phone's — what is here is the way to get the same from this one.

```sh
# with the phone on Android, adb debugging on, and root (adb root or a root shell)
adb shell su -c 'cat /sys/firmware/fdt'            > reference/android/unicorn-live.dts   # then: dtc -I dtb -O dts
adb shell su -c 'cat /proc/iomem'                  > reference/android/iomem.txt
adb shell su -c 'dmesg'                            > reference/android/dmesg.txt
adb shell su -c 'cat /proc/cmdline'                > reference/android/cmdline.txt
adb shell su -c 'cat /proc/cpuinfo'                > reference/android/cpuinfo.txt
adb shell su -c 'getprop'                          > reference/android/getprop.txt
adb shell su -c 'ls -l /dev/block/by-name/'        > reference/android/partitions.txt
adb shell su -c 'ls -l /vendor/firmware/'          > reference/android/vendor-firmware.txt
adb shell su -c 'ls -lR /vendor/firmware_mnt/image/qca6490/' > reference/android/firmware_mnt.txt
```

`tools/android/collect.sh` does the same over a root shell, and `tools/android/*.sh` are the small
probes the 12S port used while bringing up the GPU, the amplifiers and the panel.

The four that matter first for this port:

| File | Answers |
|---|---|
| `unicorn-live.dts` | the phone's real device tree — the one to replace `uefi/files/Resources/DTBs/unicorn.dtb` with |
| `iomem.txt` | where the framebuffer really is ("Display Reserved") and how big |
| `dmesg.txt` | the panel's real timings, the UFS and SMMU configuration, the amplifier probes |
| `getprop.txt` | the build the phone runs, so a firmware file can be traced to it |

Nothing in here is committed with personal data in it: scrub MAC addresses, SSIDs and serial numbers
before it goes into git (the common repository's `tools/provenance-check.py` and `logs/README.md` say
how).
