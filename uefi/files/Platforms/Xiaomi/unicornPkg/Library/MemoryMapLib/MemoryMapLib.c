// unicorn: the SM8475 (cape) platform's memory map -- Silicium's b4q, the reference device of this SoC, as
// the Xiaomi 12S port uses it. Every entry the Xiaomi 12S Pro's own device tree names was checked against
// that tree (cape-reserved-memory.dtsi, xiaomi-sm8475-common.dtsi: hyp 0x80000000/0x600000, xbl_dtlog
// 0x80600000/0x40000, aop 0x80800000..0x808a0000, smem 0x80900000/0x200000, cpucp 0x80b00000/0x100000,
// the modem's carve-outs from 0x85200000, imem 0x146aa000, the QTEE/TZ block from 0xe0000000) and agrees;
// the ones the tree does not name are the platform's display, UEFI and DXE-heap regions, which is where a
// UEFI image has to live on this family. Nothing here is a number guessed for this phone: when the phone
// boots, its log's [memmap] dump (xnuloader's) is what this file gets corrected against.
#include <Library/MemoryMapLib.h>

STATIC
EFI_MEMORY_REGION_DESCRIPTOR
gMemoryDescriptor[] = {
  // Name, Address, Length, HobOption, ResourceType, ResourceAttribute, MemoryType, ArmAttribute

  // DDR Regions
  // HakiOS: carve-outs this VM does not own stay out of the page tables: Qualcomm XPUs reset the SoC on the
  // prefetcher's speculative reads of a cacheable mapping, which shows up as a freeze at a random point.
  {"HYP",                0x80000000, 0x00600000, HobOnlyNoCacheSetting, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK_XN},
  {"DT BLOB",            0x80600000, 0x00040000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, UNCACHED_UNBUFFERED_XN},
  {"AOP",                0x80800000, 0x000A0000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"SMEM",               0x80900000, 0x00200000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"CUCP",               0x80B00000, 0x00100000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"PIL Reserved",       0x85200000, 0x1C200000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"Display Demura",     0xA1400000, 0x02B00000, HobOnlyNoCacheSetting, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_THROUGH_XN},
  {"DBI Dump",           0xA6000000, 0x00F00000, NoHob,  MMAP_IO, INITIALIZED, Conv,   UNCACHED_UNBUFFERED_XN},
  {"FD Reserved",        0xA7000000, 0x00100000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK},
  {"UEFI FD",            0xA7100000, 0x00300000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK},
  {"UEFI FD Reserved",   0xA7400000, 0x00200000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK},
  {"Info Blk",           0xA7601000, 0x00001000, AddMem, SYS_MEM, SYS_MEM_CAP, RtData, WRITE_BACK_XN},
  {"MMU PageTables",     0xA7602000, 0x00003000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK_XN},
  {"Log Buffer",         0xA7605000, 0x00008000, AddMem, SYS_MEM, SYS_MEM_CAP, RtData, WRITE_BACK_XN},
  {"UEFI Stack",         0xA760D000, 0x00040000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK_XN},
  {"SEC Heap",           0xA764D000, 0x0008C000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK_XN},
  {"Sched Heap",         0xA76D9000, 0x00400000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK_XN},
  {"FV Region",          0xA7AD9000, 0x00400000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK_XN},
  {"Kernel",             0xA8000000, 0x10000000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK_XN},
  {"Display Reserved",   0xB8000000, 0x02B00000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_THROUGH_XN},
  {"OEM VM",             0xBB000000, 0x05000000, HobOnlyNoCacheSetting, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK_XN},
  {"MTE Reserved",       0xC0000000, 0x07000000, HobOnlyNoCacheSetting, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK_XN},
  // HakiOS: the same place Redmi K60's (SM8475) port keeps it: 384 MiB, free RAM in the live device tree, big enough for
  // the kernel's window
  {"DXE Heap",           0xC7000000, 0x18000000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK_XN},
  {"MTE Reserved 2",     0xDF000000, 0x01000000, HobOnlyNoCacheSetting, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK_XN},
  {"VM PIL",             0xE0000000, 0x05700000, HobOnlyNoCacheSetting, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK_XN},
  {"TZ STATS",           0xE8800000, 0x01000000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"TAGS",               0xE9800000, 0x00300000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"QTEE",               0xE9B00000, 0x00500000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"TZApps Reserved",    0xEA000000, 0x07400000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"Stock DXE Heap",     0xF8000000, 0x07C00000, HobOnlyNoCacheSetting, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK_XN},
  {"HYP Reserved",       0x830000000,0x10000000, HobOnlyNoCacheSetting, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK_XN},

  // HakiOS: where the boot shim leaves the "XNUP" payload (XNU_PAYLOAD_BASE, 0xB0000000 on this device -- the
  // DSC's BuildOptions and mkpayload.py agree): inside "Kernel" (0xA8000000 + 0x10000000), far above the boot
  // image ABL unpacks at its start, and reserved, so UEFI leaves it alone. 0xF0000000, HakiOS's default, is
  // inside this family's TZ apps region below ("TZApps Reserved" 0xEA000000 + 0x07400000) and resets the phone.

  // Other Memory Regions
  {"IMEM Base",          0x14680000, 0x0002A000, NoHob,  MMAP_IO, INITIALIZED, Conv,   NS_DEVICE},
  {"IMEM Cookie Base",   0x146AA000, 0x00016000, AddDev, MMAP_IO, INITIALIZED, Conv,   NS_DEVICE},

  // Register Regions
  //
  // GCC: the clock/power controller at 0x00100000. It is not a Qualcomm "register region" in the original
  // map because the blob-free boot never touched it — the USB function work does (USB30 GDSC, the USB
  // clocks, the QUP votes and the eUSB2 PHY resets all live here, docs/USB-BRINGUP.md §4). Unmapped MMIO
  // in UEFI is a synchronous abort, not a read of zeros, so it has to be declared.
  {"GCC",                0x00100000, 0x00100000, AddDev, MMAP_IO, INITIALIZED, Conv,   NS_DEVICE},
  {"IPC_ROUTER_TOP",     0x00400000, 0x00100000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"SECURITY CONTROL",   0x00780000, 0x00007000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"QUP",                0x00800000, 0x00300000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"PRNG_CFG_PRNG",      0x010C0000, 0x0000C000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"CRYPTO0 CRYPTO",     0x01DC0000, 0x00040000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"TCSR_TCSR_REGS",     0x01FC0000, 0x00030000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"PERIPH_SS",          0x08800000, 0x00100000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"USB",                0x0A600000, 0x00200000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"AOSS",               0x0B000000, 0x04000000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"TLMM",               0x0F000000, 0x01000000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"SMMU",               0x15000000, 0x00200000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"APSS_HM",            0x17000000, 0x02000000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE}
};

VOID
GetMemoryMap (
  OUT EFI_MEMORY_REGION_DESCRIPTOR **MemoryDescriptor,
  OUT UINT8                         *MemoryDescriptorCount)
{
  // Pass Data
  *MemoryDescriptor      = gMemoryDescriptor;
  *MemoryDescriptorCount = ARRAY_SIZE (gMemoryDescriptor);
}
