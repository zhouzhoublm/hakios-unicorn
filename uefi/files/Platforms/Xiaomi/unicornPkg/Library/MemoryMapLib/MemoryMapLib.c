#include <Library/MemoryMapLib.h>

STATIC
EFI_MEMORY_REGION_DESCRIPTOR
gMemoryDescriptor[] = {

  // ===== K60 (unicorn/SM8475) 运行时保留区 + UEFI 系统区（2026-10-27 实测生成）=====
  // Name, Address, Length, HobOption, ResourceType, ResourceAttribute, MemoryType, ArmAttribute

  // DDR Regions —— K60 运行时保留区（实测）+ UEFI 系统区
  {"HYP",         0x80000000, 0x00600000, AddMem, MEM_RES  , SYS_MEM_CAP, Reserv , WRITE_BACK_XN},
  {"DT BLOB",          0x80600000, 0x00040000, AddMem, MEM_RES  , SYS_MEM_CAP, Reserv , WRITE_BACK_XN},
  {"AOP",   0x80800000, 0x00060000, AddMem, MEM_RES  , SYS_MEM_CAP, Reserv , WRITE_BACK_XN},
  {"aop_cmd_db_region",  0x80860000, 0x00020000, AddMem, MEM_RES  , SYS_MEM_CAP, Reserv , WRITE_BACK_XN},
  {"aop_config_region",  0x80880000, 0x00020000, AddMem, MEM_RES  , SYS_MEM_CAP, Reserv , WRITE_BACK_XN},
  {"tme_crash_dump_region", 0x808A0000, 0x00040000, AddMem, MEM_RES  , SYS_MEM_CAP, Reserv , WRITE_BACK_XN},
  {"tme_log_region",     0x808E0000, 0x00004000, AddMem, MEM_RES  , SYS_MEM_CAP, Reserv , WRITE_BACK_XN},
  {"uefi_log_region",    0x808E4000, 0x00010000, AddMem, MEM_RES  , SYS_MEM_CAP, Reserv , WRITE_BACK_XN},
  {"SMEM",        0x80900000, 0x00200000, AddMem, MEM_RES  , SYS_MEM_CAP, Reserv , WRITE_BACK_XN},
  {"CUCP FW",    0x80B00000, 0x00100000, AddMem, MEM_RES  , SYS_MEM_CAP, Reserv , WRITE_BACK_XN},
  {"cdsp_secure_heap_regi", 0x80C00000, 0x04600000, AddMem, MEM_RES  , SYS_MEM_CAP, Reserv , WRITE_BACK_XN},
  {"video_region",       0x85700000, 0x00700000, AddMem, MEM_RES  , UNCACHEABLE, Reserv , UNCACHED_UNBUFFERED_XN},
  {"slpi_region",        0x88000000, 0x01900000, AddMem, MEM_RES  , UNCACHEABLE, Reserv , UNCACHED_UNBUFFERED_XN},
  {"cdsp_region",        0x89900000, 0x02000000, AddMem, MEM_RES  , UNCACHEABLE, Reserv , UNCACHED_UNBUFFERED_XN},
  {"ipa_fw_region",      0x8B900000, 0x00010000, AddMem, MEM_RES  , SYS_MEM_CAP, Reserv , WRITE_BACK_XN},
  {"ipa_gsi_region",     0x8B910000, 0x0000A000, AddMem, MEM_RES  , SYS_MEM_CAP, Reserv , WRITE_BACK_XN},
  {"gpu_microcode_region", 0x8B91A000, 0x00002000, AddMem, MEM_RES  , SYS_MEM_CAP, Reserv , WRITE_BACK_XN},
  {"spss_region_region", 0x8BA00000, 0x00180000, AddMem, MEM_RES  , UNCACHEABLE, Reserv , UNCACHED_UNBUFFERED_XN},
  {"spu_tz_shared_mem",  0x8BB80000, 0x00060000, AddMem, MEM_RES  , UNCACHEABLE, Reserv , UNCACHED_UNBUFFERED_XN},
  {"spu_modem_shared_mem", 0x8BBE0000, 0x00020000, AddMem, MEM_RES  , UNCACHEABLE, Reserv , UNCACHED_UNBUFFERED_XN},
  {"mpss_region",        0x8BC00000, 0x13200000, AddMem, MEM_RES  , UNCACHEABLE, Reserv , UNCACHED_UNBUFFERED_XN},
  {"cvp_region",         0x9EE00000, 0x00700000, AddMem, MEM_RES  , UNCACHEABLE, Reserv , UNCACHED_UNBUFFERED_XN},
  {"camera_region",      0x9F500000, 0x00800000, AddMem, MEM_RES  , UNCACHEABLE, Reserv , UNCACHED_UNBUFFERED_XN},
  {"adsp_region",        0x9FD00000, 0x03100000, AddMem, MEM_RES  , UNCACHEABLE, Reserv , UNCACHED_UNBUFFERED_XN},
  {"xbl_ramdump_region", 0xA6B80000, 0x00280000, AddMem, MEM_RES  , SYS_MEM_CAP, Reserv , WRITE_BACK_XN},
  {"xbl_sc_region",      0xA6E00000, 0x00040000, AddMem, MEM_RES  , SYS_MEM_CAP, Reserv , WRITE_BACK_XN},
  {"global_sync_region", 0xA6F00000, 0x00100000, AddMem, MEM_RES  , SYS_MEM_CAP, Reserv , WRITE_BACK_XN},
  {"ramoops",            0xA7000000, 0x00400000, AddMem, MEM_RES  , SYS_MEM_CAP, Reserv , WRITE_THROUGH},
  // ABL fastboot-boot staging window: everything between "UEFI RESV"
  // (ends 0xA8300000) and "Display Reserved" (starts 0xB8000000) had no
  // descriptor, so it was never mapped and the first read took a level-2
  // translation fault.  ABL parks three things in here:
  //   - the DTB it hands over in x0   (image_base + header + kernel_size,
  //                                    so it drifts with boot.img size:
  //                                    1 MiB image -> 0xB6760000,
  //                                    193 MiB image -> 0xAA760000)
  //   - the concatenated initrd        (0xAA961000, 214.5 MiB = our 192 MiB
  //                                    payload FAT + a ~22 MiB vendor_boot
  //                                    ramdisk tail)
  // One Reserv descriptor covers all of it: readable, never allocatable.
  {"ABL Staging",   0xA8300000, 0x0FD00000, AddMem, MEM_RES  , SYS_MEM_CAP, Reserv , WRITE_BACK_XN},
  {"Display Reserved",  0xB8000000, 0x02B00000, AddMem, MEM_RES  , SYS_MEM_CAP, Reserv , WRITE_THROUGH_XN},
  {"qheebsp_reserved_regi", 0xE0000000, 0x00600000, AddMem, MEM_RES  , UNCACHEABLE, Reserv , UNCACHED_UNBUFFERED_XN},
  {"cpusys_vm_region",   0xE0600000, 0x00400000, AddMem, MEM_RES  , UNCACHEABLE, Reserv , UNCACHED_UNBUFFERED_XN},
  {"hyp_reserved_region", 0xE0A00000, 0x00100000, AddMem, MEM_RES  , UNCACHEABLE, Reserv , UNCACHED_UNBUFFERED_XN},
  {"trust_ui_vm_qrtr",   0xE55F3000, 0x00009000, AddMem, MEM_RES  , SYS_MEM_CAP, Reserv , WRITE_BACK_XN},
  {"trust_ui_vm_vblk0_rin", 0xE55FC000, 0x00004000, AddMem, MEM_RES  , SYS_MEM_CAP, Reserv , WRITE_BACK_XN},
  {"trust_ui_vm_swiotlb", 0xE5600000, 0x00100000, AddMem, MEM_RES  , SYS_MEM_CAP, Reserv , WRITE_BACK_XN},
  {"TZ STATS",     0xE8800000, 0x00100000, AddMem, MEM_RES  , UNCACHEABLE, Reserv , UNCACHED_UNBUFFERED_XN},
  {"tags_region",        0xE8900000, 0x01200000, AddMem, MEM_RES  , UNCACHEABLE, Reserv , UNCACHED_UNBUFFERED_XN},
  {"QTEE",        0xE9B00000, 0x00500000, AddMem, MEM_RES  , UNCACHEABLE, Reserv , UNCACHED_UNBUFFERED_XN},
  // ---- UEFI 系统区（K60 布局）----
  {"UEFI FD",            0xA7400000, 0x00300000, AddMem, SYS_MEM  , SYS_MEM_CAP, BsData , WRITE_BACK},
  {"FD Reserved",   0xA7700000, 0x00200000, AddMem, SYS_MEM  , SYS_MEM_CAP, BsData , WRITE_BACK},
  {"PMIC ARB SPMI",      0x0C400000, 0x2800000, AddDev, MMAP_IO  , UNCACHEABLE, MmIO   , UNCACHED_UNBUFFERED_XN},
  {"CPU Vectors",      0xA7900000, 0x00001000, AddMem, SYS_MEM  , SYS_MEM_CAP, BsData , WRITE_BACK},
  {"Info Blk",           0xA7901000, 0x00001000, AddMem, SYS_MEM  , SYS_MEM_CAP, RtData , WRITE_BACK_XN},
  {"MMU PageTables",     0xA7902000, 0x00003000, AddMem, SYS_MEM  , SYS_MEM_CAP, BsData , WRITE_BACK_XN},
  {"Log Buffer",         0xA7905000, 0x00008000, AddMem, SYS_MEM  , SYS_MEM_CAP, RtData , WRITE_BACK_XN},
  {"UEFI Stack",         0xA790D000, 0x00040000, AddMem, SYS_MEM  , SYS_MEM_CAP, BsData , WRITE_BACK_XN},
  {"SEC Heap",           0xA794D000, 0x0008C000, AddMem, SYS_MEM  , SYS_MEM_CAP, BsData , WRITE_BACK_XN},
  {"Sched Heap",         0xA79D9000, 0x00400000, AddMem, SYS_MEM  , SYS_MEM_CAP, BsData , WRITE_BACK_XN},
  {"FV Region",          0xA7DD9000, 0x00400000, AddMem, SYS_MEM  , SYS_MEM_CAP, BsData , WRITE_BACK_XN},
  {"UEFI RESV",          0xA81D9000, 0x00127000, AddMem, SYS_MEM  , SYS_MEM_CAP, BsData , WRITE_BACK_XN},
  {"Kernel",             0xBB000000, 0x0C000000, AddMem, SYS_MEM  , SYS_MEM_CAP, Reserv , WRITE_BACK_XN},
  // 384 MiB from 0xC7000000 to 0xDF000000. Two constraints drove this:
  //   - UniXNU reads Payloads/kernel.macho (52 MiB) with one AllocateZeroPool
  //     and only frees it after DarwinBoot has taken its window, so the
  //     image sits in the heap while the window is being reserved;
  //   - DarwinBoot asks for one CONTIGUOUS 256 MiB run (WINDOW_SIZE) and
  //     failed with "biggest run 0xF0A0000" when the heap was 320 MiB.
  // "Kernel" above keeps 192 MiB as a reservation for XNU itself.
  {"DXE Heap",           0xC7000000, 0x18000000, AddMem, SYS_MEM  , SYS_MEM_CAP, Conv   , WRITE_BACK_XN},

  // 所以 DarwinBoot 能找到的最大连续空闲段只有 ~306 MiB，XNU 窗口被卡在 288 MiB。
  // 这里把这段高位 DRAM 描述成普通可用内存，给内核一个足够大的连续窗口。

  // Other Memory Regions
  {"IMEM Base",          0x14680000, 0x0002A000, NoHob , MMAP_IO  , INITIALIZED, Conv   , NS_DEVICE},
  {"IMEM Cookie Base",   0x146AA000, 0x00016000, AddDev, MMAP_IO  , INITIALIZED, Conv   , NS_DEVICE},

  // Register Regions（SM8475 与 pong 同 SoC，寄存器布局相同）
  {"IPC_ROUTER_TOP",     0x00400000, 0x00100000, AddDev, MMAP_IO  , UNCACHEABLE, MmIO   , NS_DEVICE},
  {"SECURITY CONTROL",   0x00780000, 0x00007000, AddDev, MMAP_IO  , UNCACHEABLE, MmIO   , NS_DEVICE},
  {"QUP",                0x00800000, 0x00300000, AddDev, MMAP_IO  , UNCACHEABLE, MmIO   , NS_DEVICE},

  // FAULT WINDOW 0x7D: 交棒后有一次访问落在 0x07D7B14，而这一段在我们表里完全没有
  // 描述（QUP 到 0x00B00000 结束，下一个是 0x0C400000 的 PMIC），实测触发
  // "Synchronous external abort (ESR EC 0x25 ISS 0x10)"。先按设备窗口描述出来。
  {"TCSR 0x7D window",   0x07D00000, 0x00300000, AddDev, MMAP_IO  , UNCACHEABLE, MmIO   , NS_DEVICE},
  {"PRNG_CFG_PRNG",      0x010C0000, 0x0000C000, AddDev, MMAP_IO  , UNCACHEABLE, MmIO   , NS_DEVICE},
  {"CRYPTO0 CRYPTO",     0x01DC0000, 0x00040000, AddDev, MMAP_IO  , UNCACHEABLE, MmIO   , NS_DEVICE},
  {"TCSR_TCSR_REGS",     0x01FC0000, 0x00030000, AddDev, MMAP_IO  , UNCACHEABLE, MmIO   , NS_DEVICE},
  {"PERIPH_SS",          0x08800000, 0x00100000, AddDev, MMAP_IO  , UNCACHEABLE, MmIO   , NS_DEVICE},
  {"USB",                0x0A600000, 0x00200000, AddDev, MMAP_IO  , UNCACHEABLE, MmIO   , NS_DEVICE},
  {"AOSS",               0x0B000000, 0x04000000, AddDev, MMAP_IO  , UNCACHEABLE, MmIO   , NS_DEVICE},
  {"TLMM",               0x0F000000, 0x01000000, AddDev, MMAP_IO  , UNCACHEABLE, MmIO   , NS_DEVICE},
  {"SMMU",               0x15000000, 0x00200000, AddDev, MMAP_IO  , UNCACHEABLE, MmIO   , NS_DEVICE},
  {"APSS_HM",            0x17000000, 0x02000000, AddDev, MMAP_IO  , UNCACHEABLE, MmIO   , NS_DEVICE},
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
