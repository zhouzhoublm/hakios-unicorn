#!/bin/zsh
# usage: build-image.sh NAME "extra boot-args"   -> xnu-NAME.img at the top of hakios-xnu-kernel (kernel+userland must be built)
set -e
cd "$(cd "$(dirname "$0")" && pwd)/../../.."   # the top of hakios-xnu-kernel (this repo sits in devices/mayfly)
export PATH=/opt/homebrew/opt/llvm/bin:$PWD/xnu-deps/host/bin:$PWD/tools/macos-bin:/opt/homebrew/opt/bison/bin:$PATH
T=${LOGDIR:-$HOME/hakios-mayfly-logs}; mkdir -p $T
C="loader_return_after=300 root_apfs=ufs_darwin ufs=2 bootcpu=7 cpus=7 touchscreen=1 touch_bitbang_ticks=4 gpu=1 panel_hz=120 fb_hz=120 ca_vbl=1 fb_flip=1"
xnuloader/mkpayload.py --usb -o $T/p-$1.bin loader.efi=xnuloader/loader.efi kernel=hakios-kernel ramdisk=userland/root.apfs resident=userland/flashboot.apfs --cmdline "epoch=$(date +%s) $C $2" >/dev/null
./mkxnuimg.py -d mayfly --mode DEBUG -p $T/p-$1.bin -o xnu-$1.img | tail -1
