#!/usr/bin/env python3
"""
Pack this device's UEFI into an Android boot image that `fastboot boot` starts.

  kernel = gzip(BootShim + UEFI FD + "XNUP" container) + device tree

The boot shim copies the FD to FD_BASE and the XNUP container to PAYLOAD_BASE, where UEFI's boot
manager finds `loader.efi` and starts it (hakios-uefi's XnuLoader.c). Both numbers come from this
device's own files, not from this script:

  FD_BASE / FD_SIZE   uefi/files/Resources/Configs/<device>.toml   ([uefi_fd] base, size)
  PAYLOAD_BASE        <device>Pkg/<device>.dsc                     (-DXNU_PAYLOAD_BASE in BuildOptions)
  the device tree     uefi/files/Resources/DTBs/<device>.dtb

Two payloads:
  --selftest   loader only (`--checkpoint 999`): the loader prints [cpu] [display] [memmap] [payload]
               and resets. This is the first run on a new phone: it needs no kernel.
  (default)    loader + kernel + ramdisk [+ resident], the real boot.

usage:
  devices/unicorn/tools/pack-image.py --selftest -o xnu-unicorn-selftest.img
  devices/unicorn/tools/pack-image.py -o xnu-unicorn.img \
      --kernel hakios-kernel --ramdisk userland/root.apfs --resident userland/flashboot.apfs \
      --cmdline "root_apfs=ufs_darwin ufs=2 bootcpu=7 cpus=7"

Needs: the UEFI built (Mu-Silicium/Build/<device>Pkg/<TARGET>_CLANGPDB/FV/SILICIUM_UEFI.fd), an
aarch64 cross compiler or clang for BootShim, and prebuilt/loader.efi from the common repository.
"""
import argparse, gzip, hashlib, os, re, shutil, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEV = os.path.dirname(HERE)                      # devices/unicorn
ROOT = os.path.dirname(os.path.dirname(DEV))     # the HakiOS common repository
DEVICE = os.path.basename(DEV)

BASE_ARGS = ("loader_return_after=300 bootcpu=7 cpus=7")


def read_fd_params():
    toml = os.path.join(DEV, "uefi", "files", "Resources", "Configs", f"{DEVICE}.toml")
    text = open(toml).read()
    block = text.split("[uefi_fd]", 1)[1].split("[", 1)[0]
    base = int(re.search(r"base\s*=\s*(0x[0-9A-Fa-f]+|\d+)", block).group(1), 0)
    size = int(re.search(r"size\s*=\s*(0x[0-9A-Fa-f]+|\d+)", block).group(1), 0)
    return base, size


def read_payload_base():
    dsc = os.path.join(DEV, "uefi", "files", "Platforms", "Xiaomi", f"{DEVICE}Pkg", f"{DEVICE}.dsc")
    m = re.search(r"XNU_PAYLOAD_BASE\s*=\s*(0x[0-9A-Fa-f]+)", open(dsc).read())
    if not m:
        sys.exit(f"pack-image.py: no XNU_PAYLOAD_BASE in {dsc}")
    return int(m.group(1), 16)


def find_fd(target):
    for t in ([target] if target else ["DEBUG", "RELEASE"]):
        p = os.path.join(ROOT, "Mu-Silicium", "Build", f"{DEVICE}Pkg", f"{t}_CLANGPDB", "FV", "SILICIUM_UEFI.fd")
        if os.path.isfile(p):
            return p
    sys.exit(f"pack-image.py: no SILICIUM_UEFI.fd for {DEVICE}; build the UEFI first (docs/BUILD.md)")


def build_shim(fd_base, fd_size, payload_base):
    shim_dir = os.path.join(ROOT, "Mu-Silicium", "BootShim")
    if not os.path.isdir(shim_dir):
        sys.exit("pack-image.py: no Mu-Silicium/BootShim; uefi/apply.sh makes the tree first")
    for f in ("BootShim.elf", "BootShim.bin"):
        try:
            os.unlink(os.path.join(shim_dir, f))
        except FileNotFoundError:
            pass
    env = dict(os.environ)
    env.setdefault("PATH", "/usr/bin:/bin")
    make = ["make", "REQUIRES_KERNEL_HEADER=1", f"FD_BASE={fd_base:#x}", f"FD_SIZE={fd_size:#x}"]
    tries = [
        make + [f"CFLAGS=--target=aarch64-linux-gnu -DPAYLOAD_BASE={payload_base:#x}"],
        make + ["CC=aarch64-linux-gnu-gcc", "OBJCOPY=aarch64-linux-gnu-objcopy",
                f"CFLAGS=-DPAYLOAD_BASE={payload_base:#x}"],
    ]
    for cmd in tries:
        subprocess.run(cmd, cwd=shim_dir, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        p = os.path.join(shim_dir, "BootShim.bin")
        if os.path.isfile(p):
            return p
    sys.exit("pack-image.py: BootShim did not build (need clang --target=aarch64-linux-gnu or aarch64-linux-gnu-gcc)")


def make_payload(args):
    if args.payload:
        return args.payload
    mkpayload = os.path.join(ROOT, "xnuloader", "mkpayload.py")
    loader = os.path.join(ROOT, "prebuilt", "loader.efi")
    if not os.path.isfile(loader):
        sys.exit(f"pack-image.py: {loader} missing (the common repository's prebuilt loader)")
    out = os.path.join(ROOT, "out")
    os.makedirs(out, exist_ok=True)
    payload = os.path.join(out, f"payload-{DEVICE}{'-selftest' if args.selftest else ''}.bin")
    cmd = [sys.executable, mkpayload, "--usb", "-o", payload, f"loader.efi={loader}"]
    if args.selftest:
        cmd += ["--checkpoint", "999"]
    else:
        for name, path in (("kernel", args.kernel), ("ramdisk", args.ramdisk), ("resident", args.resident)):
            if path:
                if not os.path.isfile(path):
                    sys.exit(f"pack-image.py: {path} not found")
                cmd.append(f"{name}={os.path.abspath(path)}")
    cmd += ["--cmdline", (args.cmdline or BASE_ARGS).strip()]
    print("  " + " ".join(os.path.basename(c) if i == 0 else c for i, c in enumerate(cmd[2:])))
    subprocess.check_call(cmd, stdout=subprocess.DEVNULL)
    return payload


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-o", "--output", default=os.path.join(ROOT, f"xnu-{DEVICE}.img"))
    ap.add_argument("--selftest", action="store_true", help="loader only, no kernel (the first run)")
    ap.add_argument("--kernel", help="the kernel built for this device (hakios-kernel)")
    ap.add_argument("--ramdisk", help="the Darwin root image (userland/root.apfs)")
    ap.add_argument("--resident", help="a resident image the loader places itself (userland/flashboot.apfs)")
    ap.add_argument("--payload", help="a ready XNUP container (skips loader/kernel assembly)")
    ap.add_argument("--cmdline", help="boot arguments")
    ap.add_argument("--target", choices=["DEBUG", "RELEASE"])
    args = ap.parse_args()

    fd_base, fd_size = read_fd_params()
    payload_base = read_payload_base()
    fd_path = find_fd(args.target)
    fd = open(fd_path, "rb").read()
    if len(fd) != fd_size:
        sys.exit(f"pack-image.py: {fd_path} is {len(fd):#x} bytes, the toml says {fd_size:#x}")
    print(f"  FD     {os.path.relpath(fd_path, ROOT)} ({len(fd):#x} bytes) -> {fd_base:#x}")

    shim = open(build_shim(fd_base, fd_size, payload_base), "rb").read()
    print(f"  shim   {len(shim)} bytes, FD_BASE={fd_base:#x} PAYLOAD_BASE={payload_base:#x}")

    payload = make_payload(args)
    data = open(payload, "rb").read()
    if data[:4] != b"XNUP":
        sys.exit(f"pack-image.py: {payload} is not an XNUP container")
    print(f"  XNUP   {os.path.relpath(payload, ROOT)} ({len(data)} bytes)")

    dtb_path = os.path.join(DEV, "uefi", "files", "Resources", "DTBs", f"{DEVICE}.dtb")
    dtb = open(dtb_path, "rb").read()
    print(f"  dtb    {os.path.relpath(dtb_path, ROOT)} ({len(dtb)} bytes)")

    blob = shim + fd + data
    blob += b"\0" * (-len(blob) % 16)
    kernel = gzip.compress(blob, 6) + dtb

    tmp_kernel = os.path.join(ROOT, "out", "kernel~")
    tmp_ramdisk = os.path.join(ROOT, "out", "ramdisk~")
    os.makedirs(os.path.join(ROOT, "out"), exist_ok=True)
    open(tmp_kernel, "wb").write(kernel)
    open(tmp_ramdisk, "wb").write(b"")

    mkbootimg = os.path.join(ROOT, "Mu-Silicium", "Resources", "Scripts", "mkbootimg.py")
    header_version = re.search(r"header_version\s*=\s*(\d+)", open(
        os.path.join(DEV, "uefi", "files", "Resources", "Configs", f"{DEVICE}.toml")).read()).group(1)
    try:
        subprocess.check_call([sys.executable, mkbootimg, "--header_version", header_version,
                               "--kernel", tmp_kernel, "--ramdisk", tmp_ramdisk, "-o", args.output])
    finally:
        os.unlink(tmp_kernel)
        os.unlink(tmp_ramdisk)

    raw = open(args.output, "rb").read()
    assert raw[:8] == b"ANDROID!", "not an Android boot image"
    hs = int.from_bytes(raw[20:24], "little")
    hv = int.from_bytes(raw[40:44], "little")
    print(f"  image  {os.path.relpath(args.output)}: {len(raw)} bytes, header v{hv}, header_size {hs}, "
          f"kernel {int.from_bytes(raw[8:12], 'little')} bytes (shim+fd+payload {len(blob)}, dtb {len(dtb)})")
    print(f"  sha256 {hashlib.sha256(raw).hexdigest()}")


if __name__ == "__main__":
    main()
