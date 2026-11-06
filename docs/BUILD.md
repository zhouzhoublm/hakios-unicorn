# Building this device's UEFI and boot image

Two things are built here: the **UEFI firmware** (`SILICIUM_UEFI.fd`, from Mu-Silicium with HakiOS's
changes and this phone's platform) and the **boot image** that carries it (`xnu-*.img`: BootShim + FD +
an "XNUP" container with the loader and optionally the kernel + this phone's device tree).

The kernel and the userland are the common repository's (`make kernel`, `userland/build.sh`); they need
a Mac with Xcode. **Neither the UEFI nor the image does** — the recipe below was run on Linux (Ubuntu
24.04, x86-64, in WSL) and is what produced the first `xnu-unicorn-selftest.img`.

## What you need

```
git python3 (3.12) pip  gcc g++ make nasm iasl uuid-dev  mono-devel (or the nasm workaround below)
gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu          (or clang + ld.lld + llvm-objcopy, for BootShim)
python3 -m pip install edk2-pytool-extensions edk2-pytool-library regex
# for the device tree: cpp, dtc, fdtoverlay (device-tree-compiler)
```

## The tree

```sh
# a checkout of the common repository, this device at devices/unicorn/, and HakiOS's UEFI at uefi/
git clone https://github.com/oosexgang/hakios-xnu-kernel         # or your fork with the CI below
cd hakios-xnu-kernel
git clone https://github.com/oosexgang/hakios-uefi uefi
git clone https://github.com/zhouzhoublm/hakios-unicorn devices/unicorn   # this repository
uefi/apply.sh unicorn        # Mu-Silicium at uefi/BASE + hakios.patch + uefi/files/ + this platform
```

`uefi/apply.sh` clones Mu-Silicium (with submodules, ~3 GB) and copies `uefi/files/` and
`devices/unicorn/uefi/files/` over it. Then this device's `XnuLoader.h` (the payload base override) has
to replace Silicium's:

```sh
cp devices/unicorn/uefi/patches/XnuLoader.h.unicorn "$(find Mu-Silicium -name XnuLoader.h | head -1)"
```

and the FDF names vendor binaries that are not in the repository (that is what makes the build
blob-free); stub them so the FDF parses:

```sh
cd Mu-Silicium
grep -rho 'Binaries/[A-Za-z0-9_./-]*' --include='*.fdf' --include='*.inc' --include='*.dsc' . | sort -u |
  while read -r q; do [ -e "$q" ] || { mkdir -p "$(dirname "$q")"; : > "$q"; }; done
```

Without Mono, `mu_nasm` (a NuGet dependency) is replaced by the system nasm:

```sh
D=Mu_Basecore/BaseTools/Bin/mu_nasm_extdep; mkdir -p $D/Linux-x86-64
echo 'version: 20016.1.1' > $D/extdep_state.yaml
ln -sf /usr/bin/nasm $D/Linux-x86-64/nasm; ln -sf /usr/bin/ndisasm $D/Linux-x86-64/ndisasm
```

## The UEFI

`FD_BASE`, `FD_SIZE` and `FD_BLOCKS` come from **this device's** `Resources/Configs/unicorn.toml`
(0xA7100000 / 0x300000 / 0x300 — docs/FINDINGS.md says why those):

```sh
cd Mu-Silicium
stuart_update -c Platforms/Xiaomi/unicornPkg/DeviceBuild.py
stuart_build  -c Platforms/Xiaomi/unicornPkg/DeviceBuild.py \
              FD_BASE=0xA7100000 FD_SIZE=0x300000 FD_BLOCKS=0x300 'BLD_*_TARGET=DEBUG'
# -> Build/unicornPkg/DEBUG_CLANGPDB/FV/SILICIUM_UEFI.fd
```

`build_uefi.py -d unicorn -r DEBUG` does the same when the device is in Mu-Silicium's own build script's
list; the two `stuart_*` calls above are what the CI runs and are known to work here.

## The boot image

```sh
devices/unicorn/tools/pack-image.py --selftest -o xnu-unicorn-selftest.img
```

`pack-image.py` reads the FD base/size from the toml and the payload base from the DSC
(`-DXNU_PAYLOAD_BASE=0xB0000000ULL`), builds the BootShim for those two addresses, packs the loader
(`--selftest`, no kernel), and writes the image as

```
kernel = gzip(BootShim + SILICIUM_UEFI.fd + XNUP(loader.efi)) + unicorn.dtb
        Android boot image header v4, empty ramdisk            (append_dtb = true in the toml)
```

For the real boot, give it the kernel and the Darwin root:

```sh
devices/unicorn/tools/pack-image.py \
    --kernel hakios-kernel --ramdisk userland/root.apfs --resident userland/flashboot.apfs \
    -o xnu-unicorn.img --cmdline "root_apfs=ufs_darwin ufs=2 bootcpu=7 cpus=7"
# or: devices/unicorn/tools/build-image.sh NAME "extra boot-args"
```

## The CI

`.github/workflows/build-uefi.yml` of the fork this port was built in
([zhouzhoublm/hakios-xnu-kernel](https://github.com/zhouzhoublm/hakios-xnu-kernel)) does all of the
above on a GitHub runner (`workflow_dispatch`, inputs `device`, `device_repo`, `payload_mode`,
`target`) and uploads `SILICIUM_UEFI.fd` and `xnu-<device>.img` as the artifact `uefi-fd-<device>`. It
mirrors these steps: `uefi/hakios.patch`, `uefi/files/`, the device's platform, the `XnuLoader.h`
override, the stubs, `stuart_update`, `stuart_build`, BootShim + payload + gzip + `mkbootimg.py`
header v4. Use it when the machine you have cannot reach GitHub's submodules reliably.

## What was built and checked (2026-11-06)

The CI above was run for this device (`workflow_dispatch`, `device=unicorn`,
`device_repo=https://github.com/zhouzhoublm/hakios-unicorn`, `payload_mode=selftest`, `target=DEBUG`)
and finished green. Its artifact `uefi-fd-unicorn` was unpacked and checked:

| Thing | Value |
|---|---|
| `SILICIUM_UEFI.fd` | 3 145 728 bytes (0x300000, exactly `FD_SIZE`), base 0xA7100000, `FVMAIN_COMPACT` 19 % used, built in 62 s |
| modules in the FD | DxeCore, ArmCpuDxe, ArmGicDxe, ArmTimerDxe, **SimpleFbDxe**, GraphicsConsoleDxe, BdsDxe, Smbios/ACPI/IortDtDxe, the three BMPs... and **no vendor DXE driver** (blob-free), with `loader.efi` (the XNU loader hook) present in the decompressed tree |
| BootShim | 192 bytes, built with `-DFD_BASE=0xA7100000 -DFD_SIZE=0x300000 -DPAYLOAD_BASE=0xB0000000` |
| payload (selftest) | 66 328 bytes, `XNUP` with `loader.efi` and `--checkpoint 999` |
| device tree | 346 590 bytes, identical to `devices/unicorn/uefi/files/Resources/DTBs/unicorn.dtb` in this repository |
| `xnu-unicorn.img` | 970 752 bytes, `ANDROID!` header **v4**, header_size 1584 (padded to 4096 by the v4 signature), kernel field 959 145 = 612 555 bytes of gzip(shim+FD+payload) + the DTB |

```
fd    sha256 45054ab4bb7a270f3ffdcee23e316a0020052cdc06e8d330c9866389743797e1
img   sha256 848d2089e796740954f895d0ce1eabe56b6a8bc07c2a012e622658d871832520
dtb   sha256 9a2e12e7bd92c98724a09c25584bdf41368efa5537e0457eadd1b4b6457e98f9
```

What it does **not** prove: that the phone starts it. That is the next thing (docs/OPEN-ISSUES.md, 1).

## The device tree

`uefi/files/Resources/DTBs/unicorn.dtb` is built from source; the exact commands (`cpp` + `dtc` + two
`fdtoverlay`s... one, see below) are in docs/FINDINGS.md. The short form:

```sh
cpp -nostdinc -undef -D__DTS__ -x assembler-with-cpp -I qcom -I . -I qcom/include \
    -I <kernel>/include -I <kernel>/include/uapi qcom/cape.dts -o cape.dts
dtc -@ -I dts -O dtb -o cape.dtb cape.dts
cpp ... qcom/unicorn-sm8475-pm8008-overlay.dts -o board.dts
dtc -@ -I dts -O dtb -o board.dtbo board.dts
fdtoverlay -i cape.dtb -o unicorn.dtb board.dtbo
```

The phone's own tree (`adb pull /sys/firmware/fdt`) supersedes it, and should (reference/README.md).
