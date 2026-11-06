/*
 * The GPU of the Xiaomi 12S (mayfly), an Adreno 730 ("gen7", chip ID 0x07030002), woken step by step the way
 * drivers/gpu/hakios.adreno.c wakes redwood's Adreno 642L. Every address below belongs to this phone, and nothing
 * is touched unless boot-arg adreno7=N asks for it.
 *
 * This is the first part: up to the first words read from the GPU block. Each step says what it is about to
 * touch before it touches it, reads come before writes, and a step that does not find what it needs ends the
 * sequence.
 *
 *   1  reads   the GPU's clocks in the global clock controller, and its supply rails in RPMh's list
 *   2  reads   the GPU's own clock controller, as the boot loader left it
 *   3  writes  the "CX" power switch on: the GMU, the bus interface, the GPU's SMMU
 *   4  writes  the clocks of that half on
 *   5  reads   the first words from the GPU block: the GMU (with a counter that should be seen counting), the
 *              bus interface, the CX odds and ends
 *   6  reads   the GMU's channel to RPMh
 *   7  reads   the GPU's SMMU: its ID registers, its stream table
 *   8  reads   that SMMU's first translation context
 *   9  writes  two words into the GMU's memory and reads them back: the way its firmware goes
 *  10  asks    TrustZone questions that pass no memory: whether it takes calls from this kernel
 *  11  asks    TrustZone to check the signature of the GPU's zap shader (fw/a730_zap.mdt in the small image)
 *  12  writes  page tables for the memory the GMU's firmware asks for, and the SMMU: context 1 with those tables, and
 *              the GMU's requests (stream 5) through it
 *  13  writes  the GMU's firmware (fw/gmu_gen70000.bin) into its own memory and into that memory
 *  14  writes  the GMU's configuration, wakes its RPMh channel, lets its processor out of reset; starts its message
 *              queues
 *  15  talks   to the GMU: its tables, then the request to power the 3D core, whose registers are then read
 *  16  writes  the 3D core's translation context, with its ring and its microcode (fw/a730_sqe.fw) in it
 *  17  writes  the 3D core's registers; starts its command processor and initializes it
 *  18  asks    TrustZone to load the zap shader; commands the core out of secure mode and to write a word of ours
 *
 * More boot-args: adreno7_pause=MS (how long the newest step waits after announcing a group of accesses, default
 * 1500, so that the loader's copy of the log has the line before a silent reset), adreno7_fine=1 (wait before every
 * access).
 *
 * Addresses: the phone's own device tree (clock-controller@3d90000 "qcom,cape-gpucc", qcom,gdsc@3d99108,
 * qcom,gdsc@3d9905c, qcom,gmu@3d69000, qcom,kgsl-3d0@3d00000 "qcom,adreno-gpu-gen7-4-0"), register layouts from
 * Linux 6.12: drivers/clk/qcom/gcc-sm8450.c and gpucc-sm8450.c (the device tree's GPU clock controller offsets are
 * the same as these), drivers/gpu/drm/msm/adreno/a6xx_gmu.c and registers/adreno/a6xx_gmu.xml, which serve the
 * Adreno 730 too.
 */
#include <kern/clock.h>
#include <kern/startup.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <sys/errno.h>
#include <sys/sysctl.h>
#include <sys/hakios.qrtr.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_putc(char c);
extern void IOSleep(unsigned int milliseconds);
extern void IODelay(unsigned int microseconds);
extern int cpu_number(void);
extern void timer_resync_deadlines(void);
extern uint64_t kvtophys(vm_offset_t va);

static uint32_t last_step, pause_ms = 1500, fine;
static char last_write[96] = "nothing written";
static uint32_t step;

static int say(const char *format, ...) __printflike(1, 2);

/* Every line goes into the loader's log as it is said, before the next register is touched. */
static int
say(const char *format, ...)
{
	char line[400];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	return printf("%s", line);
}
#define printf say

/*
 * Boot-arg adreno7_spin=1: every wait looks at the clock instead of putting the thread to sleep (seen: after
 * TrustZone authenticated the zap shader the thread never came back from a 1.5 s sleep, while the rest of the
 * kernel went on).
 */
static uint32_t spin_waits;

static void
spin_ns(uint64_t ns)
{
	uint64_t until;

	nanoseconds_to_absolutetime(ns, &until);
	until += mach_absolute_time();
	while (mach_absolute_time() < until) {
	}
}

static void
sleep_us(uint32_t us)
{
	if (spin_waits) {
		spin_ns((uint64_t)us * NSEC_PER_USEC);
		return;
	}
	delay_for_interval(us, NSEC_PER_USEC);
}

static void
sleep_ms(uint32_t ms)
{
	if (spin_waits) {
		spin_ns((uint64_t)ms * NSEC_PER_MSEC);
		return;
	}
	delay_for_interval(ms, NSEC_PER_MSEC);
}

/* ---- register access (plain loads and stores: some of this may be the hypervisor's imitation) ----------- */

static uint32_t __attribute__((noinline))
rd(vm_offset_t address)
{
	uint32_t value;

	__asm__ volatile ("ldr %w0, [%1]" : "=r" (value) : "r" (address) : "memory");
	return value;
}

static void __attribute__((noinline))
wr(vm_offset_t address, uint32_t value)
{
	__asm__ volatile ("str %w0, [%1]" : : "r" (value), "r" (address) : "memory");
}

struct block {
	const char *name;
	uint32_t phys, size;
	vm_offset_t base;
};

static void
need(struct block *b)
{
	if (b->base == 0) {
		b->base = ml_io_map(b->phys, b->size);
	}
}

static void
hold(bool single_access)
{
	if (step == last_step && pause_ms && (!single_access || fine)) {
		sleep_ms(pause_ms);
	}
}

static void
announce(const char *what)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	printf("adreno: step %u (%llu s, cpu %d): %s\n", step, ns / NSEC_PER_SEC, cpu_number(), what);
	hold(false);
}

static uint32_t
get(struct block *b, const char *name, uint32_t offset)
{
	uint32_t value;

	need(b);
	hold(true);
	value = rd(b->base + offset);
	if (step == last_step) {        /* the steps before it have been seen to work: their reads are not news */
		printf("adreno: %u: %s %s (%#x) = %#x\n", step, b->name, name, b->phys + offset, value);
	}
	return value;
}

static void
put(struct block *b, const char *name, uint32_t offset, uint32_t value)
{
	need(b);
	snprintf(last_write, sizeof(last_write), "step %u wrote %#x to %s %s at %#x", step, value, b->name, name,
	    b->phys + offset);
	if (step == last_step) {
		printf("adreno: %u: write %s %s at %#x: %#x\n", step, b->name, name, b->phys + offset, value);
	}
	hold(true);
	wr(b->base + offset, value);
}

static uint32_t
set(struct block *b, const char *name, uint32_t offset, uint32_t bits)
{
	uint32_t was = get(b, name, offset);

	if ((was & bits) == bits) {
		return was;
	}
	put(b, name, offset, was | bits);
	return get(b, name, offset);
}

static bool
wait_for(struct block *b, uint32_t offset, uint32_t mask, uint32_t value, uint32_t ms)
{
	for (uint32_t i = 0; i < ms * 10; i++) {
		if ((rd(b->base + offset) & mask) == value) {
			return true;
		}
		sleep_us(100);
	}
	return (rd(b->base + offset) & mask) == value;
}

/* ---- step 1: the clocks from outside, and RPMh's names ------------------------------------------------ */

/* gcc-sm8450.c */
static struct block gcc = { "gcc", 0x00100000, 0x1f4200 };
#define GCC_PLL_VOTE            0x62000         /* bit 15: gcc_gpu_gpll0_clk_src (600 MHz), bit 16: ..._div_clk_src (300 MHz) */
#define GCC_PLL_VOTE_GPU_DIV    (1u << 16)
#define GCC_PLL_VOTE_GPU        (1u << 15)
#define GCC_GPU_CFG_AHB_CBCR    0x81004         /* always on: Linux turns it on at probe */
#define GCC_GPU_MEMNOC_GFX_CBCR 0x81010
#define GCC_GPU_SNOC_DVM_GFX_CBCR 0x81018
#define GCC_DDRSS_GPU_AXI_CBCR  0x81154
#define CBCR_ENABLE             (1u << 0)
#define CBCR_OFF                (1u << 31)

#define CMD_DB_PHYS             0x80860000UL
#define CMD_DB_SIZE             0x20000UL

struct cmd_db_slave {
	uint16_t id, header_offset, data_offset, count, version, reserved[3];
};
struct cmd_db_header {
	uint32_t version;
	uint8_t magic[4];
	struct cmd_db_slave slave[8];
	uint32_t checksum, reserved;
	uint8_t data[];
};
struct cmd_db_entry {
	char id[8];
	uint32_t priority[2];
	uint32_t address;
	uint16_t length, offset;
};

static const struct cmd_db_header *cmd_db;

static void
show_rpmh_resource(const char *name)
{
	char id[8] = { 0 }, levels[160];
	const struct cmd_db_header *db;
	int at = 0;

	if (cmd_db == NULL) {
		cmd_db = (const struct cmd_db_header *)ml_io_map_wcomb(CMD_DB_PHYS, CMD_DB_SIZE);
	}
	db = cmd_db;
	if (db->magic[0] != 0xdb || db->magic[1] != 0x30 || db->magic[2] != 0x03 || db->magic[3] != 0x0c) {
		printf("adreno: %u: RPMh's list is not where it should be\n", step);
		return;
	}
	strlcpy(id, name, sizeof(id));
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(db->data + slave->header_offset);

		if (slave->id == 0 || slave->count > 512) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			const uint8_t *data = db->data + slave->data_offset + entry[i].offset;

			if (memcmp(entry[i].id, id, sizeof(id)) != 0) {
				continue;
			}
			levels[0] = 0;
			for (unsigned l = 0; l < entry[i].length / 2u && l < 16 && at < (int)sizeof(levels) - 8; l++) {
				uint32_t level = data[2 * l] | data[2 * l + 1] << 8;

				if (l > 0 && level == 0) {
					break;
				}
				at += snprintf(levels + at, sizeof(levels) - (size_t)at, " %u", level);
			}
			printf("adreno: %u: RPMh has \"%s\" at %#x; levels:%s\n", step, name, entry[i].address, levels);
			return;
		}
	}
	printf("adreno: %u: RPMh has no \"%s\"\n", step, name);
}

static bool
step_outside(void)
{
	announce("read the GPU's clocks in the global clock controller");
	get(&gcc, "pll votes", GCC_PLL_VOTE);
	get(&gcc, "gpu_cfg_ahb", GCC_GPU_CFG_AHB_CBCR);
	get(&gcc, "gpu_memnoc_gfx", GCC_GPU_MEMNOC_GFX_CBCR);
	get(&gcc, "gpu_snoc_dvm_gfx", GCC_GPU_SNOC_DVM_GFX_CBCR);
	get(&gcc, "ddrss_gpu_axi", GCC_DDRSS_GPU_AXI_CBCR);
	announce("look up the GPU's supply rails in RPMh's list (memory at 0x80860000)");
	show_rpmh_resource("gfx.lvl");
	show_rpmh_resource("cx.lvl");
	show_rpmh_resource("mx.lvl");
	show_rpmh_resource("gmxc.lvl");
	return true;
}

/* ---- step 2: the GPU's clock controller ----------------------------------------------------------------- */

/* gpucc-sm8450.c; the device tree's gdsc and reset addresses agree */
static struct block gpucc = { "gpucc", 0x03d90000, 0xa000 };
#define GPUCC_PLL0_MODE         0x0000          /* the 3D core's PLL (Lucid EVO): bit 31 locked; +4: L */
#define GPUCC_PLL1_MODE         0x1000          /* 500 MHz x2, an alternative source for the GMU's clock */
#define GPUCC_SMMU_VOTE_CBCR    0x7000          /* gpu_cc_hlos1_vote_gpu_smmu_clk */
#define GPUCC_CXO_AON_CBCR      0x9004
#define GPUCC_GX_BCR            0x9058          /* dt: gpu_gx_sw_reset */
#define GPUCC_GX_GDSCR          0x905c          /* dt: gpu_gx_gdsc: the 3D core's power switch, the GMU's to operate */
#define GPUCC_GX_GMU_CBCR       0x90bc
#define GPUCC_AHB_CBCR          0x911c          /* gpu_cc_ahb_clk: the register bus inside the GPU block */
#define GPUCC_CX_APB_CBCR       0x912c          /* "apb_pclk" in the device tree */
#define GPUCC_CX_SNOC_DVM_CBCR  0x9130
#define GPUCC_CX_GMU_CBCR       0x913c          /* gpu_cc_cx_gmu_clk: the GMU */
#define GPUCC_CXO_CBCR          0x9144
#define GPUCC_HUB_CX_INT_CBCR   0x9148
#define GPUCC_MEMNOC_GFX_CBCR   0x9150
#define GPUCC_CX_GDSCR          0x9108          /* dt: gpu_cc_cx_gdsc */
#define GPUCC_GMU_RCGR          0x9318          /* gpu_cc_gmu_clk_src: +0 command, +4 source and divider */
#define GPUCC_HUB_AON_CBCR      0x93e8
#define GPUCC_HUB_RCGR          0x93ec          /* gpu_cc_hub_clk_src */
#define GPUCC_GX_CLAMP          0x9504          /* dt: gpu_cc_gx_domain_addr */
#define GPUCC_CX_HW_CTRL        0x953c          /* gds_hw_ctrl of the CX switch: bit 31 = it has power */

#define GDSC_SW_COLLAPSE        (1u << 0)
#define GDSC_HW_CONTROL         (1u << 1)
#define GDSC_POWER_ON           (1u << 31)

static bool
gpucc_answers(void)
{
	need(&gcc);
	need(&gpucc);
	if (rd(gcc.base + GCC_GPU_CFG_AHB_CBCR) & CBCR_OFF) {
		printf("adreno: %u: the GPU clock controller's registers have no clock (gcc gpu_cfg_ahb %#x)\n", step,
		    rd(gcc.base + GCC_GPU_CFG_AHB_CBCR));
		return false;
	}
	return true;
}

static bool
step_gpucc(void)
{
	announce("make sure the clock of the GPU clock controller's registers runs (gcc_gpu_cfg_ahb)");
	if ((set(&gcc, "gpu_cfg_ahb", GCC_GPU_CFG_AHB_CBCR, CBCR_ENABLE) & CBCR_OFF) &&
	    !wait_for(&gcc, GCC_GPU_CFG_AHB_CBCR, CBCR_OFF, 0, 10)) {
		printf("adreno: %u: it stays off: the GPU's clock controller would not answer\n", step);
		return false;
	}
	announce("read the GPU's clock controller (0x3d90000) as it is: the first access to the GPU's part of the SoC");
	get(&gpucc, "cx_gdscr", GPUCC_CX_GDSCR);
	get(&gpucc, "cx_hw_ctrl", GPUCC_CX_HW_CTRL);
	get(&gpucc, "gx_gdscr", GPUCC_GX_GDSCR);
	get(&gpucc, "gx_bcr", GPUCC_GX_BCR);
	get(&gpucc, "gx_clamp", GPUCC_GX_CLAMP);
	get(&gpucc, "pll0 mode", GPUCC_PLL0_MODE);
	get(&gpucc, "pll0 l", GPUCC_PLL0_MODE + 4);
	get(&gpucc, "pll1 mode", GPUCC_PLL1_MODE);
	get(&gpucc, "pll1 l", GPUCC_PLL1_MODE + 4);
	get(&gpucc, "gmu_clk_src command", GPUCC_GMU_RCGR);
	get(&gpucc, "gmu_clk_src source", GPUCC_GMU_RCGR + 4);
	get(&gpucc, "hub_clk_src command", GPUCC_HUB_RCGR);
	get(&gpucc, "hub_clk_src source", GPUCC_HUB_RCGR + 4);
	get(&gpucc, "ahb", GPUCC_AHB_CBCR);
	get(&gpucc, "cx_gmu", GPUCC_CX_GMU_CBCR);
	get(&gpucc, "cxo", GPUCC_CXO_CBCR);
	get(&gpucc, "hub_aon", GPUCC_HUB_AON_CBCR);
	get(&gpucc, "hub_cx_int", GPUCC_HUB_CX_INT_CBCR);
	get(&gpucc, "cx_apb", GPUCC_CX_APB_CBCR);
	get(&gpucc, "smmu vote", GPUCC_SMMU_VOTE_CBCR);
	return true;
}

/* ---- step 3: CX power --------------------------------------------------------------------------------- */

static bool
cx_is_on(void)
{
	return (rd(gpucc.base + GPUCC_CX_GDSCR) | rd(gpucc.base + GPUCC_CX_HW_CTRL)) & GDSC_POWER_ON;
}

static bool
step_cx_power(void)
{
	uint32_t gdscr;

	if (!gpucc_answers()) {
		return false;
	}
	announce("switch the CX half of the GPU block on (gpu_cc_cx_gdsc): clear the collapse bit of its power switch");
	gdscr = get(&gpucc, "cx_gdscr", GPUCC_CX_GDSCR);
	get(&gpucc, "cx_hw_ctrl", GPUCC_CX_HW_CTRL);
	if (cx_is_on()) {
		printf("adreno: %u: CX is on already\n", step);
		return true;
	}
	if (gdscr & GDSC_HW_CONTROL) {
		printf("adreno: %u: the switch is under hardware control: not ours to operate\n", step);
		return false;
	}
	put(&gpucc, "cx_gdscr", GPUCC_CX_GDSCR, gdscr & ~GDSC_SW_COLLAPSE);
	for (int i = 0; i < 500 && !cx_is_on(); i++) {
		sleep_us(100);
	}
	get(&gpucc, "cx_gdscr", GPUCC_CX_GDSCR);
	get(&gpucc, "cx_hw_ctrl", GPUCC_CX_HW_CTRL);
	if (!cx_is_on()) {
		printf("adreno: %u: CX does not come on\n", step);
		return false;
	}
	printf("adreno: %u: CX is on\n", step);
	if (rd(gpucc.base + GPUCC_GX_GDSCR) & GDSC_POWER_ON) {
		printf("adreno: %u: the 3D core has power already (gx_gdscr %#x)\n", step, rd(gpucc.base + GPUCC_GX_GDSCR));
	}
	return true;
}

/* ---- step 4: clocks ------------------------------------------------------------------------------------ */

#define RCG_UPDATE              (1u << 0)
#define RCG_SOURCE_MASK         0x371fu         /* [4:0] 2*divider-1, [10:8] source, [13:12] mode */
#define RCG_GPLL0_DIV(twice)    (6u << 8 | ((twice) - 1))       /* source 6: the 300 MHz tap (parent maps 1 and 2) */

static bool
clock_on(struct block *b, const char *name, uint32_t cbcr, bool must)
{
	if (!(set(b, name, cbcr, CBCR_ENABLE) & CBCR_OFF) || wait_for(b, cbcr, CBCR_OFF, 0, 10)) {
		return true;
	}
	printf("adreno: %u: %s %s stays off (%#x)%s\n", step, b->name, name, rd(b->base + cbcr),
	    must ? "" : "; its status is not one to rely on, going on");
	return !must;
}

static bool
clock_source(const char *name, uint32_t rcgr, uint32_t source)
{
	uint32_t was = get(&gpucc, name, rcgr + 4);

	if ((was & RCG_SOURCE_MASK) == source) {
		return true;
	}
	put(&gpucc, name, rcgr + 4, (was & ~RCG_SOURCE_MASK) | source);
	put(&gpucc, name, rcgr, rd(gpucc.base + rcgr) | RCG_UPDATE);
	if (!wait_for(&gpucc, rcgr, RCG_UPDATE, 0, 10)) {
		printf("adreno: %u: %s does not take its new source (command %#x)\n", step, name, rd(gpucc.base + rcgr));
		return false;
	}
	get(&gpucc, name, rcgr + 4);
	return true;
}

/* What Linux brings the GMU up with (a6xx_gmu_resume): the GMU at 200 MHz, the hub at 150 MHz, from the 300 MHz tap. */
static bool
step_clocks(void)
{
	if (!gpucc_answers()) {
		return false;
	}
	if (!cx_is_on()) {
		printf("adreno: %u: CX is off: step 3\n", step);
		return false;
	}
	announce("in the global clock controller: the 300 MHz tap for the GPU; the GPU's bus and memory clocks on");
	set(&gcc, "pll votes", GCC_PLL_VOTE, GCC_PLL_VOTE_GPU_DIV);
	if (!clock_on(&gcc, "gpu_memnoc_gfx", GCC_GPU_MEMNOC_GFX_CBCR, false) ||
	    !clock_on(&gcc, "gpu_snoc_dvm_gfx", GCC_GPU_SNOC_DVM_GFX_CBCR, false) ||
	    !clock_on(&gcc, "ddrss_gpu_axi", GCC_DDRSS_GPU_AXI_CBCR, false)) {
		return false;
	}
	announce("in the GPU's clock controller: the GMU's clock to 200 MHz, the hub's to 150 MHz");
	if (!clock_source("gmu_clk_src", GPUCC_GMU_RCGR, RCG_GPLL0_DIV(3)) ||
	    !clock_source("hub_clk_src", GPUCC_HUB_RCGR, RCG_GPLL0_DIV(4))) {
		return false;
	}
	announce("in the GPU's clock controller: the clocks of the GMU, the register bus and the SMMU on");
	if (!clock_on(&gpucc, "cxo", GPUCC_CXO_CBCR, true) ||
	    !clock_on(&gpucc, "ahb", GPUCC_AHB_CBCR, false) ||
	    !clock_on(&gpucc, "cx_apb", GPUCC_CX_APB_CBCR, false) ||
	    !clock_on(&gpucc, "hub_aon", GPUCC_HUB_AON_CBCR, true) ||
	    !clock_on(&gpucc, "hub_cx_int", GPUCC_HUB_CX_INT_CBCR, true) ||
	    !clock_on(&gpucc, "cx_snoc_dvm", GPUCC_CX_SNOC_DVM_CBCR, false) ||
	    !clock_on(&gpucc, "memnoc_gfx", GPUCC_MEMNOC_GFX_CBCR, false) ||
	    !clock_on(&gpucc, "cx_gmu", GPUCC_CX_GMU_CBCR, true) ||
	    !clock_on(&gpucc, "smmu vote", GPUCC_SMMU_VOTE_CBCR, false)) {
		return false;
	}
	get(&gpucc, "ahb", GPUCC_AHB_CBCR);
	return true;
}

/* ---- step 5: the first words from the GPU block ------------------------------------------------------- */

/*
 * The GMU's registers: a6xx_gmu.xml counts in words from 0x3d6a000 (Linux's sm8450.dtsi: gmu@3d6a000; the vendor's
 * device tree starts the same block 0x2000 lower).
 */
static struct block gmu = { "gmu", 0x03d6a000, 0x35000 };
#define GMU_WORD(n)             (4u * (n))
#define GMU_CM3_SYSRESET        GMU_WORD(0x5000)
#define GMU_CM3_BOOT_CONFIG     GMU_WORD(0x5001)
#define GMU_CM3_FW_INIT_RESULT  GMU_WORD(0x501c)
#define GMU_CM3_CFG             GMU_WORD(0x502d)
#define GMU_ALWAYS_ON_COUNTER_L GMU_WORD(0x5088)
#define GMU_ALWAYS_ON_COUNTER_H GMU_WORD(0x5089)
#define GMU_RPMH_POWER_STATE    GMU_WORD(0x50ec)
#define GMU_HFI_CTRL_STATUS     GMU_WORD(0x5180)
#define GMU_HFI_VERSION_INFO    GMU_WORD(0x5181)
#define GMU_AO_HOST_INTERRUPT_STATUS GMU_WORD(0x9305)
#define GMU_AO_GPU_CX_BUSY_STATUS GMU_WORD(0x930c)
#define GMU_AHB_FENCE_STATUS    GMU_WORD(0x9313)

static struct block gbif = { "gbif", 0x03d00000 + 4 * 0x3c00, 0x1000 };
#define GBIF_HALT               (4 * 0x45)
#define GBIF_HALT_ACK           (4 * 0x46)
static struct block cx_misc = { "cx_misc", 0x03d9e000, 0x1000 };

static bool
step_first_words(void)
{
	uint64_t start, nanos;
	uint32_t before, hz, ahb;

	if (!gpucc_answers()) {
		return false;
	}
	ahb = rd(gpucc.base + GPUCC_AHB_CBCR);
	if (!cx_is_on() || (rd(gpucc.base + GPUCC_CX_GMU_CBCR) & (CBCR_ENABLE | CBCR_OFF)) != CBCR_ENABLE) {
		printf("adreno: %u: needs CX on and the GMU's clock running: steps 3 and 4\n", step);
		return false;
	}
	printf("adreno: %u: the register bus clock's control says %#x\n", step, ahb);
	announce("read registers of the GMU (0x3d6a000): the first words from the GPU block itself");
	get(&gmu, "cm3_sysreset", GMU_CM3_SYSRESET);
	get(&gmu, "cm3_boot_config", GMU_CM3_BOOT_CONFIG);
	get(&gmu, "cm3_fw_init_result", GMU_CM3_FW_INIT_RESULT);
	get(&gmu, "cm3_cfg", GMU_CM3_CFG);
	get(&gmu, "hfi_ctrl_status", GMU_HFI_CTRL_STATUS);
	get(&gmu, "hfi_version_info", GMU_HFI_VERSION_INFO);
	get(&gmu, "rpmh_power_state", GMU_RPMH_POWER_STATE);
	get(&gmu, "ao_host_interrupt_status", GMU_AO_HOST_INTERRUPT_STATUS);
	get(&gmu, "ao_gpu_cx_busy_status", GMU_AO_GPU_CX_BUSY_STATUS);
	get(&gmu, "ahb_fence_status", GMU_AHB_FENCE_STATUS);
	get(&gmu, "always_on_counter_h", GMU_ALWAYS_ON_COUNTER_H);
	get(&gmu, "always_on_counter_l", GMU_ALWAYS_ON_COUNTER_L);
	start = mach_absolute_time();
	before = rd(gmu.base + GMU_ALWAYS_ON_COUNTER_L);
	sleep_ms(10);
	hz = rd(gmu.base + GMU_ALWAYS_ON_COUNTER_L) - before;
	absolutetime_to_nanoseconds(mach_absolute_time() - start, &nanos);
	hz = nanos ? (uint32_t)((uint64_t)hz * NSEC_PER_SEC / nanos) : 0;
	printf("adreno: %u: the GMU's counter counts at %u Hz: %s\n", step, hz, hz > 18000000 && hz < 20500000 ?
	    "the 19.2 MHz crystal: THE GPU BLOCK IS ALIVE" : "not the crystal's 19.2 MHz");
	announce("read the GPU's bus interface (0x3d0f000) and the CX odds and ends (0x3d9e000)");
	get(&gbif, "halt", GBIF_HALT);
	get(&gbif, "halt_ack", GBIF_HALT_ACK);
	get(&cx_misc, "word 0", 0);
	get(&cx_misc, "word 1", 4);
	return true;
}

/* ---- step 6: the GMU's channel to RPMh ----------------------------------------------------------------- */

/* Linux's sm8450.dtsi: gmu "rscc" at 0x3d50000 (the vendor's: kgsl-3d0 "rscc"); offsets in words, a6xx_gmu.xml */
static struct block rscc = { "rscc", 0x03d50000, 0x10000 };
#define RSCC_RSC_STATUS0_DRV0   (4 * 0x004)
#define RSCC_PDC_SLAVE_ID_DRV0  (4 * 0x00b)
#define RSCC_SEQ_BUSY_DRV0      (4 * 0x101)
#define RSCC_SEQ_MEM_0_DRV0     (4 * 0x180)
#define RSCC_TCS0_DRV0_STATUS   (4 * 0x346)
#define GMU_RSCC_CONTROL_ACK    GMU_WORD(0x9308)

static bool
cx_ready(void)
{
	if (!gpucc_answers()) {
		return false;
	}
	if (!cx_is_on() || (rd(gpucc.base + GPUCC_CX_GMU_CBCR) & (CBCR_ENABLE | CBCR_OFF)) != CBCR_ENABLE) {
		printf("adreno: %u: needs CX on and the GMU's clock running: steps 3 and 4\n", step);
		return false;
	}
	return true;
}

static bool
step_rscc(void)
{
	if (!cx_ready()) {
		return false;
	}
	announce("read the GMU's RPMh channel (0x3d50000)");
	get(&rscc, "rsc_status0_drv0", RSCC_RSC_STATUS0_DRV0);
	get(&rscc, "pdc_slave_id_drv0", RSCC_PDC_SLAVE_ID_DRV0);
	get(&rscc, "seq_busy_drv0", RSCC_SEQ_BUSY_DRV0);
	get(&rscc, "seq_mem_0_drv0", RSCC_SEQ_MEM_0_DRV0);
	get(&rscc, "tcs0_drv0_status", RSCC_TCS0_DRV0_STATUS);
	get(&gmu, "rscc_control_ack", GMU_RSCC_CONTROL_ACK);
	return true;
}

/* ---- steps 7 and 8: the SMMU --------------------------------------------------------------------------- */

/*
 * dt: kgsl-smmu@3da0000 (qcom,qsmmu-v500), 0x40000 long, "qcom,skip-init", 0x15 contexts and 0x18 stream match
 * registers by the device tree's overrides. Streams (Linux's sm8450.dtsi): 0 and 1 the 3D core (mask 0x400), 5 the
 * GMU. Offsets: arm-smmu.h.
 */
static uint32_t gpu_mhz_asked;                  /* the GPU's speed as asked of the GMU (fixed: no DCVS yet), for the HUD */
static struct block smmu = { "smmu", 0x03da0000, 0x40000 };
#define SMMU_SCR0               0x000
#define SMMU_IDR0               0x020
#define SMMU_IDR1               0x024
#define SMMU_IDR2               0x028
#define SMMU_IDR7               0x03c
#define SMMU_SGFSR              0x048
#define SMMU_SMR(n)             (0x800 + 4 * (n))
#define SMMU_S2CR(n)            (0xc00 + 4 * (n))
#define SMMU_CBAR(n)            (4 * (n))
#define SMMU_CB_SCTLR           0x00
#define SMMU_CB_TTBR0           0x20
#define SMMU_CB_TCR             0x30
#define SMMU_CB_FSR             0x58

static uint32_t smmu_page, smmu_pages, smmu_contexts, smmu_matches;

static bool
smmu_layout(void)
{
	uint32_t idr0 = get(&smmu, "idr0", SMMU_IDR0), idr1 = get(&smmu, "idr1", SMMU_IDR1);

	smmu_page = idr1 >> 31 ? 0x10000 : 0x1000;
	smmu_pages = 2u << (idr1 >> 28 & 7);
	smmu_contexts = idr1 & 0xff;
	smmu_matches = idr0 & 0xff;
	printf("adreno: %u: SMMU: %u stream match registers, %u contexts, %u pages of %#x before the contexts' own\n", step,
	    smmu_matches, smmu_contexts, smmu_pages, smmu_page);
	if (2 * smmu_pages * smmu_page != smmu.size) {
		printf("adreno: %u: that makes %#x of registers, the device tree says %#x: not reading further\n", step,
		    2 * smmu_pages * smmu_page, smmu.size);
		return false;
	}
	return true;
}

static bool
step_smmu(void)
{
	if (!cx_ready()) {
		return false;
	}
	announce("read the ID registers of the GPU's SMMU (0x3da0000): global registers, which the hypervisor answers for");
	if (!smmu_layout()) {
		return false;
	}
	get(&smmu, "idr2", SMMU_IDR2);
	get(&smmu, "idr7", SMMU_IDR7);
	get(&smmu, "scr0", SMMU_SCR0);
	get(&smmu, "sgfsr", SMMU_SGFSR);
	announce("read the SMMU's stream table and the kinds of its contexts");
	for (uint32_t i = 0; i < smmu_matches && i < 12; i += 3) {
		printf("adreno: %u: smr/s2cr %u: %#x/%#x %#x/%#x %#x/%#x\n", step, i, rd(smmu.base + SMMU_SMR(i)),
		    rd(smmu.base + SMMU_S2CR(i)), rd(smmu.base + SMMU_SMR(i + 1)), rd(smmu.base + SMMU_S2CR(i + 1)),
		    rd(smmu.base + SMMU_SMR(i + 2)), rd(smmu.base + SMMU_S2CR(i + 2)));
	}
	for (uint32_t i = 0; i < smmu_contexts && i < 12; i += 4) {
		printf("adreno: %u: cbar %u: %#x %#x %#x %#x\n", step, i, rd(smmu.base + smmu_page + SMMU_CBAR(i)),
		    rd(smmu.base + smmu_page + SMMU_CBAR(i + 1)), rd(smmu.base + smmu_page + SMMU_CBAR(i + 2)),
		    rd(smmu.base + smmu_page + SMMU_CBAR(i + 3)));
	}
	return true;
}

static bool
step_smmu_context(void)
{
	uint32_t context;

	if (!cx_ready()) {
		return false;
	}
	announce("read translation context 0 of the SMMU: registers of the hardware itself, not the hypervisor's");
	if (!smmu_layout()) {
		return false;
	}
	context = smmu_pages * smmu_page;
	get(&smmu, "cb0 sctlr", context + SMMU_CB_SCTLR);
	get(&smmu, "cb0 ttbr0, low", context + SMMU_CB_TTBR0);
	get(&smmu, "cb0 ttbr0, high", context + SMMU_CB_TTBR0 + 4);
	get(&smmu, "cb0 tcr", context + SMMU_CB_TCR);
	get(&smmu, "cb0 fsr", context + SMMU_CB_FSR);
	return true;
}

/* ---- memory the GPU block reaches (drivers/gpu/hakios.adreno.c, as it is) ------------------------------ */

#define PTE_TABLE               3ull
#define PTE_PAGE                3ull
#define PTE_CACHED              (1ull << 2)     /* memory attribute 1 of SMMU_MAIR0; without it 0, uncached */
#define PTE_UNPRIVILEGED        (1ull << 6)     /* the 3D core's accesses are; the GMU's are not */
#define PTE_SHARED              (2ull << 8)
#define PTE_ACCESSED            (1ull << 10)
#define PTE_NOT_GLOBAL          (1ull << 11)
#define PTE_ADDRESS             0x0000fffffffff000ull
#define PTE_GMU                 (PTE_SHARED | PTE_ACCESSED | PTE_NOT_GLOBAL)

#define SMMU_S2CR_FAULT         (2u << 16)
#define SMMU_SMR_VALID          (1u << 31)
#define SMMU_STREAM_MASK        0x400           /* dt: the second cell of every "iommus" of this SMMU */
#define SMMU_CBAR_STAGE_1       (1u << 16 | 3u << 8 | 0xfu << 12)       /* our tables alone decide; weakest defaults */
#define SMMU_CBA2R(n)           (0x800 + 4 * (n))                       /* in the second page, like CBAR */
#define SMMU_CBA2R_64_BIT       1
#define SMMU_CB_TCR2            0x10
#define SMMU_CB_TTBR1           0x28
#define SMMU_CB_MAIR0           0x38
#define SMMU_CB_MAIR1           0x3c
#define SMMU_CB_FAR             0x60
#define SMMU_CB_FSYNR0          0x68
#define SMMU_CB_TLBIASID        0x610
#define SMMU_CB_TLBSYNC         0x7f0
#define SMMU_CB_TLBSTATUS       0x7f4
#define SMMU_SCTLR_ON           (1u << 0 | 1u << 1 | 1u << 2 | 1u << 5 | 1u << 12) /* on; faults recorded, no interrupt */
#define SMMU_TCR_39_BITS        (25u | 1u << 8 | 1u << 10 | 3u << 12 | 1u << 23)   /* one table of 4K pages, cached walks */
#define SMMU_TCR2               (1u | 1u << 4 | 7u << 15)       /* 36 bits of physical address, 16 of context tag */
#define SMMU_MAIR0              0xf404ff44      /* Linux's: 0 uncached, 1 cached, 2 device */
#define SMMU_FSR_ALL            0xc00001fe      /* every kind of fault, and "several" */


/* Until every write to memory so far has been carried out. */
static void
settle(void)
{
	__asm__ volatile ("dsb sy");
}

/* Push what this processor has written to a stretch of memory out of its cache, and forget it: the GPU block reads memory. */
static void
to_memory(vm_offset_t start, vm_size_t size)
{
	for (vm_offset_t line = start & ~(vm_offset_t)63; line < start + size; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	settle();
}

#define SPACE_TABLES            1024            /* 2 GB of addresses, were they all in use */

struct gpu_space {
	const char *name;
	uint32_t context;               /* which of the SMMU's */
	uint32_t control;               /* bits of its control register besides the usual */
	uint32_t table_count;
	uint64_t *root;
	struct {
		uint64_t phys;
		uint64_t *table;
		bool changed;           /* since it was last written out of this processor's cache (space_commit()) */
	} tables[SPACE_TABLES];
	uint64_t *last_entry, *last_table;      /* the level 2 entry looked up last, and its table */
	uint32_t last_changed;                  /* the table an entry was written in last */
};

static uint64_t *
space_new_table(struct gpu_space *space)
{
	static vm_offset_t spare;
	static unsigned spare_left;
	uint64_t *table;

	if (space->table_count == SPACE_TABLES) {
		return NULL;
	}
	if (spare_left == 0) {
		if (kmem_alloc(kernel_map, &spare, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) !=
		    KERN_SUCCESS) {
			return NULL;
		}
		spare_left = PAGE_SIZE / 4096;
	}
	table = (uint64_t *)(spare + PAGE_SIZE - 4096 * spare_left--);
	space->tables[space->table_count].phys = kvtophys((vm_offset_t)table);
	space->tables[space->table_count++].table = table;
	return table;
}

/* The table an entry of a higher table leads to; made if the entry is empty. */
/* An entry of one of the tables has been written: that table is to go out to memory. */
static void
space_changed(struct gpu_space *space, const uint64_t *entry)
{
	if ((uintptr_t)entry - (uintptr_t)space->tables[space->last_changed].table < 4096) {
		space->tables[space->last_changed].changed = true;      /* page after page of one buffer */
		return;
	}
	for (uint32_t i = space->table_count; i-- > 0;) {
		if ((uintptr_t)entry - (uintptr_t)space->tables[i].table < 4096) {
			space->tables[i].changed = true;
			space->last_changed = i;
			return;
		}
	}
}

/* The SMMU reads the tables from memory, not from this processor's cache. */
static void
space_commit(struct gpu_space *space)
{
	for (uint32_t i = 0; i < space->table_count; i++) {
		if (space->tables[i].changed) {
			to_memory((vm_offset_t)space->tables[i].table, 4096);
			space->tables[i].changed = false;
		}
	}
}

static uint64_t *
space_next(struct gpu_space *space, uint64_t *entry)
{
	uint64_t *table;

	if (*entry & 1) {
		for (uint32_t i = 0; i < space->table_count; i++) {
			if (space->tables[i].phys == (*entry & PTE_ADDRESS)) {
				return space->tables[i].table;
			}
		}
		return NULL;
	}
	table = space_new_table(space);
	if (table != NULL) {
		*entry = kvtophys((vm_offset_t)table) | PTE_TABLE;
		space_changed(space, entry);
	}
	return table;
}

/* One 4K page. */
static bool
space_map(struct gpu_space *space, uint64_t address, uint64_t phys, uint64_t attributes)
{
	uint64_t *level2, *level3;

	if (address >> 39 || (space->root == NULL && (space->root = space_new_table(space)) == NULL)) {
		return false;
	}
	level2 = space_next(space, &space->root[address >> 30 & 0x1ff]);
	if (level2 == NULL) {
		return false;
	}
	if (space->last_entry == &level2[address >> 21 & 0x1ff]) {
		level3 = space->last_table;             /* page after page of one buffer: the same table */
	} else {
		level3 = space_next(space, &level2[address >> 21 & 0x1ff]);
		space->last_entry = &level2[address >> 21 & 0x1ff];
		space->last_table = level3;
	}
	if (level3 == NULL) {
		space->last_entry = NULL;
		return false;
	}
	level3[address >> 12 & 0x1ff] = phys == 0 && attributes == 0 ? 0 : phys | attributes | PTE_PAGE;
	space_changed(space, level3);
	return true;
}

/* Memory of ours that the GPU block sees at an address of its own. */
struct gpu_block {
	uint64_t address;               /* the GPU's */
	uint32_t size;
	vm_offset_t memory;             /* ours */
};

static bool
block_new(struct gpu_space *space, struct gpu_block *block, uint64_t address, uint32_t size, uint64_t attributes)
{
	block->address = address;
	block->size = (size + 4095) & ~4095u;
	if (kmem_alloc(kernel_map, &block->memory, round_page(block->size), KMA_KOBJECT | KMA_ZERO | KMA_DATA,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		return false;
	}
	for (uint32_t offset = 0; offset < block->size; offset += 4096) {
		if (!space_map(space, address + offset, kvtophys(block->memory + offset), attributes)) {
			return false;
		}
	}
	to_memory(block->memory, block->size);
	printf("adreno: %u: %s sees %#llx..%#llx in memory of ours (%#llx)\n", step, space->name, address,
	    address + block->size - 1, kvtophys(block->memory));
	return true;
}

/*
 * A second view of a block without this processor's cache in between, for memory that both
 * sides write a word at a time. Only blocks of one page of ours: those are in one piece.
 */
static vm_offset_t
block_uncached(const struct gpu_block *block)
{
	uint64_t phys = kvtophys(block->memory);

	for (uint32_t offset = 0; offset < block->size; offset += 4096) {
		if (kvtophys(block->memory + offset) != phys + offset) {
			return 0;
		}
	}
	return ml_io_map_wcomb(phys, block->size);
}

static uint32_t
smmu_context(const struct gpu_space *space)
{
	return (smmu_pages + space->context) * smmu_page;
}

/* What a context has recorded of a request it could not serve. */
static bool
space_faulted(const struct gpu_space *space)
{
	uint32_t context = smmu_context(space), status = rd(smmu.base + context + SMMU_CB_FSR);

	if ((status & SMMU_FSR_ALL) == 0) {             /* bits 10:9 only tell the format of the syndrome */
		return false;
	}
	printf("adreno: %u: THE SMMU REFUSED %s: status %#x, address %#llx, syndrome %#x\n", step, space->name, status,
	    (uint64_t)rd(smmu.base + context + SMMU_CB_FAR + 4) << 32 | rd(smmu.base + context + SMMU_CB_FAR),
	    rd(smmu.base + context + SMMU_CB_FSYNR0));
	return true;
}

/*
 * One more stream through a context that is on. A match is taken only as the device tree has
 * it, a stream and the mask 0x400: one entry for streams 0 and 1 (mask 0x401) reads back 0.
 */
static bool
space_stream(struct gpu_space *space, uint32_t match, uint32_t stream)
{
	put(&smmu, "s2cr", SMMU_S2CR(match), space->context);
	put(&smmu, "smr", SMMU_SMR(match), SMMU_SMR_VALID | SMMU_STREAM_MASK << 16 | stream);
	if (get(&smmu, "smr", SMMU_SMR(match)) != (SMMU_SMR_VALID | SMMU_STREAM_MASK << 16 | stream) ||
	    (get(&smmu, "s2cr", SMMU_S2CR(match)) & 0x300ff) != space->context) {
		printf("adreno: %u: the SMMU did not take what was written: stream %u is not going through context %u\n", step,
		    stream, space->context);
		return false;
	}
	return true;
}

/* Give a context our tables and send a stream through it; `match` is the entry of the stream table to use. */
static bool
space_on(struct gpu_space *space, uint32_t match, uint32_t stream)
{
	uint32_t context = smmu_context(space);
	uint64_t root = space->tables[0].phys | (uint64_t)space->context << 48;        /* tagged with the context's number */

	for (uint32_t i = 0; i < space->table_count; i++) {
		to_memory((vm_offset_t)space->tables[i].table, 4096);
	}
	put(&smmu, "cb sctlr", context + SMMU_CB_SCTLR, 0);
	put(&smmu, "cba2r", smmu_page + SMMU_CBA2R(space->context), SMMU_CBA2R_64_BIT);
	put(&smmu, "cbar", smmu_page + SMMU_CBAR(space->context), SMMU_CBAR_STAGE_1);
	put(&smmu, "cb tcr2", context + SMMU_CB_TCR2, SMMU_TCR2);
	put(&smmu, "cb tcr", context + SMMU_CB_TCR, SMMU_TCR_39_BITS);
	put(&smmu, "cb ttbr0, low", context + SMMU_CB_TTBR0, (uint32_t)root);
	put(&smmu, "cb ttbr0, high", context + SMMU_CB_TTBR0 + 4, (uint32_t)(root >> 32));
	put(&smmu, "cb ttbr1, low", context + SMMU_CB_TTBR1, 0);
	put(&smmu, "cb ttbr1, high", context + SMMU_CB_TTBR1 + 4, 0);
	put(&smmu, "cb mair0", context + SMMU_CB_MAIR0, SMMU_MAIR0);
	put(&smmu, "cb mair1", context + SMMU_CB_MAIR1, 0);
	put(&smmu, "cb fsr", context + SMMU_CB_FSR, SMMU_FSR_ALL);
	put(&smmu, "cb tlbiasid", context + SMMU_CB_TLBIASID, space->context);
	put(&smmu, "cb tlbsync", context + SMMU_CB_TLBSYNC, 1);
	if (!wait_for(&smmu, context + SMMU_CB_TLBSTATUS, 1, 0, 100)) {
		printf("adreno: %u: the SMMU does not finish forgetting old translations (%#x)\n", step,
		    rd(smmu.base + context + SMMU_CB_TLBSTATUS));
		return false;
	}
	put(&smmu, "cb sctlr", context + SMMU_CB_SCTLR, SMMU_SCTLR_ON | space->control);
	put(&smmu, "s2cr", SMMU_S2CR(match), space->context);
	put(&smmu, "smr", SMMU_SMR(match), SMMU_SMR_VALID | SMMU_STREAM_MASK << 16 | stream);

	/* the global ones are the hypervisor's to grant: see what it made of them */
	if (get(&smmu, "smr", SMMU_SMR(match)) != (SMMU_SMR_VALID | SMMU_STREAM_MASK << 16 | stream) ||
	    (get(&smmu, "s2cr", SMMU_S2CR(match)) & 0x300ff) != space->context ||
	    (get(&smmu, "cbar", smmu_page + SMMU_CBAR(space->context)) & 0x30000) != (SMMU_CBAR_STAGE_1 & 0x30000) ||
	    !(get(&smmu, "cb sctlr", context + SMMU_CB_SCTLR) & 1) ||
	    get(&smmu, "cb ttbr0, low", context + SMMU_CB_TTBR0) != (uint32_t)root ||
	    get(&smmu, "cb ttbr0, high", context + SMMU_CB_TTBR0 + 4) != (uint32_t)(root >> 32)) {
		printf("adreno: %u: the SMMU did not take what was written: stream %u is not going through context %u\n", step,
		    stream, space->context);
		return false;
	}
	get(&smmu, "cb tcr", context + SMMU_CB_TCR);
	get(&smmu, "cba2r", smmu_page + SMMU_CBA2R(space->context));
	get(&smmu, "cb tcr2", context + SMMU_CB_TCR2);
	get(&smmu, "cb fsr", context + SMMU_CB_FSR);
	get(&smmu, "sgfsr", SMMU_SGFSR);
	return true;
}

/* A stream goes nowhere again, and its context translates nothing. */
static void
space_off(struct gpu_space *space, uint32_t match)
{
	put(&smmu, "smr", SMMU_SMR(match), 0);
	put(&smmu, "s2cr", SMMU_S2CR(match), SMMU_S2CR_FAULT);
	put(&smmu, "cb sctlr", smmu_context(space) + SMMU_CB_SCTLR, 0);
}

/* ---- step 9: the GMU's memory -------------------------------------------------------------------------- */

#define GMU_DTCM                GMU_WORD(0x1c00)        /* the processor's data memory (0x10004000 to the firmware) */

static bool
step_gmu_memory(void)
{
	static const uint32_t pattern[2] = { 0x600d0001, 0x0a110002 };
	uint32_t was[2], back[2];

	if (!cx_ready()) {
		return false;
	}
	announce("hold the GMU's processor in reset, write two words into its data memory, read them back");
	if (get(&gmu, "cm3_fw_init_result", GMU_CM3_FW_INIT_RESULT) == 0xbabeface) {
		printf("adreno: %u: firmware is running in the GMU: leaving its memory alone\n", step);
		return false;
	}
	put(&gmu, "cm3_sysreset", GMU_CM3_SYSRESET, 1);
	for (int i = 0; i < 2; i++) {
		was[i] = get(&gmu, "dtcm", GMU_DTCM + 4 * i);
	}
	for (int i = 0; i < 2; i++) {
		put(&gmu, "dtcm", GMU_DTCM + 4 * i, pattern[i]);
	}
	for (int i = 0; i < 2; i++) {
		back[i] = get(&gmu, "dtcm", GMU_DTCM + 4 * i);
	}
	for (int i = 0; i < 2; i++) {
		put(&gmu, "dtcm", GMU_DTCM + 4 * i, was[i]);
	}
	printf("adreno: %u: the GMU's memory %s\n", step, back[0] == pattern[0] && back[1] == pattern[1] ?
	    "TAKES WHAT IS WRITTEN: its firmware can be loaded" : "does not hold what was written");
	return back[0] == pattern[0] && back[1] == pattern[1];
}

/* ---- step 10: TrustZone ---------------------------------------------------------------------------------- */

/*
 * The 3D core comes out of reset in secure mode and only TrustZone lets it out, when it is handed the vendor's
 * signed zap shader (image 13). Questions that pass no memory first: whether TrustZone listens to this kernel at
 * all. Convention: qcom_scm-smc.c; the same calls drivers/gpu/hakios.adreno.c makes on redwood.
 */
#define SCM_FAST                (1ull << 31)
#define SCM_64                  (1ull << 30)
#define SCM_SIP                 (2ull << 24)
#define SCM_CALL(service, command) ((uint64_t)(service) << 8 | (command))
#define SCM_IS_CALL_AVAILABLE   SCM_CALL(6, 1)
#define SCM_PAS_INIT_IMAGE      SCM_CALL(2, 1)
#define SCM_PAS_AUTH_AND_RESET  SCM_CALL(2, 5)
#define SCM_PAS_IS_SUPPORTED    SCM_CALL(2, 7)
#define SCM_ONE_VALUE           1
#define SCM_VALUE_AND_BUFFER    (2 | 2 << 6)
#define SCM_INTERRUPTED         1
#define PAS_GPU                 13

static int32_t
scm(const char *what, uint64_t function, uint64_t arguments, uint64_t first, uint64_t second, uint64_t third,
    uint64_t *answer)
{
	uint64_t session = 0, ctl_before, cval_before, ctl_after, cval_after;
	int cpu_before = cpu_number();

	printf("adreno: %u: ask TrustZone %s: smc %#llx (%#llx, %#llx)\n", step, what, function, first, second);
	hold(true);
	ctl_before = __builtin_arm_rsr64("CNTV_CTL_EL0");
	cval_before = __builtin_arm_rsr64("CNTV_CVAL_EL0");
	for (int i = 0; i < 100; i++) {
		register uint64_t x0 __asm__("x0") = i == 0 ? function : SCM_INTERRUPTED;
		register uint64_t x1 __asm__("x1") = arguments;
		register uint64_t x2 __asm__("x2") = first;
		register uint64_t x3 __asm__("x3") = second;
		register uint64_t x4 __asm__("x4") = third;
		register uint64_t x5 __asm__("x5") = 0;
		register uint64_t x6 __asm__("x6") = session;
		register uint64_t x7 __asm__("x7") = 0;

		__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3), "+r" (x4), "+r" (x5), "+r" (x6),
		    "+r" (x7) : : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
		session = x6;
		if (x0 != SCM_INTERRUPTED) {
			/*
			 * Seen: after the call that authenticates the zap shader, a sleeping thread of ours never woke. The
			 * kernel's timer is the virtual one: what the call left of it is shown, and the CPU's deadline set again.
			 */
			ctl_after = __builtin_arm_rsr64("CNTV_CTL_EL0");
			cval_after = __builtin_arm_rsr64("CNTV_CVAL_EL0");
			if (ctl_after != ctl_before || cval_after != cval_before || cpu_number() != cpu_before) {
				printf("adreno: %u: the timer: control %#llx -> %#llx, compare %#llx -> %#llx, now %#llx; cpu %d -> %d\n",
				    step, ctl_before, ctl_after, cval_before, cval_after, __builtin_arm_rsr64("CNTVCT_EL0"), cpu_before,
				    cpu_number());
			}
			timer_resync_deadlines();
			printf("adreno: %u: TrustZone returns %d, answer %#llx\n", step, (int32_t)x0, x1);
			*answer = x1;
			return (int32_t)x0;
		}
	}
	printf("adreno: %u: TrustZone keeps saying the call was interrupted\n", step);
	return -1;
}

static uint64_t scm_how = SCM_FAST | SCM_64 | SCM_SIP;

static bool
step_trustzone(void)
{
	uint64_t answer = 0;
	uint32_t fast, slow;

	announce("ask TrustZone what Linux asks it first: is the call \"is this call available\" available");
	if (scm("whether it can be asked", scm_how | SCM_IS_CALL_AVAILABLE, SCM_ONE_VALUE, SCM_SIP | SCM_IS_CALL_AVAILABLE,
	    0, 0, &answer) != 0 || answer != 1) {
		scm_how = SCM_FAST | SCM_SIP;
		if (scm("the same, the 32-bit way", scm_how | SCM_IS_CALL_AVAILABLE, SCM_ONE_VALUE,
		    SCM_SIP | SCM_IS_CALL_AVAILABLE, 0, 0, &answer) != 0 || answer != 1) {
			printf("adreno: %u: TrustZone does not answer the way it answers Linux\n", step);
			return false;
		}
	}
	announce("ask TrustZone whether it has the calls that authenticate a firmware image, and whether it lists the GPU's");
	scm("for the call that takes an image's signature", scm_how | SCM_IS_CALL_AVAILABLE, SCM_ONE_VALUE,
	    SCM_SIP | SCM_PAS_INIT_IMAGE, 0, 0, &answer);
	scm("for the call that authenticates and starts an image", scm_how | SCM_IS_CALL_AVAILABLE, SCM_ONE_VALUE,
	    SCM_SIP | SCM_PAS_AUTH_AND_RESET, 0, 0, &answer);
	answer = 0;
	fast = scm("whether it supports image 13 (the GPU)", scm_how | SCM_PAS_IS_SUPPORTED, SCM_ONE_VALUE, PAS_GPU, 0, 0,
	    &answer) == 0 && answer;
	answer = 0;
	slow = scm("the same, preemptible", (scm_how & ~SCM_FAST) | SCM_PAS_IS_SUPPORTED, SCM_ONE_VALUE, PAS_GPU, 0, 0,
	    &answer) == 0 && answer;
	printf("adreno: %u: the GPU's image \"supported\": %u at once, %u as Linux asks\n", step, fast, slow);
	return true;
}

/* ---- step 11: the zap shader's signature ----------------------------------------------------------------- */

extern const uint8_t *hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);


/* A file the userland build put into fw/ of the small image (from ../fw-gpu: the phone's own, never in the repository). */
static const uint8_t *
image_file(const char *name, uint64_t *size)
{
	int disk = -1;
	const uint8_t *data = NULL;

	/* the steps start early: the RAM disks may not be there yet (seen: the file missing when no step waited) */
	for (int i = 0; i < 200 && (data = hakios_image_file("fw", name, size, &disk)) == NULL; i++) {
		sleep_ms(100);
	}

	if (data != NULL) {
		printf("adreno: %u: fw/%s: %llu bytes in the image of /dev/md%d\n", step, name, *size, disk);
	} else {
		printf("adreno: %u: no fw/%s in the small image: the phone's /vendor/firmware/%s goes into fw-gpu/\n", step, name,
		    name);
	}
	return data;
}

/*
 * The first of the three calls with which Linux has a zap shader loaded (qcom_mdt_pas_init): TrustZone is given the
 * header (the ELF header and the block of hashes, all of a730_zap.mdt) and checks the signature. Nothing of the GPU
 * is involved. The page is TrustZone's to protect while it works: never touched again, never freed.
 */
static bool
step_zap_signature(void)
{
	const uint8_t *header;
	vm_offset_t page = 0;
	uint64_t size = 0, answer = ~0ull;
	uint32_t segments, hashes, header_size;
	int32_t result;

	announce("find the zap shader's header, fw/a730_zap.mdt, in the small image that came with the kernel");
	header = image_file("a730_zap.mdt", &size);
	if (header == NULL) {
		return false;
	}
	/*
	 * qcom_mdt_read_metadata(): what TrustZone takes is the ELF header with its table (segment 0's size) followed by
	 * the hash segment (the one whose flags say type 2). In a730_zap.mdt that is the whole file: the hashes are packed
	 * right after the header (the third segment, not the second as in redwood's a660_zap.mdt).
	 */
	segments = size >= 52 ? header[44] | header[45] << 8 : 0;
	if (memcmp(header, "\177ELF\1", 5) != 0 || segments < 2 || 52 + 32 * (uint64_t)segments > size || size > PAGE_SIZE) {
		printf("adreno: %u: that is not such a header (%02x %02x %02x %02x, %u segments)\n", step, header[0], header[1],
		    header[2], header[3], segments);
		return false;
	}
	memcpy(&header_size, header + 52 + 16, sizeof(header_size));
	hashes = 0;
	for (uint32_t i = 0; i < segments; i++) {
		uint32_t flags, file_size;

		memcpy(&flags, header + 52 + 32 * i + 24, sizeof(flags));
		memcpy(&file_size, header + 52 + 32 * i + 16, sizeof(file_size));
		if ((flags >> 24 & 7) == 2) {
			hashes = file_size;
			printf("adreno: %u: segment %u holds the hashes: %u bytes\n", step, i, hashes);
		}
	}
	printf("adreno: %u: an ELF header of %u segments, %u bytes with its table; the file is %llu bytes\n", step, segments,
	    header_size, size);
	if (hashes == 0 || (uint64_t)header_size + hashes != size) {
		printf("adreno: %u: the file is not the header followed by the hashes alone: not what TrustZone takes\n", step);
		return false;
	}
	if (kmem_alloc(kernel_map, &page, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("adreno: %u: no memory\n", step);
		return false;
	}
	memcpy((void *)page, header, size);
	to_memory(page, PAGE_SIZE);
	announce("hand the header to TrustZone to check its signature (image 13)");
	result = scm("to take the zap shader's signature", SCM_64 | SCM_SIP | SCM_PAS_INIT_IMAGE, SCM_VALUE_AND_BUFFER, PAS_GPU,
	    kvtophys(page), 0, &answer);
	if (result == 0 && answer == 0) {
		printf("adreno: %u: TRUSTZONE ACCEPTS THE ZAP SHADER'S SIGNATURE FROM THIS KERNEL\n", step);
		return true;
	}
	printf("adreno: %u: TrustZone does not take it: the call returns %d, its answer is %#llx\n", step, result, answer);
	return false;
}

/* ---- step 12: memory for the GMU, and the SMMU's context for it ------------------------------------------ */

/*
 * The GMU is a Cortex-M3 with 16K of code and 16K of data memory of its own (step 9); the rest it reaches through the
 * SMMU at addresses its firmware names. gmu_gen70000.bin starts with a table of what it needs (type 1 or 7, no data:
 * "N bytes at A") and then the blocks of data, of which only the one at 0x4000 (56K of code, the "icache" region)
 * goes into memory, the rest into the two TCMs. Besides those, as Linux's a6xx_gmu_memory_probe() has them for the
 * 7xx: 28K of "debug" at 0x60400000, and the log and the message page in the uncached range after the dummy pages.
 */
#define GMU_ITCM_ADDRESS        0x00000000u
#define GMU_DTCM_ADDRESS        0x10004000u
#define GMU_TCM_SIZE            0x4000u
#define GMU_LOG_ADDRESS         0x60002000u
#define GMU_LOG_SIZE            0x4000u
#define GMU_HFI_ADDRESS         0x60006000u
#define GMU_DEBUG_ADDRESS       0x60400000u
#define GMU_DEBUG_SIZE          0x7000u
#define GMU_STREAM              5
#define GMU_MATCH               1
#define GMU_ITCM                GMU_WORD(0x0c00)
#define GMU_DTCM_VERSIONS       GMU_WORD(0x1c00 + 0xff8)

struct gmu_firmware_block {
	uint32_t address, size, type, value;
};
#define GMU_FIRMWARE_NEEDS      1
#define GMU_FIRMWARE_NEEDS_KEPT 7

static struct gpu_space gmu_space = { .name = "the GMU", .context = 1 };
static struct gpu_block gmu_blocks[12], gmu_log, gmu_hfi, gmu_debug;
static unsigned gmu_block_count;
static const uint8_t *gmu_firmware;
static uint64_t gmu_firmware_size;
static bool gmu_space_is_on;

static bool
in_tcm(uint32_t address)
{
	return address - GMU_ITCM_ADDRESS < GMU_TCM_SIZE || address - GMU_DTCM_ADDRESS < GMU_TCM_SIZE;
}

static bool
step_gmu_space(void)
{
	if (!cx_ready()) {
		return false;
	}
	announce("find the GMU's firmware, fw/gmu_gen70000.bin, in the small image, and make the memory it asks for");
	gmu_firmware = image_file("gmu_gen70000.bin", &gmu_firmware_size);
	if (gmu_firmware == NULL) {
		return false;
	}
	for (uint64_t at = 0; at + sizeof(struct gmu_firmware_block) <= gmu_firmware_size;) {
		struct gmu_firmware_block block;

		memcpy(&block, gmu_firmware + at, sizeof(block));
		at += sizeof(block) + block.size;
		if (at > gmu_firmware_size) {
			printf("adreno: %u: the file ends inside a block\n", step);
			return false;
		}
		if (block.size != 0 || (block.type != GMU_FIRMWARE_NEEDS && block.type != GMU_FIRMWARE_NEEDS_KEPT) ||
		    in_tcm(block.address)) {
			continue;
		}
		if (gmu_block_count == sizeof(gmu_blocks) / sizeof(gmu_blocks[0]) || (block.address & 4095) ||
		    block.value > 0x100000 || !block_new(&gmu_space, &gmu_blocks[gmu_block_count++], block.address, block.value,
		    PTE_GMU)) {
			printf("adreno: %u: cannot make %#x bytes at %#x for the firmware\n", step, block.value, block.address);
			return false;
		}
	}
	if (!block_new(&gmu_space, &gmu_log, GMU_LOG_ADDRESS, GMU_LOG_SIZE, PTE_GMU) ||
	    !block_new(&gmu_space, &gmu_hfi, GMU_HFI_ADDRESS, GMU_LOG_SIZE, PTE_GMU) ||
	    !block_new(&gmu_space, &gmu_debug, GMU_DEBUG_ADDRESS, GMU_DEBUG_SIZE, PTE_GMU)) {
		return false;
	}
	announce("give the SMMU's context 1 these tables and send the GMU's requests (stream 5) through it: the first "
	    "writes to the SMMU, the global ones through the hypervisor");
	if (!smmu_layout()) {
		return false;
	}
	gmu_space_is_on = true;
	return space_on(&gmu_space, GMU_MATCH, GMU_STREAM);
}

/* ---- step 13: the GMU's firmware ----------------------------------------------------------------------- */

static bool
step_gmu_firmware(void)
{
	uint32_t wrong = 0;

	if (!cx_ready() || gmu_firmware == NULL || !gmu_space_is_on) {
		return false;
	}
	announce("load the GMU's firmware: what goes into its own code and data memory through its registers, the rest "
	    "into the memory of step 12");
	put(&gmu, "cm3_sysreset", GMU_CM3_SYSRESET, 1);
	for (int pass = 0; pass < 2; pass++) {
		for (uint64_t at = 0; at + sizeof(struct gmu_firmware_block) <= gmu_firmware_size;) {
			struct gmu_firmware_block block;
			const uint8_t *data = gmu_firmware + at + sizeof(block);

			memcpy(&block, gmu_firmware + at, sizeof(block));
			at += sizeof(block) + block.size;
			if (block.size == 0) {
				continue;
			}
			if (in_tcm(block.address) && (uint64_t)block.address + block.size <= 0x100000000ull &&
			    in_tcm(block.address + block.size - 1)) {
				uint32_t registers = block.address < GMU_DTCM_ADDRESS ? GMU_ITCM + block.address - GMU_ITCM_ADDRESS :
				    GMU_DTCM + block.address - GMU_DTCM_ADDRESS;

				for (uint32_t offset = 0; offset + 4 <= block.size; offset += 4) {
					uint32_t word;

					memcpy(&word, data + offset, sizeof(word));
					if (pass == 0) {
						wr(gmu.base + registers + offset, word);
					} else if (rd(gmu.base + registers + offset) != word) {
						wrong++;
					}
				}
				if (pass == 1) {
					printf("adreno: %u: %#x bytes at the GMU's %#x, through its registers\n", step, block.size,
					    block.address);
				}
				continue;
			}
			for (unsigned i = 0; pass == 0 && i <= gmu_block_count; i++) {
				if (i == gmu_block_count) {
					printf("adreno: %u: the firmware has %#x bytes for %#x, where it asked for no memory\n", step,
					    block.size, block.address);
					return false;
				}
				if (block.address >= gmu_blocks[i].address &&
				    (uint64_t)block.address + block.size <= gmu_blocks[i].address + gmu_blocks[i].size) {
					memcpy((void *)(gmu_blocks[i].memory + (block.address - gmu_blocks[i].address)), data, block.size);
					to_memory(gmu_blocks[i].memory, gmu_blocks[i].size);
					printf("adreno: %u: %#x bytes at the GMU's %#x, in memory\n", step, block.size, block.address);
					break;
				}
			}
		}
	}
	snprintf(last_write, sizeof(last_write), "step %u wrote the GMU's firmware", step);
	if (wrong) {
		printf("adreno: %u: %u words of the GMU's own memory do not read back as written\n", step, wrong);
		return false;
	}
	printf("adreno: %u: THE GMU'S FIRMWARE IS LOADED; versions read back: core %#x, power %#x, HFI %#x\n", step,
	    rd(gmu.base + GMU_DTCM_VERSIONS), rd(gmu.base + GMU_DTCM_VERSIONS + 8), rd(gmu.base + GMU_DTCM_VERSIONS + 16));
	return true;
}

/* ---- step 14: start the GMU ---------------------------------------------------------------------------- */

#define GMU_ICACHE_CONFIG       GMU_WORD(0x4c00)
#define GMU_DCACHE_CONFIG       GMU_WORD(0x4c01)
#define GMU_SYS_BUS_CONFIG      GMU_WORD(0x4c0f)
#define GMU_CX_FAL_INTF         GMU_WORD(0x50f0)
#define GMU_CX_FALNEXT_INTF     GMU_WORD(0x50f1)
#define GMU_HFI_QTBL_INFO       GMU_WORD(0x5184)
#define GMU_HFI_QTBL_ADDR       GMU_WORD(0x5185)
#define GMU_HFI_CTRL_INIT       GMU_WORD(0x5186)
#define GMU_GMU2HOST_INTR_CLR   GMU_WORD(0x5191)
#define GMU_GMU2HOST_INTR_INFO  GMU_WORD(0x5192)
#define GMU_HOST2GMU_INTR_SET   GMU_WORD(0x5194)
#define GMU_GENERAL_8           GMU_WORD(0x51cd)        /* 7xx: where the log is, and its pages - 1 */
#define GMU_GENERAL_9           GMU_WORD(0x51ce)        /* 7xx: how far the log was written */
#define GMU_GENERAL_10          GMU_WORD(0x51cf)        /* 7xx: which GPU this is */
#define GMU_RSCC_CONTROL_REQ    GMU_WORD(0x9307)
#define GMU_AHB_FENCE_RANGE_0   GMU_WORD(0x9311)
#define CX_MISC_TCM_RET_CNTL    (4 * 0x39)
static struct block pdc = { "pdc", 0x0b290000, 0x10000 };       /* dt: qcom,gmu "gmu_pdc" */
#define PDC_GPU_ENABLE          (4 * 0x1140)
#define PDC_GPU_SEQ_START_ADDR  (4 * 0x1148)
#define RSCC_PDC_SEQ_START_ADDR (4 * 0x008)
#define RSCC_PDC_MATCH_VALUE_LO (4 * 0x009)
#define RSCC_PDC_MATCH_VALUE_HI (4 * 0x00a)
#define RSCC_HIDDEN_TCS_CMD0_ADDR (4 * 0x00d)
#define RSCC_HIDDEN_TCS_CMD0_DATA (4 * 0x00e)
#define RSCC_OVERRIDE_START_ADDR (4 * 0x100)
#define GMU_CHIP                0x07030200      /* dt qcom,chipid 0x07030002 the way the GMU wants it (a6xx_gmu_fw_start) */

struct hfi_queue {
	uint32_t status, start, type, words, message_words, unused[5], read, write;
};
struct hfi_table {
	uint32_t version, words, first_queue, queue_words, queues, active_queues;
	struct hfi_queue queue[17];
};
#define HFI_QUEUES              3
#define HFI_QUEUE_WORDS         1024
#define HFI_QUEUE_AT(n)         (((uint32_t)sizeof(struct hfi_table) + 15) / 16 * 16 + 4096 * (n))

static volatile struct hfi_table *hfi;
static bool gmu_runs;

static void
gmu_post_mortem(void)
{
	printf("adreno: %u: the GMU: init result %#x, interrupt info %#x, ao status %#x, hfi status %#x, sysreset %#x\n", step,
	    rd(gmu.base + GMU_CM3_FW_INIT_RESULT), rd(gmu.base + GMU_GMU2HOST_INTR_INFO),
	    rd(gmu.base + GMU_AO_HOST_INTERRUPT_STATUS), rd(gmu.base + GMU_HFI_CTRL_STATUS), rd(gmu.base + GMU_CM3_SYSRESET));
	printf("adreno: %u: its log starts %#x %#x %#x %#x\n", step, ((volatile uint32_t *)gmu_log.memory)[0],
	    ((volatile uint32_t *)gmu_log.memory)[1], ((volatile uint32_t *)gmu_log.memory)[2],
	    ((volatile uint32_t *)gmu_log.memory)[3]);
}

/* a6xx_gmu_fw_start() and what it calls, for the 7xx, in its order */
static bool
step_gmu_start(void)
{
	static const uint8_t priority[HFI_QUEUES] = { 10, 10, 40 };
	uint32_t result = 0, mask, started;

	if (!cx_ready() || !gmu_space_is_on) {
		return false;
	}
	hfi = (volatile struct hfi_table *)block_uncached(&gmu_hfi);
	if (hfi == NULL) {
		printf("adreno: %u: the page for the messages is not in one piece\n", step);
		return false;
	}
	hfi->words = sizeof(struct hfi_table) / 4;
	hfi->first_queue = 6;
	hfi->queue_words = sizeof(struct hfi_queue) / 4;
	hfi->queues = hfi->active_queues = HFI_QUEUES;
	for (unsigned i = 0; i < HFI_QUEUES; i++) {
		hfi->queue[i].start = GMU_HFI_ADDRESS + HFI_QUEUE_AT(i);
		hfi->queue[i].type = i | (uint32_t)priority[i] << 8;
		hfi->queue[i].words = HFI_QUEUE_WORDS;
		hfi->queue[i].status = 1;
	}

	announce("wake the GMU's RPMh channel, then tell the GMU where things are (the message table, the log, which GPU "
	    "this is) and let its processor out of reset");
	if (get(&gmu, "cm3_sysreset", GMU_CM3_SYSRESET) != 1) {
		printf("adreno: %u: the GMU's processor is not held in reset: not configuring it while it runs\n", step);
		return false;
	}
	/* a6xx_gmu_rpmh_init(), which Linux does once at probe: the PDC is the always-on processor's, two words of it ours */
	put(&rscc, "rsc_status0_drv0", RSCC_RSC_STATUS0_DRV0, 1u << 24);
	put(&rscc, "pdc_slave_id_drv0", RSCC_PDC_SLAVE_ID_DRV0, 1);
	put(&rscc, "hidden_tcs_cmd0_data", RSCC_HIDDEN_TCS_CMD0_DATA, 0);
	put(&rscc, "hidden_tcs_cmd0_addr", RSCC_HIDDEN_TCS_CMD0_ADDR, 0);
	put(&rscc, "hidden_tcs_cmd1_data", RSCC_HIDDEN_TCS_CMD0_DATA + 4 * 2, 0);
	put(&rscc, "hidden_tcs_cmd1_addr", RSCC_HIDDEN_TCS_CMD0_ADDR + 4 * 2, 0);
	put(&rscc, "hidden_tcs_cmd2_data", RSCC_HIDDEN_TCS_CMD0_DATA + 4 * 4, 0x80000000);     /* the 730's; 0x80000021 is the 740's */
	put(&rscc, "hidden_tcs_cmd2_addr", RSCC_HIDDEN_TCS_CMD0_ADDR + 4 * 4, 0);
	put(&rscc, "override_start_addr", RSCC_OVERRIDE_START_ADDR, 0);
	put(&rscc, "pdc_seq_start_addr", RSCC_PDC_SEQ_START_ADDR, 0x4520);
	put(&rscc, "pdc_match_value_lo", RSCC_PDC_MATCH_VALUE_LO, 0x4510);
	put(&rscc, "pdc_match_value_hi", RSCC_PDC_MATCH_VALUE_HI, 0x4514);
	for (unsigned i = 0; i < 5; i++) {
		static const uint32_t program[5] = { 0xeaaae5a0, 0xe1a1ebab, 0xa2e0a581, 0xecac82e2, 0x0020edad };

		put(&rscc, "seq_mem_drv0", RSCC_SEQ_MEM_0_DRV0 + 4 * i, program[i]);
	}
	put(&pdc, "gpu_seq_start_addr", PDC_GPU_SEQ_START_ADDR, 0);
	put(&pdc, "gpu_enable", PDC_GPU_ENABLE, 0x80000001);
	put(&gmu, "cx_falnext_intf", GMU_CX_FALNEXT_INTF, 1);
	put(&gmu, "cx_fal_intf", GMU_CX_FAL_INTF, 1);
	put(&cx_misc, "tcm_ret_cntl", CX_MISC_TCM_RET_CNTL, 1);
	/*
	 * a6xx_rpmh_stop() at the end of a6xx_gmu_rpmh_init(), then a6xx_rpmh_start(): the channel is put to sleep first
	 * and then woken; a wake request to a channel that is not asleep goes unanswered (seen: ack 0, busy 0).
	 */
	printf("adreno: %u: rpmh channel before: status %#x, ack %#x, busy %#x\n", step, rd(rscc.base + RSCC_RSC_STATUS0_DRV0),
	    rd(gmu.base + GMU_RSCC_CONTROL_ACK), rd(rscc.base + RSCC_SEQ_BUSY_DRV0));
	put(&gmu, "rscc_control_req", GMU_RSCC_CONTROL_REQ, 1);
	if (!wait_for(&rscc, RSCC_RSC_STATUS0_DRV0, 1u << 16, 1u << 16, 100)) {
		printf("adreno: %u: the channel does not go to sleep (status %#x, ack %#x)\n", step,
		    rd(rscc.base + RSCC_RSC_STATUS0_DRV0), rd(gmu.base + GMU_RSCC_CONTROL_ACK));
	}
	put(&gmu, "rscc_control_req", GMU_RSCC_CONTROL_REQ, 0);
	put(&gmu, "rscc_control_req", GMU_RSCC_CONTROL_REQ, 2);
	if (!wait_for(&gmu, GMU_RSCC_CONTROL_ACK, 2, 2, 100) || !wait_for(&rscc, RSCC_SEQ_BUSY_DRV0, ~0u, 0, 100)) {
		printf("adreno: %u: the GMU's RPMh channel does not wake (status %#x, ack %#x, busy %#x): going on anyway\n", step,
		    rd(rscc.base + RSCC_RSC_STATUS0_DRV0), rd(gmu.base + GMU_RSCC_CONTROL_ACK), rd(rscc.base + RSCC_SEQ_BUSY_DRV0));
	}
	put(&gmu, "rscc_control_req", GMU_RSCC_CONTROL_REQ, 0);
	put(&gmu, "cm3_fw_init_result", GMU_CM3_FW_INIT_RESULT, 0);
	put(&gmu, "cm3_boot_config", GMU_CM3_BOOT_CONFIG, 2);
	put(&gmu, "hfi_qtbl_addr", GMU_HFI_QTBL_ADDR, GMU_HFI_ADDRESS);
	put(&gmu, "hfi_qtbl_info", GMU_HFI_QTBL_INFO, 1);
	put(&gmu, "ahb_fence_range_0", GMU_AHB_FENCE_RANGE_0, 1u << 31 | 0x32u << 18 | 0x8a0);
	put(&gmu, "cm3_cfg", GMU_CM3_CFG, 0x4052);
	put(&gmu, "general_10", GMU_GENERAL_10, GMU_CHIP);
	put(&gmu, "general_8", GMU_GENERAL_8, GMU_LOG_ADDRESS | (GMU_LOG_SIZE / 4096 - 1));
	put(&gmu, "sys_bus_config", GMU_SYS_BUS_CONFIG, 1);
	put(&gmu, "icache_config", GMU_ICACHE_CONFIG, 1);
	put(&gmu, "dcache_config", GMU_DCACHE_CONFIG, 1);
	put(&gmu, "gmu2host_intr_clr", GMU_GMU2HOST_INTR_CLR, 0xffffffff);
	if (rd(gmu.base + GMU_DTCM_VERSIONS) <= 0x20010004) {
		mask = 0xffffffff;
		started = 0xbabeface;
	} else {
		mask = 0x1ff;
		started = 0x100;
	}
	put(&gmu, "general_9", GMU_GENERAL_9, 0);
	gmu_runs = true;
	put(&gmu, "cm3_sysreset", GMU_CM3_SYSRESET, 0);
	for (int i = 0; i < 1000 && (result & mask) != started; i++) {
		sleep_us(100);
		result = rd(gmu.base + GMU_CM3_FW_INIT_RESULT);
	}
	get(&gmu, "cm3_fw_init_result", GMU_CM3_FW_INIT_RESULT);
	get(&gmu, "gmu2host_intr_info", GMU_GMU2HOST_INTR_INFO);
	get(&gmu, "ao_host_interrupt_status", GMU_AO_HOST_INTERRUPT_STATUS);
	if (space_faulted(&gmu_space) || (result & mask) != started) {
		printf("adreno: %u: the GMU's firmware does not come up\n", step);
		gmu_post_mortem();
		return false;
	}
	printf("adreno: %u: THE GMU'S FIRMWARE RUNS\n", step);
	announce("start the message queues (hfi_ctrl_init)");
	put(&gmu, "hfi_ctrl_init", GMU_HFI_CTRL_INIT, 1);
	if (!wait_for(&gmu, GMU_HFI_CTRL_STATUS, 1, 1, 100)) {
		printf("adreno: %u: the queues do not start (%#x)\n", step, rd(gmu.base + GMU_HFI_CTRL_STATUS));
		gmu_post_mortem();
		return false;
	}
	printf("adreno: %u: THE GMU'S MESSAGE QUEUES ARE UP\n", step);
	return true;
}

/* ---- step 15: talk to the GMU; the 3D core powered -------------------------------------------------------- */

/* RPMh's address for a resource and, for a rail, its levels (cmd-db.c); 0 if it has none such */
static uint32_t
rpmh_resource(const char *name, uint16_t *levels, unsigned *level_count)
{
	const struct cmd_db_header *db;
	char id[8] = { 0 };

	if (level_count != NULL) {
		*level_count = 0;
	}
	if (cmd_db == NULL) {
		cmd_db = (const struct cmd_db_header *)ml_io_map_wcomb(CMD_DB_PHYS, CMD_DB_SIZE);
	}
	db = cmd_db;
	if (db->magic[0] != 0xdb || db->magic[1] != 0x30 || db->magic[2] != 0x03 || db->magic[3] != 0x0c) {
		return 0;
	}
	strlcpy(id, name, sizeof(id));
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(db->data + slave->header_offset);

		if (slave->id == 0 || slave->count > 512) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			const uint8_t *data = db->data + slave->data_offset + entry[i].offset;

			if (memcmp(entry[i].id, id, sizeof(id)) != 0) {
				continue;
			}
			for (unsigned l = 0; levels != NULL && l < entry[i].length / 2u && l < 16; l++) {
				levels[l] = (uint16_t)(data[2 * l] | data[2 * l + 1] << 8);
				if (l > 0 && levels[l] == 0) {
					break;
				}
				*level_count = l + 1;
			}
			return entry[i].address;
		}
	}
	return 0;
}

#define HFI_BW_TABLE            3
#define HFI_PERF_TABLE          4
#define HFI_START               10
#define HFI_CORE_FW_START       14
#define HFI_GX_BW_PERF_VOTE     30
#define HFI_ERROR               100
#define HFI_ANSWER              1
#define HFI_NO_INDEX            0xff

struct hfi_perf_table {
	uint32_t header, gpu_levels, gmu_levels;
	struct {
		uint32_t vote, acd, khz;
	} gpu[16];
	struct {
		uint32_t vote, khz;
	} gmu[4];
};
struct hfi_bw_table {
	uint32_t header, levels, cnoc_commands, ddr_commands, cnoc_wait, ddr_wait;
	uint32_t cnoc_address[6], cnoc_data[2][6], ddr_address[8], ddr_data[16][8];
};

static uint32_t hfi_number;
static bool post_mortem_done, core_is_on;

static bool
hfi_receive(unsigned which, uint32_t number, uint32_t *verdict)
{
	volatile struct hfi_queue *queue = &hfi->queue[which];
	volatile uint32_t *data = (volatile uint32_t *)((vm_offset_t)hfi + HFI_QUEUE_AT(which));
	bool answered = false;

	while (queue->read != queue->write) {
		uint32_t message[8] = { 0 }, at = queue->read % HFI_QUEUE_WORDS, words = data[at] >> 8 & 0xff;

		for (uint32_t i = 0; i < words; i++) {
			if (i < 8) {
				message[i] = data[at];
			}
			at = (at + 1) % HFI_QUEUE_WORDS;
		}
		queue->read = (at + 3) / 4 * 4 % HFI_QUEUE_WORDS;
		if ((message[0] >> 16 & 0xf) == HFI_ANSWER && message[1] >> 20 == number) {
			*verdict = message[2];
			answered = true;
		} else {
			message[7] = 0;
			if ((message[0] & 0xff) == HFI_ERROR) {
				printf("adreno: %u: THE GMU REPORTS AN ERROR: %u, %u, \"%s\"\n", step, message[1] >> 16,
				    message[1] & 0xffff, (const char *)&message[2]);
			} else {
				printf("adreno: %u: the GMU says (queue %u, %u words): %#x %#x %#x %#x %#x %#x\n", step, which, words,
				    message[0], message[1], message[2], message[3], message[4], message[5]);
			}
		}
		if (words == 0) {
			break;
		}
	}
	return answered;
}

static bool
hfi_send(const char *what, uint32_t id, uint32_t *message, uint32_t words)
{
	volatile struct hfi_queue *queue = &hfi->queue[0];
	volatile uint32_t *data = (volatile uint32_t *)((vm_offset_t)hfi + HFI_QUEUE_AT(0));
	uint32_t at = queue->write % HFI_QUEUE_WORDS, number = ++hfi_number & 0xfff, verdict = 0, waiting = 0;
	bool answered = false;

	message[0] = id | words << 8 | number << 20;
	printf("adreno: %u: send the GMU %s: message %u, %u words\n", step, what, id, words);
	snprintf(last_write, sizeof(last_write), "step %u sent the GMU message %u (%s)", step, id, what);
	hold(true);
	for (uint32_t i = 0; i < (words + 3) / 4 * 4; i++) {
		data[at] = i < words ? message[i] : 0xfafafafa;
		at = (at + 1) % HFI_QUEUE_WORDS;
	}
	queue->write = at;
	settle();
	wr(gmu.base + GMU_HOST2GMU_INTR_SET, 1);
	for (int i = 0; i < 10000 && !answered; i++) {
		sleep_us(100);
		waiting = rd(gmu.base + GMU_GMU2HOST_INTR_INFO);
		if (waiting & 1) {
			wr(gmu.base + GMU_GMU2HOST_INTR_CLR, 1);
			answered = hfi_receive(1, number, &verdict);
		}
	}
	hfi_receive(2, ~0u, &waiting);
	if (!answered || verdict != 0) {
		printf("adreno: %u: the GMU %s (%#x) to %s; fw_init_result %#x, gmu2host_intr_info %#x\n", step,
		    answered ? "says no" : "does not answer", verdict, what, rd(gmu.base + GMU_CM3_FW_INIT_RESULT),
		    rd(gmu.base + GMU_GMU2HOST_INTR_INFO));
		space_faulted(&gmu_space);
		if (!post_mortem_done) {
			post_mortem_done = true;
			gmu_post_mortem();
		}
		return false;
	}
	printf("adreno: %u: the GMU accepts %s\n", step, what);
	return true;
}

static bool
rail_vote(uint32_t level, const uint16_t *rail, unsigned rail_levels, const uint16_t *mx, unsigned mx_levels, uint32_t *vote)
{
	unsigned at, with = 0;

	for (at = 0; at < rail_levels && rail[at] < level; at++) {
	}
	if (at == rail_levels || mx_levels == 0) {
		return false;
	}
	while (with + 1 < mx_levels && mx[with] < rail[at]) {
		with++;
	}
	*vote = (uint32_t)rail[at] << 16 | with << 8 | at;
	return true;
}

/* dt: kgsl_3d0_reg_memory: the 3D core's registers; a6xx.xml counts in words */
static struct block gpu = { "gpu", 0x03d00000, 0x40000 };
#define GPU_WORD(n)             (4u * (n))
#define RBBM_INT_0_STATUS       GPU_WORD(0x0201)
#define RBBM_STATUS             GPU_WORD(0x0210)
#define CP_RB_BASE              GPU_WORD(0x0800)
#define GMU_SPTPRAC_PWR_CLK_STATUS GMU_WORD(0x50d0)
#define GMU_CORE_REQUEST        (1u << 30)      /* GMU_OOB_GPU_SET, the "new" bits */
#define GMU_CORE_GRANTED        (1u << 31)

/*
 * a6xx_hfi_start() for the 7xx: the table of speeds of the 3D core with their supply levels (the phone's own, dt:
 * qcom,gpu-pwrlevels, which go on to 900 MHz; 580 by default, the phone's own level nearest 600: 816 heated the phone with no thermal guard running; adreno7_speed=5 for 645, 6 for 765, 7 for 816) and of the GMU; the bus votes (a730_build_bw_table,
 * the "off" level only); "core firmware start"; the speed. Then GMU_OOB_GPU_SET: keep the 3D core powered for us.
 */
static bool
step_core_power(void)
{
	static const struct {
		uint32_t khz, level;
	} speeds[] = { { 0, 0 }, { 285000, 56 }, { 439000, 96 }, { 515000, 128 }, { 580000, 144 }, { 645000, 192 }, { 765000, 256 }, { 816000, 320 } };
	static struct hfi_perf_table perf;
	static struct hfi_bw_table bw;
	uint32_t core_start[2] = { 0, 0 }, start[1] = { 0 }, vote[4] = { 0, 1, 4, HFI_NO_INDEX }, granted = 0, speed = 0;
	uint16_t gfx[16], cx[16], mx[16];
	unsigned gfx_levels, cx_levels, mx_levels;

	if (!cx_ready() || !gmu_runs) {
		return false;
	}
	if (!rpmh_resource("gfx.lvl", gfx, &gfx_levels) || !rpmh_resource("cx.lvl", cx, &cx_levels) ||
	    !rpmh_resource("mx.lvl", mx, &mx_levels)) {
		printf("adreno: %u: RPMh's list lacks one of the rails\n", step);
		return false;
	}
	perf.gpu_levels = sizeof(speeds) / sizeof(speeds[0]);
	for (unsigned i = 0; i < perf.gpu_levels; i++) {
		perf.gpu[i].khz = speeds[i].khz;
		perf.gpu[i].acd = 0xffffffff;
		if (!rail_vote(speeds[i].level, gfx, gfx_levels, mx, mx_levels, &perf.gpu[i].vote)) {
			printf("adreno: %u: RPMh has no level %u for the 3D core's rail\n", step, speeds[i].level);
			return false;
		}
	}
	perf.gmu_levels = 2;
	perf.gmu[1].khz = 200000;
	if (!rail_vote(0, cx, cx_levels, mx, mx_levels, &perf.gmu[0].vote) ||
	    !rail_vote(48, cx, cx_levels, mx, mx_levels, &perf.gmu[1].vote)) {
		return false;
	}
	printf("adreno: %u: votes: 3D core %#x %#x %#x %#x, GMU %#x %#x\n", step, perf.gpu[0].vote, perf.gpu[1].vote,
	    perf.gpu[2].vote, perf.gpu[3].vote, perf.gmu[0].vote, perf.gmu[1].vote);
	bw.levels = 1;
	bw.ddr_commands = 3;
	bw.ddr_wait = 7;
	bw.ddr_address[0] = rpmh_resource("SH0", NULL, NULL);
	bw.ddr_address[1] = rpmh_resource("MC0", NULL, NULL);
	bw.ddr_address[2] = rpmh_resource("ACV", NULL, NULL);
	bw.ddr_data[0][0] = bw.ddr_data[0][1] = bw.ddr_data[0][2] = 0x40000000;
	bw.cnoc_commands = 1;
	bw.cnoc_wait = 1;
	bw.cnoc_address[0] = rpmh_resource("CN0", NULL, NULL);
	bw.cnoc_data[0][0] = 0x40000000;
	bw.cnoc_data[1][0] = 0x60000001;
	printf("adreno: %u: RPMh's bus resources: SH0 %#x, MC0 %#x, ACV %#x, CN0 %#x\n", step, bw.ddr_address[0],
	    bw.ddr_address[1], bw.ddr_address[2], bw.cnoc_address[0]);
	if (bw.ddr_address[0] == 0 || bw.ddr_address[1] == 0 || bw.ddr_address[2] == 0 || bw.cnoc_address[0] == 0) {
		return false;
	}
	if (PE_parse_boot_argn("adreno7_speed", &speed, sizeof(speed)) && speed >= 1 && speed < perf.gpu_levels) {
		vote[2] = speed;
	}
	printf("adreno: %u: the speed wanted: %u MHz\n", step, speeds[vote[2]].khz / 1000);
	gpu_mhz_asked = speeds[vote[2]].khz / 1000;
	announce("send the GMU its tables (speeds and supply levels, bus votes), \"core firmware start\", the speed");
	if (!hfi_send("the table of speeds", HFI_PERF_TABLE, &perf.header, sizeof(perf) / 4) ||
	    !hfi_send("the table of bus votes", HFI_BW_TABLE, &bw.header, sizeof(bw) / 4) ||
	    !hfi_send("\"start the core firmware\"", HFI_CORE_FW_START, core_start, 2) ||
	    !hfi_send("\"start\"", HFI_START, start, 1) ||
	    !hfi_send("the speed wanted", HFI_GX_BW_PERF_VOTE, vote, 4)) {
		return false;
	}
	announce("ask the GMU to power the 3D core and keep it powered");
	get(&gmu, "sptprac_pwr_clk_status", GMU_SPTPRAC_PWR_CLK_STATUS);
	get(&gpucc, "gx_gdscr", GPUCC_GX_GDSCR);
	put(&gmu, "host2gmu_intr_set", GMU_HOST2GMU_INTR_SET, GMU_CORE_REQUEST);
	for (int i = 0; i < 5000 && !(granted & GMU_CORE_GRANTED); i++) {
		sleep_us(100);
		granted = rd(gmu.base + GMU_GMU2HOST_INTR_INFO);
	}
	core_is_on = true;
	put(&gmu, "gmu2host_intr_clr", GMU_GMU2HOST_INTR_CLR, GMU_CORE_GRANTED);
	get(&gmu, "sptprac_pwr_clk_status", GMU_SPTPRAC_PWR_CLK_STATUS);
	get(&gpucc, "gx_gdscr", GPUCC_GX_GDSCR);
	get(&gmu, "rpmh_power_state", GMU_RPMH_POWER_STATE);
	if (!(granted & GMU_CORE_GRANTED) || (rd(gmu.base + GMU_SPTPRAC_PWR_CLK_STATUS) & 0xc0) != 0) {
		printf("adreno: %u: the GMU does not power the 3D core (gmu2host_intr_info %#x, fw_init_result %#x)\n", step,
		    granted, rd(gmu.base + GMU_CM3_FW_INIT_RESULT));
		hfi_receive(2, ~0u, &granted);
		space_faulted(&gmu_space);
		return false;
	}
	announce("read registers of the 3D core itself (0x3d00000): the first time it has power");
	get(&gpu, "rbbm_status", RBBM_STATUS);
	get(&gpu, "rbbm_int_0_status", RBBM_INT_0_STATUS);
	get(&gpu, "cp_rb_base", CP_RB_BASE);
	printf("adreno: %u: THE 3D CORE HAS POWER AND ANSWERS\n", step);
	return !space_faulted(&gmu_space);
}

/* ---- step 16: the 3D core's memory and its microcode ------------------------------------------------------ */

/*
 * The 3D core reads its orders from a ring of memory with a small processor of its own ("SQE", microcode
 * a730_sqe.fw), through the SMMU: streams 0 and 1 (Linux's sm8450.dtsi), here both through context 0, with tables
 * of ours that hold the ring, the microcode and a page to try writing to.
 */
#define CORE_STREAM             0
#define CORE_MATCH              0
#define SECOND_MATCH            2
#define PTE_CORE                (PTE_GMU | PTE_UNPRIVILEGED)
#define PTE_READ_ONLY           (1ull << 7)
#define CORE_RING_ADDRESS       0x01000000u
#define CORE_RING_SIZE          0x8000u
#define CORE_SQE_ADDRESS        0x01100000u
#define CORE_SCRATCH_ADDRESS    0x01200000u
#define SMMU_SCTLR_SERVE_AFTER_FAULT (1u << 8)

static struct gpu_space core_space = { .name = "the 3D core", .context = 0, .control = SMMU_SCTLR_SERVE_AFTER_FAULT };
static struct gpu_block core_ring, core_sqe, core_scratch;
static bool core_space_is_on, core_runs, zap_is_loaded;
#define RING_WORDS              (CORE_RING_SIZE / 4)
static uint32_t ring_at, ring_sent;

#define CP_RB_CNTL              GPU_WORD(0x0802)
#define CP_RB_RPTR_ADDR         GPU_WORD(0x0804)
#define CP_RB_RPTR              GPU_WORD(0x0806)
#define CP_RB_WPTR              GPU_WORD(0x0807)
#define CP_SQE_CNTL             GPU_WORD(0x0808)
#define CP_HW_FAULT             GPU_WORD(0x0821)
#define CP_INTERRUPT_STATUS     GPU_WORD(0x0823)
#define CP_PROTECT_STATUS       GPU_WORD(0x0824)
#define CP_SQE_INSTR_BASE       GPU_WORD(0x0830)
#define CP_APRIV_CNTL           GPU_WORD(0x0844)
#define CP_AHB_CNTL             GPU_WORD(0x098d)
#define CP_BV_RB_RPTR_ADDR      GPU_WORD(0x0a98)
#define CP_BV_APRIV_CNTL        GPU_WORD(0x0ad0)
#define CP_LPAC_APRIV_CNTL      GPU_WORD(0x0b31)
#define RBBM_GBIF_CLIENT_QOS_CNTL GPU_WORD(0x0011)
#define RBBM_GBIF_HALT          GPU_WORD(0x0016)
#define RBBM_INTERFACE_HANG_INT_CNTL GPU_WORD(0x001f)
#define RBBM_PERFCTR_CNTL       GPU_WORD(0x0500)
#define RBBM_PERFCTR_GPU_BUSY_MASKED GPU_WORD(0x050b)
#define UCHE_WRITE_THRU_BASE    GPU_WORD(0x0e07)
#define UCHE_TRAP_BASE          GPU_WORD(0x0e09)
#define UCHE_CACHE_WAYS         GPU_WORD(0x0e17)
#define UCHE_CLIENT_PF          GPU_WORD(0x0e19)
#define UCHE_GBIF_GX_CONFIG     GPU_WORD(0x0e3a)
#define UCHE_CMDQ_CONFIG        GPU_WORD(0x0e3c)
#define RB_CMP_DBG_ECO_CNTL     GPU_WORD(0x8e28)
#define RBBM_SECVID_TRUST_CNTL  GPU_WORD(0xf400)
#define RBBM_SECVID_TSB_TRUSTED_BASE GPU_WORD(0xf800)
#define RBBM_SECVID_TSB_TRUSTED_SIZE GPU_WORD(0xf802)
#define RBBM_SECVID_TSB_CNTL    GPU_WORD(0xf803)
#define GBIF_QSB_SIDE0          (4 * 0x03)
#define CP_RB_CNTL_VALUE        (12u | 2u << 8)         /* 2^12 pairs of words, blocks of 4 (MSM_GPU_RB_CNTL_DEFAULT) */
#define A7XX_APRIV_MASK         0xfu                    /* icache, ring fetch, ring privilege, read pointer write-back */
#define A7XX_BR_APRIVMASK       (A7XX_APRIV_MASK | 1u << 5 | 1u << 6)

#define CP_THREAD_CONTROL       0x17
#define CP_MEM_WRITE            0x3d
#define CP_ME_INIT              0x48
#define CP_SET_SECURE_MODE      0x66

static uint32_t
odd_parity(uint32_t v)
{
	return 0x9669 >> (0xf & (v ^ v >> 4 ^ v >> 8 ^ v >> 12 ^ v >> 16 ^ v >> 20 ^ v >> 24 ^ v >> 28)) & 1;
}

static void
ring(uint32_t word)
{
	((uint32_t *)core_ring.memory)[ring_at] = word;
	ring_at = (ring_at + 1) % RING_WORDS;
}

static void
ring_command(uint32_t command, uint32_t words)
{
	ring(7u << 28 | words | odd_parity(words) << 15 | command << 16 | odd_parity(command) << 23);
}

static void
ring_send(void)
{
	if (ring_at >= ring_sent) {
		to_memory(core_ring.memory + 4 * ring_sent, 4 * (ring_at - ring_sent));
	} else {
		to_memory(core_ring.memory + 4 * ring_sent, 4 * (RING_WORDS - ring_sent));
		to_memory(core_ring.memory, 4 * ring_at);
	}
	ring_sent = ring_at;
	wr(gpu.base + CP_RB_WPTR, ring_at);
}

static bool
ring_run(const char *what)
{
	uint32_t taken = ~0u, status = ~0u;

	printf("adreno: %u: have the 3D core %s: the ring up to word %u\n", step, what, ring_at);
	snprintf(last_write, sizeof(last_write), "step %u had the 3D core %s", step, what);
	hold(true);
	ring_send();
	for (int i = 0; i < 20000 && (taken != ring_at || (status & ~1u)); i++) {
		sleep_us(100);
		taken = rd(gpu.base + CP_RB_RPTR);
		status = rd(gpu.base + RBBM_STATUS);
	}
	if (taken == ring_at && !(status & ~1u) && rd(gpu.base + CP_HW_FAULT) == 0 && !space_faulted(&core_space)) {
		printf("adreno: %u: the 3D core has done it (ring read up to %u, status %#x)\n", step, taken, status);
		return true;
	}
	printf("adreno: %u: the 3D core does not get through: ring read up to %u of %u, status %#x, interrupts %#x, "
	    "command processor: fault %#x, interrupts %#x, protection %#x\n", step, taken, ring_at, status,
	    rd(gpu.base + RBBM_INT_0_STATUS), rd(gpu.base + CP_HW_FAULT), rd(gpu.base + CP_INTERRUPT_STATUS),
	    rd(gpu.base + CP_PROTECT_STATUS));
	space_faulted(&core_space);
	return false;
}

static void
put64(struct block *b, const char *name, uint32_t offset, uint64_t value)
{
	put(b, name, offset, (uint32_t)value);
	put(b, name, offset + 4, (uint32_t)(value >> 32));
}

static bool
step_core_space(void)
{
	const uint8_t *microcode;
	uint64_t size = 0;

	if (!cx_ready() || !core_is_on) {
		return false;
	}
	announce("make the 3D core's memory: its ring, its microcode (fw/a730_sqe.fw), a page to try writing to");
	microcode = image_file("a730_sqe.fw", &size);
	if (microcode == NULL || size < 8 || size > 0x40000 ||
	    !block_new(&core_space, &core_ring, CORE_RING_ADDRESS, CORE_RING_SIZE, PTE_CORE) ||
	    !block_new(&core_space, &core_sqe, CORE_SQE_ADDRESS, (uint32_t)size - 4, PTE_CORE | PTE_READ_ONLY) ||
	    !block_new(&core_space, &core_scratch, CORE_SCRATCH_ADDRESS, 4096, PTE_CORE)) {
		return false;
	}
	memcpy((void *)core_sqe.memory, microcode + 4, size - 4);       /* the file's first word is not microcode */
	to_memory(core_sqe.memory, core_sqe.size);
	printf("adreno: %u: the microcode's version words: %#x %#x\n", step, ((uint32_t *)core_sqe.memory)[0],
	    ((uint32_t *)core_sqe.memory)[1]);
	announce("give the SMMU's context 0 these tables and send the 3D core's requests (streams 0 and 1) through it");
	core_space_is_on = true;
	if (!space_on(&core_space, CORE_MATCH, CORE_STREAM) || !space_stream(&core_space, SECOND_MATCH, 1)) {
		return false;
	}
	get(&gpu, "rbbm_status", RBBM_STATUS);
	return !space_faulted(&core_space);
}

/* ---- step 17: the 3D core's registers; its command processor started and initialized ------------------------ */

/* Linux's hw_init() for the 730, less clock gating, register protection and the compressed formats' settings */
static void ubwc_config(void);

static bool
step_core_start(void)
{
	if (!cx_ready() || !core_is_on || !core_space_is_on) {
		return false;
	}
	announce("write the 3D core's registers as Linux does for the 730 before it starts the command processor");
	put(&gbif, "halt", GBIF_HALT, 0);
	put(&gpu, "rbbm_gbif_halt", RBBM_GBIF_HALT, 0);
	put(&gpu, "rbbm_secvid_tsb_cntl", RBBM_SECVID_TSB_CNTL, 0);
	put64(&gpu, "rbbm_secvid_tsb_trusted_base", RBBM_SECVID_TSB_TRUSTED_BASE, 0);
	put(&gpu, "rbbm_secvid_tsb_trusted_size", RBBM_SECVID_TSB_TRUSTED_SIZE, 0);
	for (unsigned i = 0; i < 4; i++) {
		put(&gbif, "qsb_side", GBIF_QSB_SIDE0 + 4 * i, 0x00071620);
	}
	put(&gpu, "rbbm_gbif_client_qos_cntl", RBBM_GBIF_CLIENT_QOS_CNTL, 0x2120212);
	put(&gpu, "uche_gbif_gx_config", UCHE_GBIF_GX_CONFIG, 0x10240e0);
	put(&gpu, "rbbm_perfctr_gpu_busy_masked", RBBM_PERFCTR_GPU_BUSY_MASKED, 0xffffffff);
	put64(&gpu, "uche_trap_base", UCHE_TRAP_BASE, 0x0001fffffffff000ull);
	put64(&gpu, "uche_write_thru_base", UCHE_WRITE_THRU_BASE, 0x0001fffffffff000ull);
	put(&gpu, "uche_cache_ways", UCHE_CACHE_WAYS, 1u << 23);
	put(&gpu, "cp_ahb_cntl", CP_AHB_CNTL, 1);
	put(&gpu, "rbbm_perfctr_cntl", RBBM_PERFCTR_CNTL, 1);
	put(&gpu, "rbbm_interface_hang_int_cntl", RBBM_INTERFACE_HANG_INT_CNTL, 1u << 30 | 0xcfffff);
	put(&gpu, "uche_client_pf", UCHE_CLIENT_PF, 1u << 7 | 1);
	put(&gpu, "uche_cmdq_config", UCHE_CMDQ_CONFIG, 6u << 16 | 6u << 12 | 9u << 8 | 1u << 3 | 1u << 2 | 2);
	put(&gpu, "cp_apriv_cntl", CP_APRIV_CNTL, A7XX_BR_APRIVMASK);
	put(&gpu, "cp_bv_apriv_cntl", CP_BV_APRIV_CNTL, A7XX_APRIV_MASK);
	put(&gpu, "cp_lpac_apriv_cntl", CP_LPAC_APRIV_CNTL, A7XX_APRIV_MASK);
	put(&gpu, "rb_cmp_dbg_eco_cntl", RB_CMP_DBG_ECO_CNTL, rd(gpu.base + RB_CMP_DBG_ECO_CNTL) | 1u << 11);
	ubwc_config();
	put64(&gpu, "cp_sqe_instr_base", CP_SQE_INSTR_BASE, CORE_SQE_ADDRESS);
	put64(&gpu, "cp_rb_base", CP_RB_BASE, CORE_RING_ADDRESS);
	put(&gpu, "cp_rb_cntl", CP_RB_CNTL, CP_RB_CNTL_VALUE);
	put64(&gpu, "cp_rb_rptr_addr", CP_RB_RPTR_ADDR, CORE_SCRATCH_ADDRESS + 0x100);
	put64(&gpu, "cp_bv_rb_rptr_addr", CP_BV_RB_RPTR_ADDR, CORE_SCRATCH_ADDRESS + 0x200);
	get(&gpu, "cp_sqe_instr_base", CP_SQE_INSTR_BASE);
	get(&gpu, "cp_rb_base", CP_RB_BASE);
	get(&gpu, "rbbm_status", RBBM_STATUS);

	announce("start the 3D core's command processor");
	core_runs = true;
	put(&gpu, "cp_sqe_cntl", CP_SQE_CNTL, 1);
	sleep_ms(20);
	printf("adreno: %u: alive after it; status %#x, interrupts %#x, command processor: fault %#x, interrupts %#x, "
	    "protection %#x, ring read up to %u\n", step, rd(gpu.base + RBBM_STATUS), rd(gpu.base + RBBM_INT_0_STATUS),
	    rd(gpu.base + CP_HW_FAULT), rd(gpu.base + CP_INTERRUPT_STATUS), rd(gpu.base + CP_PROTECT_STATUS),
	    rd(gpu.base + CP_RB_RPTR));
	if (space_faulted(&core_space)) {
		return false;
	}
	announce("give the 3D core its first commands: no concurrent binning, \"initialize\" (a7xx_cp_init)");
	ring_command(CP_THREAD_CONTROL, 1);
	ring(1u << 27);
	ring_command(CP_ME_INIT, 7);
	ring(1u << 0 | 1u << 1 | 1u << 3 | 1u << 6 | 1u << 8);
	ring(0x00000003);
	ring(0x20000000);
	ring(0x00000002);
	ring(0);
	ring(0);
	ring(0);
	if (!ring_run("initialize its command processor")) {
		return false;
	}
	printf("adreno: %u: THE 3D CORE TAKES COMMANDS\n", step);
	return true;
}

/* ---- step 18: the zap shader loaded; the core out of secure mode; a word written on command ------------------ */

/*
 * The rest of what Linux does with the zap shader (qcom_mdt_pas_init, qcom_mdt_load), after the command processor
 * is initialized as Linux has it: TrustZone gives up one a kernel before may have left, takes the header, is told
 * where the image goes (dt: gpu_microcode_region@8b91a000, 0x2000), the loadable segment (a730_zap.b01, relocatable)
 * is put there, and TrustZone authenticates and runs it. Then CP_SET_SECURE_MODE 0, and a word written to memory.
 */
#define ZAP_MEMORY              0x8b91a000ull
#define ZAP_MEMORY_SIZE         0x2000u
#define SCM_PAS_MEM_SETUP       SCM_CALL(2, 2)
#define SCM_PAS_SHUTDOWN        SCM_CALL(2, 6)
#define SCM_THREE_VALUES        3

static bool
step_zap_and_secure(void)
{
	const uint8_t *segment, *header;
	volatile uint8_t *memory;
	volatile uint32_t *scratch = (volatile uint32_t *)core_scratch.memory;
	vm_offset_t page = 0;
	uint64_t size = 0, header_bytes = 0, answer = ~0ull;

	if (!cx_ready() || !core_runs) {
		return false;
	}
	announce("ask TrustZone to give up a zap shader a kernel before this one may have left");
	scm("to give up the zap shader", SCM_64 | SCM_SIP | SCM_PAS_SHUTDOWN, SCM_ONE_VALUE, PAS_GPU, 0, 0, &answer);
	segment = image_file("a730_zap.b01", &size);
	header = image_file("a730_zap.mdt", &header_bytes);
	if (segment == NULL || header == NULL || size == 0 || size > 0x1000 || header_bytes > PAGE_SIZE) {
		return false;
	}
	if (kmem_alloc(kernel_map, &page, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		return false;
	}
	memcpy((void *)page, header, header_bytes);
	to_memory(page, PAGE_SIZE);
	announce("hand TrustZone the header, tell it where the image goes, put the image there, have it authenticated and run");
	if (scm("to take the zap shader's signature", SCM_64 | SCM_SIP | SCM_PAS_INIT_IMAGE, SCM_VALUE_AND_BUFFER, PAS_GPU,
	    kvtophys(page), 0, &answer) != 0 || answer != 0) {
		printf("adreno: %u: TrustZone does not take the signature (%#llx)\n", step, answer);
		return false;
	}
	if (scm("where the image will be", SCM_64 | SCM_SIP | SCM_PAS_MEM_SETUP, SCM_THREE_VALUES, PAS_GPU, ZAP_MEMORY, 0x1000,
	    &answer) != 0 || answer != 0) {
		printf("adreno: %u: TrustZone does not take the place (%#llx)\n", step, answer);
		return false;
	}
	/*
	 * Device memory, not write-combined: once TrustZone has the image it protects this memory, and a speculative read
	 * through a normal mapping would then be a violation (seen with a write-combined mapping: the kernel stopped
	 * right after the image was authenticated). Unmapped again before TrustZone is asked to authenticate.
	 */
	memory = (volatile uint8_t *)ml_io_map(ZAP_MEMORY, ZAP_MEMORY_SIZE);
	printf("adreno: %u: write %llu bytes of the zap shader at %#llx (it begins %02x %02x %02x %02x)\n", step, size,
	    ZAP_MEMORY, segment[0], segment[1], segment[2], segment[3]);
	hold(true);
	for (uint64_t i = 0; i < 0x1000; i++) {
		memory[i] = i < size ? segment[i] : 0;
	}
	settle();
	if (scm("to authenticate the zap shader and run it", SCM_64 | SCM_SIP | SCM_PAS_AUTH_AND_RESET, SCM_ONE_VALUE, PAS_GPU, 0,
	    0, &answer) != 0 || answer != 0) {
		printf("adreno: %u: TrustZone does not authenticate the zap shader (%#llx)\n", step, answer);
		return false;
	}
	printf("adreno: %u: TRUSTZONE HAS AUTHENTICATED AND STARTED THE ZAP SHADER\n", step);
	zap_is_loaded = true;

	announce("command the 3D core out of secure mode, then to write a word into a page of ours");
	printf("adreno: %u: back from the pause\n", step);
	ring_command(CP_SET_SECURE_MODE, 1);
	ring(0);
	if (!ring_run("leave secure mode")) {
		return false;
	}
	ring_command(CP_MEM_WRITE, 3);
	ring(CORE_SCRATCH_ADDRESS);
	ring(0);
	ring(0x600dc0de);
	if (!ring_run("write a word to memory")) {
		return false;
	}
	to_memory(core_scratch.memory, 64);
	printf("adreno: %u: the page reads %#x: %s\n", step, scratch[0], scratch[0] == 0x600dc0de ?
	    "THE 3D CORE WRITES OUR MEMORY ON COMMAND" : "not what it was told to write");
	return scratch[0] == 0x600dc0de;
}

/* ---- the GPU kept on, for the system to draw with ------------------------------------------------------------ */

/*
 * With boot-arg adreno7_keep=1 the GPU stays as step 18 leaves it, and the service HakiosGPU (drivers/gpu/
 * hakios.gpu.cpp) hands it to programs through the interface of Linux's msm driver, which Mesa's freedreno speaks.
 * As drivers/gpu/hakios.adreno.c does it for redwood; what differs is the 7xx: the answers to GET_PARAM, the
 * compressed formats' settings, and how a submission goes into the ring (Linux's a7xx_submit(): the two command
 * threads, BR and BV, and a fence written by each).
 */
#define GPU_CHIP_ID             0x07030002ull   /* dt: qcom,chipid ("Adreno730v3"; Mesa: FD725) */
#define GPU_ADDRESSES_START     0x20000000ull
#define GPU_ADDRESSES_SIZE      0xe0000000ull
#define GPU_FENCE_AT            0x40            /* in the scratch page: BR's fence */
#define GPU_BV_FENCE_AT         0x80            /* ... and BV's */
#define CP_SCRATCH_REG2         0x0885
#define CP_IB1_BASE             GPU_WORD(0x0928)
#define CP_IB1_REM_SIZE         GPU_WORD(0x092a)
#define CP_IB2_BASE             GPU_WORD(0x092b)
#define CP_IB2_REM_SIZE         GPU_WORD(0x092d)
#define CP_INDIRECT_BUFFER      0x3f
#define CP_EVENT_WRITE          0x46            /* CP_EVENT_WRITE7 on the 7xx */
#define CP_SET_MARKER           0x65
#define CP_WAIT_TIMESTAMP       0x14
#define THREAD_BR               1u
#define THREAD_BV               2u
#define THREAD_BOTH             3u
#define THREAD_SYNC             (1u << 31)
#define EVENT_CCU_INVALIDATE_DEPTH 24
#define EVENT_CCU_INVALIDATE_COLOR 25
#define EVENT_CACHE_CLEAN       49
#define EVENT_WRITE_ENABLED     (1u << 27)

static bool gpu_kept;
static uint32_t gpu_fence;
uint32_t gpu_log;
static struct {
	uint64_t start, size;
} gpu_free[1024];
static unsigned gpu_free_count;

extern void HakiosGPUStart(void);
extern void HakiosGPUStop(void);

static void
ring_register(uint32_t number, uint32_t value)
{
	ring(4u << 28 | 1 | odd_parity(1) << 7 | number << 8 | odd_parity(number) << 27);
	ring(value);
}

/* Linux's a6xx_set_ubwc_config() for the 730: highest bank bit 16, swizzle 6, amsbc, rgb565 predicator, macrotile */
static void
ubwc_config(void)
{
	put(&gpu, "rb_nc_mode_cntl", GPU_WORD(0x8e08), 1u << 11 | 1u << 4 | 3u << 1);
	put(&gpu, "tpl1_nc_mode_cntl", GPU_WORD(0xb604), 3u << 1);
	put(&gpu, "sp_nc_mode_cntl", GPU_WORD(0xae02), 2u << 4 | 3u << 1);
	put(&gpu, "gras_nc_mode_cntl", GPU_WORD(0x8602), 3u << 5);
	put(&gpu, "uche_mode_cntl", GPU_WORD(0x0e01), 3u << 21);
	put(&gpu, "rbbm_nc_mode_cntl", GPU_WORD(0x0534), 1);
}

/* The CP and the GMU halted, the SMMU contexts off: what a panic leaves (loader_return) */
void
hakios_adreno_quiesce(void)
{
	if (core_runs) {
		wr(gpu.base + CP_SQE_CNTL, 0);
		core_runs = false;
	}
	if (gmu_runs) {
		wr(gmu.base + GMU_CM3_SYSRESET, 1);
		gmu_runs = false;
	}
	if (gmu_space_is_on) {
		wr(smmu.base + smmu_context(&gmu_space) + SMMU_CB_SCTLR, 0);
		gmu_space_is_on = false;
	}
	if (core_space_is_on) {
		wr(smmu.base + smmu_context(&core_space) + SMMU_CB_SCTLR, 0);
		core_space_is_on = false;
	}
}

void
hakios_gpu_stop(void)
{
	if (gpu_kept) {
		gpu_kept = false;
		printf("adreno: the run is about to end: the GPU is given back\n");
		hakios_adreno_quiesce();
	}
}

void
hakios_adreno_stop(void)
{
	if (gpu_kept) {
		HakiosGPUStop();
	}
}

bool
hakios_gpu_ready(void)
{
	return gpu_kept;
}

/* The answers to msm's GET_PARAM (Linux's adreno_get_param() for the 730) */
bool
hakios_gpu_parameter(uint32_t which, uint64_t *value)
{
	switch (which) {
	case 0x01:                              /* GPU_ID: none, the chip ID says it */
	case 0x08:                              /* PP_PGTABLE */
	case 0x09:                              /* FAULTS */
	case 0x0a:                              /* SUSPENDS */
	case 0x11:                              /* RAYTRACING */
		*value = 0;
		return true;
	case 0x02:                              /* GMEM_SIZE */
		*value = 2 * 1024 * 1024;
		return true;
	case 0x06: {
		/*
		 * GMEM_BASE: 0. Linux gives this chip 0x100000, but like the 650 family it never programs
		 * UCHE_GMEM_RANGE, and with 0x100000 CoreAnimation's frames came out as misplaced blocks
		 * (31 October, screen captures). Boot-arg gmem_base= to compare.
		 */
		uint32_t base = 0;

		PE_parse_boot_argn("gmem_base", &base, sizeof(base));
		*value = base;
		return true;
	}
	case 0x03:                              /* CHIP_ID */
		*value = GPU_CHIP_ID;
		return true;
	case 0x04:                              /* MAX_FREQ */
		*value = 645000000ull;
		return true;
	case 0x05:                              /* TIMESTAMP */
		*value = mach_absolute_time();
		return true;
	case 0x07:                              /* PRIORITIES */
		*value = 1;
		return true;
	case 0x10:                              /* HIGHEST_BANK_BIT */
		*value = 16;
		return true;
	case 0x12:                              /* UBWC_SWIZZLE */
		*value = 6;
		return true;
	case 0x13:                              /* MACROTILE_MODE */
		*value = 1;
		return true;
	default:
		return false;
	}
}

uint64_t
hakios_gpu_address(uint64_t size)
{
	size = (size + 0xffff) & ~0xffffull;
	for (unsigned i = 0; i < gpu_free_count; i++) {
		if (gpu_free[i].size >= size) {
			uint64_t address = gpu_free[i].start;

			gpu_free[i].start += size;
			gpu_free[i].size -= size;
			if (gpu_free[i].size == 0) {
				memmove(&gpu_free[i], &gpu_free[i + 1], (--gpu_free_count - i) * sizeof(gpu_free[0]));
			}
			return address;
		}
	}
	return 0;
}

void
hakios_gpu_address_free(uint64_t address, uint64_t size)
{
	unsigned at;

	size = (size + 0xffff) & ~0xffffull;
	for (at = 0; at < gpu_free_count && gpu_free[at].start < address; at++) {
	}
	if ((at > 0 && gpu_free[at - 1].start + gpu_free[at - 1].size > address) ||
	    (at < gpu_free_count && address + size > gpu_free[at].start)) {
		printf("adreno: the addresses %#llx (%#llx bytes) are free already: not given back again\n", address, size);
		return;
	}
	if (at > 0 && gpu_free[at - 1].start + gpu_free[at - 1].size == address) {
		gpu_free[at - 1].size += size;
		if (at < gpu_free_count && address + size == gpu_free[at].start) {
			gpu_free[at - 1].size += gpu_free[at].size;
			memmove(&gpu_free[at], &gpu_free[at + 1], (--gpu_free_count - at) * sizeof(gpu_free[0]));
		}
	} else if (at < gpu_free_count && address + size == gpu_free[at].start) {
		gpu_free[at].start = address;
		gpu_free[at].size += size;
	} else if (gpu_free_count < sizeof(gpu_free) / sizeof(gpu_free[0])) {
		memmove(&gpu_free[at + 1], &gpu_free[at], (gpu_free_count++ - at) * sizeof(gpu_free[0]));
		gpu_free[at].start = address;
		gpu_free[at].size = size;
	}
}

bool
hakios_gpu_map(uint64_t address, uint64_t phys, bool read_only)
{
	return space_map(&core_space, address, phys, PTE_CORE | (read_only ? PTE_READ_ONLY : 0));
}

void
hakios_gpu_mapped(void)
{
	space_commit(&core_space);
}

void
hakios_gpu_unmap(uint64_t address, uint64_t size)
{
	uint32_t context = smmu_context(&core_space);

	for (uint64_t offset = 0; offset < size; offset += 4096) {
		space_map(&core_space, address + offset, 0, 0);
	}
	space_commit(&core_space);
	wr(smmu.base + context + SMMU_CB_TLBIASID, core_space.context);
	wr(smmu.base + context + SMMU_CB_TLBSYNC, 1);
	for (int i = 0; i < 1000 && (rd(smmu.base + context + SMMU_CB_TLBSTATUS) & 1); i++) {
	}
}

void
hakios_gpu_to_memory(void *start, uint64_t size)
{
	to_memory((vm_offset_t)start, size);
}

bool
hakios_gpu_done(uint32_t fence)
{
	volatile uint32_t *word = (volatile uint32_t *)(core_scratch.memory + GPU_FENCE_AT);

	to_memory((vm_offset_t)word, 4);
	return (int32_t)(*word - fence) >= 0;
}

uint32_t
hakios_gpu_last(void)
{
	return gpu_fence;
}

void
hakios_gpu_worked(uint64_t *microseconds, uint32_t *submissions, uint32_t by[7])
{
	*microseconds = 0;
	*submissions = 0;
	bzero(by, 7 * sizeof(uint32_t));
}

static void
ring_fence(uint32_t at, uint32_t fence)
{
	ring_command(CP_EVENT_WRITE, 4);
	ring(EVENT_CACHE_CLEAN | EVENT_WRITE_ENABLED);
	ring(CORE_SCRATCH_ADDRESS + at);
	ring(0);
	ring(fence);
}

/* Command buffers for the 3D core, in order, as Linux's a7xx_submit() puts them in the ring; 0: the ring stays full */
uint32_t
hakios_gpu_submit(const uint64_t *addresses, const uint32_t *words, uint32_t count)
{
	uint32_t wanted = 48 + 4 * count, room = 0;

	for (int i = 0; i < 20000; i++) {
		room = (rd(gpu.base + CP_RB_RPTR) + RING_WORDS - ring_at - 1) % RING_WORDS;
		if (room >= wanted) {
			break;
		}
		spin_ns(100 * NSEC_PER_USEC);
	}
	if (room < wanted) {
		return 0;
	}
	if (++gpu_fence == 0) {
		gpu_fence = 1;
	}
	ring_command(CP_THREAD_CONTROL, 1);
	ring(THREAD_SYNC | THREAD_BR);
	ring_command(CP_THREAD_CONTROL, 1);
	ring(THREAD_BOTH);
	ring_command(CP_SET_MARKER, 1);
	ring(0x101);                            /* IFPC off */
	ring_command(CP_SET_MARKER, 1);
	ring(0x00d);                            /* the list of command buffers starts */
	for (uint32_t i = 0; i < count; i++) {
		ring_command(CP_INDIRECT_BUFFER, 3);
		ring((uint32_t)addresses[i]);
		ring((uint32_t)(addresses[i] >> 32));
		ring(words[i]);
	}
	ring_command(CP_SET_MARKER, 1);
	ring(0x00e);                            /* ... and ends */
	ring_register(CP_SCRATCH_REG2, gpu_fence);
	ring_command(CP_THREAD_CONTROL, 1);
	ring(THREAD_BR);
	ring_command(CP_EVENT_WRITE, 1);
	ring(EVENT_CCU_INVALIDATE_DEPTH);
	ring_command(CP_EVENT_WRITE, 1);
	ring(EVENT_CCU_INVALIDATE_COLOR);
	ring_command(CP_THREAD_CONTROL, 1);
	ring(THREAD_BV);
	ring_fence(GPU_BV_FENCE_AT, gpu_fence);
	ring_command(CP_THREAD_CONTROL, 1);
	ring(THREAD_BR);
	ring_command(CP_WAIT_TIMESTAMP, 4);      /* BR does not get ahead of BV */
	ring(0);
	ring(CORE_SCRATCH_ADDRESS + GPU_BV_FENCE_AT);
	ring(0);
	ring(gpu_fence);
	ring_fence(GPU_FENCE_AT, gpu_fence);
	ring_command(CP_THREAD_CONTROL, 1);
	ring(THREAD_BOTH);
	ring_command(CP_SET_MARKER, 1);
	ring(0x100);                            /* IFPC on */
	ring_send();
	return gpu_fence;
}

void
hakios_gpu_report(const char *why)
{
	printf("adreno: gpu: %s: submission %u is the last, the core has done %u (BV %u); ring: read up to %u, written up to %u; "
	    "status %#x, interrupts %#x; command processor: fault %#x, interrupts %#x, protection %#x\n", why, gpu_fence,
	    *(volatile uint32_t *)(core_scratch.memory + GPU_FENCE_AT), *(volatile uint32_t *)(core_scratch.memory + GPU_BV_FENCE_AT),
	    rd(gpu.base + CP_RB_RPTR), ring_at, rd(gpu.base + RBBM_STATUS), rd(gpu.base + RBBM_INT_0_STATUS),
	    rd(gpu.base + CP_HW_FAULT), rd(gpu.base + CP_INTERRUPT_STATUS), rd(gpu.base + CP_PROTECT_STATUS));
	printf("adreno: gpu:   first command buffer at %#llx, %u words to go; second at %#llx, %u to go\n",
	    (uint64_t)rd(gpu.base + CP_IB1_BASE + 4) << 32 | rd(gpu.base + CP_IB1_BASE), rd(gpu.base + CP_IB1_REM_SIZE),
	    (uint64_t)rd(gpu.base + CP_IB2_BASE + 4) << 32 | rd(gpu.base + CP_IB2_BASE), rd(gpu.base + CP_IB2_REM_SIZE));
	space_faulted(&core_space);
}

/*
 * The panel is a command-mode one (dt: mdss_dsi_l3s_42_02_0a_dsc_cmd): it shows a new picture only when the display
 * controller sends one. What ABL leaves is the DSI interface's autorefresh (Linux's dpu_hw_intf.c:
 * INTF_TEAR_AUTOREFRESH_CONFIG 0x2b4, bit 31 on, [15:0] one refresh every so many tear signals) at 60 signals: measured
 * here, one frame a second however fast the frame buffer is written. Boot-arg panel_every=N sets that count (default
 * 1, a refresh on every tear signal; 0 leaves ABL's). Only intf 1 is touched (dt mdss_mdp@ae00000, qcom,sde-intf-off:
 * the first DSI interface at 0xae36000; the second is not in use and may have no clock).
 */
static void
panel_rate(vm_offset_t b, const char *when)
{
	uint32_t first, second;

	first = rd(b + 0x0ac);
	IOSleep(2000);
	second = rd(b + 0x0ac);
	printf("adreno: panel: %s: autorefresh %#x, vsync config %#x, height %#x, line %#x; frames a second: %u\n", when,
	    rd(b + 0x2b4), rd(b + 0x288), rd(b + 0x28c), rd(b + 0x2b0), (second - first) / 2);
}

/*
 * Boot-arg panel_hz=120: the panel's own mode switch, its timing-switch commands from the device tree (timing@1 of
 * mdss_dsi_l3s_42_02_0a_dsc_cmd: 120 Hz at the same 831 MHz link and 7 ms transfer as the 60 Hz mode ABL starts, so no
 * clock changes). Sent by the DSI controller's command DMA, high speed, from memory the display can already read: the
 * last page of what the frame buffer's region holds beyond the picture. Register offsets are Linux's dsi.xml + 4.
 */
#define DSI_CTRL_BASE           0x0ae94000
#define DSI_STATUS0             0x008
#define DSI_CMD_DMA_CTRL        0x03c
#define DSI_DMA_BASE            0x048
#define DSI_DMA_LEN             0x04c
#define DSI_TRIG_CTRL           0x084
#define DSI_TRIG_DMA            0x090
#define DSI_INTR_CTRL           0x110
#define DSI_LOW_POWER           (1U << 26)
#define DSI_DMA_BUSY            (1U << 1)
#define DSI_DMA_DONE_STATUS     (1U << 0)
#define PANEL_DMA_PAGE          0xb9000000ULL   /* FB 0xb8000000 + 16 MB, past 1080 x 2400 x 4 */

static const struct {
	uint8_t wait_ms;
	uint8_t length;
	uint8_t bytes[6];
} panel_120hz[] = {
	{ 0, 2, { 0x2f, 0x02 } },
	{ 0, 6, { 0xf0, 0x55, 0xaa, 0x52, 0x08, 0x00 } },
	{ 0, 2, { 0x6f, 0x09 } },
	{ 0, 2, { 0xc0, 0x20 } },
	{ 0, 6, { 0xf0, 0x55, 0xaa, 0x52, 0x08, 0x02 } },
	{ 0, 2, { 0xcc, 0x30 } },
	{ 10, 2, { 0xce, 0x01 } },
	{ 0, 6, { 0xf0, 0x55, 0xaa, 0x52, 0x08, 0x02 } },
	{ 0, 2, { 0xcc, 0x00 } },
};

static bool
panel_send(vm_offset_t dsi, vm_offset_t page, const uint8_t *bytes, unsigned length)
{
	uint32_t word[3] = { 0xffffffff, 0xffffffff, 0xffffffff };
	unsigned total = 4 + ((length + 3) & ~3U);

	/* Linux's dsi_cmd_dma_add: word count, the type (0x39, a long DCS write), last packet | long */
	wr(page, length | (0x39U << 16) | (0xc0U << 24));
	memcpy(word, bytes, length);
	for (unsigned i = 0; i < 3; i++) {
		wr(page + 4 + 4 * i, word[i]);
	}
	__asm__ volatile ("dsb sy");
	wr(dsi + DSI_INTR_CTRL, rd(dsi + DSI_INTR_CTRL) | DSI_DMA_DONE_STATUS);
	wr(dsi + DSI_DMA_BASE, (uint32_t)PANEL_DMA_PAGE);
	wr(dsi + DSI_DMA_LEN, total);
	__asm__ volatile ("dsb sy");
	wr(dsi + DSI_TRIG_DMA, 1);
	for (int tries = 0; tries < 200; tries++) {
		IODelay(100);
		if ((rd(dsi + DSI_STATUS0) & DSI_DMA_BUSY) == 0 && (rd(dsi + DSI_INTR_CTRL) & DSI_DMA_DONE_STATUS) != 0) {
			return true;
		}
	}
	return false;
}

static void
panel_switch(vm_offset_t intf)
{
	vm_offset_t dsi = ml_io_map(DSI_CTRL_BASE, 0x1000);
	vm_offset_t page = ml_io_map(PANEL_DMA_PAGE, 0x4000);
	uint32_t refresh = rd(intf + 0x2b4), dma = rd(dsi + DSI_CMD_DMA_CTRL), vsync = rd(intf + 0x288);
	unsigned sent = 0;

	printf("adreno: panel: dsi: status %#x, dma control %#x, trigger %#x, interrupts %#x\n", rd(dsi + DSI_STATUS0), dma,
	    rd(dsi + DSI_TRIG_CTRL), rd(dsi + DSI_INTR_CTRL));
	wr(intf + 0x2b4, refresh & ~(1U << 31));        /* no refresh under the commands: Linux stops autorefresh too */
	IOSleep(40);
	wr(dsi + DSI_CMD_DMA_CTRL, dma & ~DSI_LOW_POWER);
	for (; sent < sizeof(panel_120hz) / sizeof(panel_120hz[0]); sent++) {
		if (!panel_send(dsi, page, panel_120hz[sent].bytes, panel_120hz[sent].length)) {
			break;
		}
		if (panel_120hz[sent].wait_ms != 0) {
			IOSleep(panel_120hz[sent].wait_ms);
		}
	}
	wr(dsi + DSI_CMD_DMA_CTRL, dma);
	if (sent == sizeof(panel_120hz) / sizeof(panel_120hz[0])) {
		/* the clock lines are counted by, between tear signals: 19.2 MHz / (lines x rate) */
		wr(intf + 0x288, (vsync & ~0xffffU) | ((vsync & 0xffff) / 2));
	}
	printf("adreno: panel: %u of %lu commands sent for 120 Hz; dsi status %#x, interrupts %#x\n", sent,
	    sizeof(panel_120hz) / sizeof(panel_120hz[0]), rd(dsi + DSI_STATUS0), rd(dsi + DSI_INTR_CTRL));
	wr(intf + 0x2b4, refresh);
}

/*
 * The panel's brightness: DCS 0x51 with two bytes, big-endian (dt: qcom,mdss-dsi-bl-pmic-control-type "bl_ctrl_dcs",
 * mi,hbm-51-ctl-flag). 1 to 0x7ff is the normal range (mi,mdss-fac-bl-max-level); above, to 0xfff, is high brightness mode,
 * which is not asked for here (heat, and the panel's own limits). Sent like the 120 Hz commands, with the autorefresh held
 * for the moment it takes; a thread sends the last value asked for, so a slider dragged quickly costs one command per pass.
 * hakios_panel_brightness(permille): 0 to 1000 of the normal range; boot-arg panel_bright=N (permille) at start.
 */
static vm_offset_t panel_intf, panel_dsi, panel_page;
static volatile uint32_t panel_bright_wanted = UINT32_MAX, panel_bright_sent = UINT32_MAX;
static bool panel_bright_thread_up;

static uint32_t
panel_bright_level(uint32_t permille)
{
	uint32_t level;

	if (permille > 1000) {
		permille = 1000;
	}
	/* a curve, as the eye sees it: (permille / 1000)^2 of the range, never 0 (0 is the panel off) */
	level = (uint32_t)(((uint64_t)permille * permille * 0x7ff + 500000) / 1000000);
	return level < 1 ? 1 : level;
}

static void
panel_bright_send(uint32_t level)
{
	uint32_t refresh, dma;
	uint8_t command[3] = { 0x51, (uint8_t)(level >> 8), (uint8_t)level };
	bool sent;

	if (panel_intf == 0) {
		return;
	}
	refresh = rd(panel_intf + 0x2b4);
	dma = rd(panel_dsi + DSI_CMD_DMA_CTRL);
	wr(panel_intf + 0x2b4, refresh & ~(1U << 31));
	IOSleep(10);
	wr(panel_dsi + DSI_CMD_DMA_CTRL, dma & ~DSI_LOW_POWER);
	sent = panel_send(panel_dsi, panel_page, command, sizeof(command));
	wr(panel_dsi + DSI_CMD_DMA_CTRL, dma);
	wr(panel_intf + 0x2b4, refresh);
	if (!sent) {
		printf("adreno: panel: brightness %#x NOT sent (dsi status %#x)\n", level, rd(panel_dsi + DSI_STATUS0));
	}
}

static void
panel_bright_thread(__unused void *parameter, __unused wait_result_t result)
{
	for (;;) {
		uint32_t wanted = panel_bright_wanted;

		if (wanted != UINT32_MAX && wanted != panel_bright_sent) {
			panel_bright_sent = wanted;
			panel_bright_send(panel_bright_level(wanted));
		}
		IOSleep(30);
	}
}

void hakios_panel_brightness(uint32_t permille);
void
hakios_panel_brightness(uint32_t permille)
{
	static uint32_t told;

	panel_bright_wanted = permille > 1000 ? 1000 : permille;
	if (told++ < 20) {
		printf("adreno: panel: brightness %u/1000 asked (level %#x)\n", panel_bright_wanted, panel_bright_level(panel_bright_wanted));
	}
}

/* kern.hakios.panel_brightness: 0 to 1000, written by backboardd's hook (userland/src/inject.c) when iOS sets the brightness */
SYSCTL_DECL(_kern_hakios);
static int
panel_bright_sysctl(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int value = panel_bright_wanted == UINT32_MAX ? -1 : (int)panel_bright_wanted, error, changed = 0;

	error = sysctl_io_number(req, value, sizeof(value), &value, &changed);
	if (error == 0 && changed) {
		if (value < 0 || value > 1000) {
			return EINVAL;
		}
		hakios_panel_brightness((uint32_t)value);
	}
	return error;
}
SYSCTL_PROC(_kern_hakios, OID_AUTO, panel_brightness, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_ANYBODY | CTLFLAG_LOCKED, NULL, 0,
    panel_bright_sysctl, "I", "the panel's brightness, 0 to 1000 of its normal range");

static void
panel_bright_start(vm_offset_t intf)
{
	thread_t thread;
	uint32_t permille;

	panel_intf = intf;
	panel_dsi = ml_io_map(DSI_CTRL_BASE, 0x1000);
	panel_page = ml_io_map(PANEL_DMA_PAGE, 0x4000);
	if (!panel_bright_thread_up && kernel_thread_start(panel_bright_thread, NULL, &thread) == KERN_SUCCESS) {
		panel_bright_thread_up = true;
		thread_deallocate(thread);
	}
	if (PE_parse_boot_argn("panel_bright", &permille, sizeof(permille))) {
		hakios_panel_brightness(permille);
	}
}

static void
panel_probe(__unused void *parameter, __unused wait_result_t result)
{
	vm_offset_t b = ml_io_map(0x0ae36000, 0x1000);
	uint32_t every = 1, hz = 60;

	PE_parse_boot_argn("panel_every", &every, sizeof(every));
	PE_parse_boot_argn("panel_hz", &hz, sizeof(hz));
	panel_rate(b, "as ABL left it");
	if (every != 0) {
		wr(b + 0x2b4, (1U << 31) | (every & 0xffff));
		__asm__ volatile ("dsb sy");
		panel_rate(b, "after setting it");
	}
	if (hz == 120) {
		panel_switch(b);
		panel_rate(b, "after the switch to 120 Hz");
	}
	panel_bright_start(b);
	for (int round = 0; round < 3; round++) {
		IOSleep(15000);
		panel_rate(b, "later");
	}
	thread_terminate(current_thread());
}

/*
 * Boot-arg wifi_pcie=1: the first step towards Wi-Fi, reads only: what ABL leaves of the first PCIe root complex and of
 * the QCA6490's pins (dt: qcom,pcie@1c00000, qcom,cnss-qca6490). A read of a block without a clock resets the phone, so
 * PARF is read only when GCC says its power domain is on and its bus clock runs; nothing behind the PHY is read yet.
 * Here until there is a Wi-Fi driver of this phone's to hold it.
 */
/* dt: qcom,pcie@1c00000's qcom,phy-sequence: the PHY's register, its value, a wait in microseconds. */
static const struct { uint16_t offset; uint8_t value; uint8_t wait_us; } pcie0_phy_sequence[] = {
	{ 0x240, 0x3, 0 }, { 0xc0, 0x1, 0 }, { 0xcc, 0x31, 0 }, { 0xd0, 0x1, 0 }, { 0x60, 0xff, 0 }, { 0x64, 0x6, 0 },
	{ 0x0, 0x4c, 0 }, { 0x4, 0x6, 0 }, { 0xe0, 0x90, 0 }, { 0xe4, 0x82, 0 }, { 0xf4, 0x7, 0 }, { 0x70, 0x2, 0 },
	{ 0x10, 0x2, 0 }, { 0x74, 0x16, 0 }, { 0x14, 0x16, 0 }, { 0x78, 0x36, 0 }, { 0x18, 0x36, 0 },
	{ 0x110, 0x8, 0 }, { 0xbc, 0xe, 0 }, { 0x120, 0x42, 0 }, { 0x80, 0xa, 0 }, { 0x84, 0x1a, 0 },
	{ 0x20, 0x14, 0 }, { 0x24, 0x34, 0 }, { 0x88, 0x82, 0 }, { 0x28, 0x68, 0 }, { 0x90, 0xab, 0 },
	{ 0x94, 0xea, 0 }, { 0x98, 0x2, 0 }, { 0x30, 0xab, 0 }, { 0x34, 0xaa, 0 }, { 0x38, 0x2, 0 },
	{ 0x140, 0x14, 0 }, { 0x164, 0x34, 0 }, { 0x3c, 0x1, 0 }, { 0x1c, 0x4, 0 }, { 0x174, 0x16, 0 },
	{ 0x1bc, 0xf, 0 }, { 0x170, 0xa0, 0 }, { 0x11a4, 0x38, 0 }, { 0x10dc, 0x11, 0 }, { 0x1160, 0xbf, 0 },
	{ 0x1164, 0xbf, 0 }, { 0x1168, 0xb7, 0 }, { 0x116c, 0xea, 0 }, { 0x115c, 0x3f, 0 }, { 0x1174, 0x5c, 0 },
	{ 0x1178, 0x9c, 0 }, { 0x117c, 0x1a, 0 }, { 0x1180, 0x89, 0 }, { 0x1170, 0xdc, 0 }, { 0x1188, 0x94, 0 },
	{ 0x118c, 0x5b, 0 }, { 0x1190, 0x1a, 0 }, { 0x1194, 0x89, 0 }, { 0x10cc, 0xf0, 0 }, { 0x1008, 0x9, 0 },
	{ 0x1014, 0x5, 0 }, { 0x104c, 0x8, 0 }, { 0x1050, 0x8, 0 }, { 0x10d8, 0xf, 0 }, { 0x1118, 0x1c, 0 },
	{ 0x10f8, 0x7, 0 }, { 0x11f8, 0x8, 0 }, { 0xe84, 0x15, 0 }, { 0xe90, 0x3f, 0 }, { 0xee4, 0x2, 0 },
	{ 0xe40, 0x6, 0 }, { 0xe3c, 0x18, 0 }, { 0x2dc, 0x5, 0 }, { 0x388, 0x77, 0 }, { 0x398, 0xb, 0 },
	{ 0x3e0, 0xf, 0 }, { 0x60c, 0x1d, 0 }, { 0x614, 0x7, 0 }, { 0x620, 0xc1, 0 }, { 0x694, 0x0, 0 },
	{ 0x3d0, 0x8c, 0 }, { 0x1424, 0x1, 0 }, { 0x1428, 0x1, 0 }, { 0x200, 0x0, 0 }, { 0x244, 0x3, 0 },
};

static void
wifi_pin_out(uint32_t gpio, bool high)
{
	vm_offset_t pin = ml_io_map(0x0f100000 + 0x1000 * (vm_offset_t)gpio, 0x1000);

	wr(pin + 4, high ? 2 : 0);
	wr(pin, rd(pin) | 1U << 9);
	__asm__ volatile ("dsb sy");
}

static uint32_t
wifi_pin_in(uint32_t gpio)
{
	return rd(ml_io_map(0x0f100000 + 0x1000 * (vm_offset_t)gpio, 0x1000) + 4) & 1;
}

extern bool hakios_mayfly_vreg_on(const char *name, uint32_t millivolts, bool high_power);

/*
 * Step 4: the QCA6490 itself. Its supplies in the order and at the voltages of Android's cnss2 (dt: qcom,vdd-wlan-*-config,
 * dmesg's order), XO high while WLAN_EN rises, then PERST released and the link trained (Linux's dwc pcie-qcom).
 * What it shows: SW_CTRL high, the link at L0, and the chip's identity (17cb:1103) through an outbound window.
 * A configuration read is made only over a link that is up: without one it would hang the bus.
 */
static uint32_t wifi_deep;

#define WIFI_SMMU_TABLES        96      /* (ath11k's pool of 80 MB alone takes 40) */
#define WIFI_PTE                (3ULL | 1ULL << 6 | 2ULL << 8 | 1ULL << 10 | 1ULL << 11) /* a page, uncached, shared, accessed */
#define WIFI_ADDRESSES          0xa0000000ULL           /* dt: qcom,iommu-dma-addr-pool */
#define WIFI_SBL_SIZE           0x80000U                /* the part of the firmware BHI takes: 512 KB */
#define WIFI_MSI_DATA           0xa500U
static vm_offset_t wifi_tables, wifi_context;
static uint32_t wifi_bank, wifi_table_count = 1;

static uint64_t *
wifi_table_next(uint64_t *entry)
{
	uint64_t *table;

	if ((*entry & 1) != 0) {
		for (uint32_t i = 0; i < wifi_table_count; i++) {
			if (kvtophys(wifi_tables + 4096 * i) == (*entry & 0x0000fffffffff000ULL)) {
				return (uint64_t *)(wifi_tables + 4096 * i);
			}
		}
		return NULL;
	}
	if (wifi_table_count == WIFI_SMMU_TABLES) {
		return NULL;
	}
	table = (uint64_t *)(wifi_tables + 4096 * wifi_table_count++);
	*entry = kvtophys((vm_offset_t)table) | 3;
	return table;
}

/* Kernel memory, page by page, behind the chip's addresses: it need not be one piece in physical memory. */
static bool
wifi_show(uint64_t address, vm_offset_t memory, uint64_t size)
{
	for (uint64_t offset = 0; offset < size; offset += 4096) {
		uint64_t at = address + offset, *level2 = wifi_table_next(&((uint64_t *)wifi_tables)[at >> 30 & 0x1ff]);
		uint64_t *level3 = level2 == NULL ? NULL : wifi_table_next(&level2[at >> 21 & 0x1ff]);

		if (level3 == NULL) {
			return false;
		}
		level3[at >> 12 & 0x1ff] = (kvtophys(memory + offset) & ~0xfffULL) | WIFI_PTE;
	}
	for (vm_offset_t line = wifi_tables; line < wifi_tables + 4096 * WIFI_SMMU_TABLES; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	wr(wifi_context + 0x610, wifi_bank);
	wr(wifi_context + 0x7f0, 1);
	for (int i = 0; i < 100 && (rd(wifi_context + 0x7f4) & 1) != 0; i++) {
		IOSleep(1);
	}
	return true;
}

static uint8_t
wifi_crc8(uint32_t bdf)
{
	uint8_t bytes[2] = { (uint8_t)(bdf >> 8), (uint8_t)bdf }, crc = 0;

	for (unsigned i = 0; i < 2; i++) {
		crc ^= bytes[i];
		for (unsigned bit = 0; bit < 8; bit++) {
			crc = crc & 0x80 ? (uint8_t)(crc << 1 ^ 7) : (uint8_t)(crc << 1);
		}
	}
	return crc;
}

/*
 * Steps 8 and 9: the chip's firmware over MHI (the host's half of the protocol, as Linux's drivers/bus/mhi describes it).
 * Which stream its requests carry (Linux's qcom_pcie_config_sid_1_9_0, dt's iommu-map), one region of memory behind its
 * addresses for the contexts, rings and firmware, MSI on (the PBL/SBL wants it, though nobody takes the interrupt here:
 * it lands in a word of that memory), bus mastering on; BHI for the first 512 KB; then the contexts, event rings, M0,
 * BHIe for the whole image, and mission mode. Everything is polled. Memory is the kernel's, cached: written back before
 * the chip is told, and invalidated before it is read.
 */
#define WIFI_POOL_SIZE          0x700000U
#define WIFI_OFF_CHAN           0x0000U                 /* 128 channel contexts of 44 bytes */
#define WIFI_OFF_EVENT          0x1800U
#define WIFI_OFF_COMMAND        0x1900U
#define WIFI_OFF_RING_EV0       0x2000U                 /* 32 elements of 16 bytes */
#define WIFI_OFF_RING_EV1       0x3000U                 /* 256 */
#define WIFI_OFF_RING_COMMAND   0x4000U                 /* 128 */
#define WIFI_OFF_MSI            0x8000U
#define WIFI_OFF_FIRMWARE       0x10000U
#define WIFI_OFF_VECTOR         0x690000U
#define WIFI_SEGMENT            0x80000U

struct wifi_context {
	uint32_t word[3];
	uint32_t base[2], length[2], read[2], write[2];
} __attribute__((packed, aligned(4)));

static vm_offset_t wifi_pool, wifi_bar;
static uint32_t wifi_event_read[2], wifi_erdb;
static const uint32_t wifi_event_count[2] = { 32, 256 };
static const uint32_t wifi_event_at[2] = { WIFI_OFF_RING_EV0, WIFI_OFF_RING_EV1 };

static void
wifi_out(vm_offset_t address, uint64_t size)
{
	for (vm_offset_t line = address & ~63UL; line < address + size; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
}

static void
wifi_wr64(uint32_t offset, uint64_t value)
{
	wr(wifi_bar + offset + 4, (uint32_t)(value >> 32));
	wr(wifi_bar + offset, (uint32_t)value);
	__asm__ volatile ("dsb sy");
}

static void
wifi_context_ring(struct wifi_context *context, uint64_t base, uint64_t bytes, uint64_t write)
{
	context->base[0] = (uint32_t)base;
	context->base[1] = (uint32_t)(base >> 32);
	context->length[0] = (uint32_t)bytes;
	context->length[1] = (uint32_t)(bytes >> 32);
	context->read[0] = (uint32_t)base;
	context->read[1] = (uint32_t)(base >> 32);
	context->write[0] = (uint32_t)write;
	context->write[1] = (uint32_t)(write >> 32);
}

/*
 * Step 10: channels 20 and 21 (out and in), which carry QRTR (Linux: drivers/bus/mhi, net/qrtr/mhi.c; ath11k's channel
 * table). A hello answered, and everything the firmware then says of its services is printed.
 */
#define WIFI_OFF_RING_OUT       0x5000U
#define WIFI_OFF_RING_IN        0x6000U
#define WIFI_OFF_SEND           0x6a0000U
#define WIFI_OFF_RECEIVE        0x6a8000U
#define WIFI_RECEIVE_SIZE       0x4000U
#define WIFI_RECEIVE_COUNT      16U
#define WIFI_CHANNEL_RING       64U
struct wifi_qrtr_header {
	uint32_t version, type, source_node, source_port, confirm, size, destination_node, destination_port;
};
static uint32_t wifi_command_done = 0xff, wifi_chdb, wifi_out_write, wifi_in_write, wifi_peer, wifi_qrtr_said;
static uint32_t wifi_command_write;
static volatile uint32_t wifi_out_sent, wifi_out_done;
static thread_t wifi_poll_thread;
static LCK_GRP_DECLARE(wifi_group, "wifi");
static LCK_MTX_DECLARE(wifi_send_lock, &wifi_group);
/* the one port there is, for the QMI helpers (sys/hakios.qrtr.h) */
#define WIFI_OUR_PORT           0x4000U
struct hakios_qrtr_port {
	hakios_qrtr_receive_t receive;
	hakios_qrtr_server_t server;
	void *context;
	bool open, looked_up, told;
	uint32_t service, version, instance;
};
static struct hakios_qrtr_port wifi_port;
static struct {
	bool known;
	uint32_t service, word, node, port;
} wifi_service;

static void wifi_receive_give(uint32_t buffer);
static void wifi_channel_event(const uint32_t *element);

static void
wifi_ring_doorbell(uint32_t channel, uint32_t ring_offset, uint32_t write)
{
	struct wifi_context *context = (struct wifi_context *)(wifi_pool + WIFI_OFF_CHAN) + channel;
	uint64_t address = WIFI_ADDRESSES + ring_offset + 16ULL * write;

	context->write[0] = (uint32_t)address;
	context->write[1] = (uint32_t)(address >> 32);
	wifi_out((vm_offset_t)context, sizeof(*context));
	wifi_wr64(wifi_chdb + channel * 8, address);
}

static void
wifi_qrtr_send(uint32_t type, uint32_t source_port, uint32_t destination_node, uint32_t destination_port, const void *data, uint32_t bytes)
{
	uint32_t slot, padded = (bytes + 3) & ~3U, length = (uint32_t)sizeof(struct wifi_qrtr_header) + padded;
	struct wifi_qrtr_header *header;
	uint32_t *element;
	uint64_t address;

	lck_mtx_lock(&wifi_send_lock);
	/* a buffer of 8 KB for each of the last four: the chip reads it some time after the doorbell */
	while (current_thread() != wifi_poll_thread && wifi_out_sent - wifi_out_done >= 3) {
		IOSleep(1);
	}
	slot = wifi_out_write % 4;
	header = (struct wifi_qrtr_header *)(wifi_pool + WIFI_OFF_SEND + 0x2000 * slot);
	element = (uint32_t *)(wifi_pool + WIFI_OFF_RING_OUT + 16 * wifi_out_write);
	address = WIFI_ADDRESSES + WIFI_OFF_SEND + 0x2000ULL * slot;
	header->version = 1;
	header->type = type;
	header->source_node = 1;
	header->source_port = source_port;
	header->confirm = 0;
	header->size = bytes;
	header->destination_node = destination_node;
	header->destination_port = destination_port;
	memcpy(header + 1, data, bytes);
	wifi_out((vm_offset_t)header, length);
	element[0] = (uint32_t)address;
	element[1] = (uint32_t)(address >> 32);
	element[2] = length;
	element[3] = 2U << 16 | 1U << 9;                /* a transfer, an event when it is done */
	wifi_out((vm_offset_t)element, 16);
	wifi_out_write = (wifi_out_write + 1) % WIFI_CHANNEL_RING;
	wifi_out_sent++;
	wifi_ring_doorbell(20, WIFI_OFF_RING_OUT, wifi_out_write);
	lck_mtx_unlock(&wifi_send_lock);
}

static void
wifi_receive_give(uint32_t buffer)
{
	uint32_t *element = (uint32_t *)(wifi_pool + WIFI_OFF_RING_IN + 16 * wifi_in_write);
	uint64_t address = WIFI_ADDRESSES + WIFI_OFF_RECEIVE + (uint64_t)buffer * WIFI_RECEIVE_SIZE;

	element[0] = (uint32_t)address;
	element[1] = (uint32_t)(address >> 32);
	element[2] = WIFI_RECEIVE_SIZE;
	element[3] = 2U << 16 | 1U << 9;
	wifi_out((vm_offset_t)element, 16);
	wifi_in_write = (wifi_in_write + 1) % WIFI_CHANNEL_RING;
}

static void
wifi_channel_event(const uint32_t *element)
{
	uint32_t channel = element[3] >> 24, length = element[2] & 0xffff, code = element[2] >> 24;
	uint64_t ring_element = (uint64_t)element[0] | (uint64_t)element[1] << 32;

	if (channel == 20) {
		wifi_out_done++;
		return;
	}
	if (channel != 21) {
		return;
	}
	if (ring_element >= WIFI_ADDRESSES + WIFI_OFF_RING_IN && ring_element < WIFI_ADDRESSES + WIFI_OFF_RING_IN + 16ULL * WIFI_CHANNEL_RING) {
		uint32_t index = (uint32_t)((ring_element - WIFI_ADDRESSES - WIFI_OFF_RING_IN) / 16), buffer = index % WIFI_RECEIVE_COUNT;
		vm_offset_t data = wifi_pool + WIFI_OFF_RECEIVE + (vm_offset_t)buffer * WIFI_RECEIVE_SIZE;
		const struct wifi_qrtr_header *header = (const struct wifi_qrtr_header *)data;
		const uint32_t *control = (const uint32_t *)(header + 1);

		wifi_out(data, WIFI_RECEIVE_SIZE);
		if (wifi_qrtr_said++ < 60) {
			printf("wifi: qrtr: %u bytes (code %#x): version %u, type %u, from node %u port %#x, %u bytes to node %u port %#x; %#x %#x %#x %#x %#x\n",
			    length, code, header->version, header->type, header->source_node, header->source_port, header->size,
			    header->destination_node, header->destination_port, control[0], control[1], control[2], control[3], control[4]);
		}
		if (header->version == 1 && header->type == 2) {
			uint32_t hello[5] = { 2, 0, 0, 0, 0 };

			wifi_peer = header->source_node;
			wifi_qrtr_send(2, 0xfffffffe, wifi_peer, 0xfffffffe, hello, sizeof(hello));
		} else if (header->version == 1 && header->type == 4) {
			wifi_service.service = control[1];
			wifi_service.word = control[2];
			wifi_service.node = control[3];
			wifi_service.port = control[4];
			__asm__ volatile ("dmb sy");
			wifi_service.known = true;
		} else if (header->version == 1 && header->type == 1 && header->destination_port == WIFI_OUR_PORT &&
		    wifi_port.open && wifi_port.receive != NULL) {
			wifi_port.receive(wifi_port.context, header->source_node, header->source_port, header + 1, header->size);
			if (header->confirm != 0) {
				uint32_t resume[5] = { 7, 1, WIFI_OUR_PORT, 0, 0 };

				wifi_qrtr_send(7, WIFI_OUR_PORT, header->source_node, header->source_port, resume, sizeof(resume));
			}
		}
		wifi_receive_give(buffer);
		wifi_ring_doorbell(21, WIFI_OFF_RING_IN, wifi_in_write);
	}
}

/* What the chip has said on its event rings: state changes and execution environments. Each element goes back to it. */
static void
wifi_events(void)
{
	for (uint32_t ring = 0; ring < 2; ring++) {
		struct wifi_context *context = (struct wifi_context *)(wifi_pool + WIFI_OFF_EVENT) + ring;
		uint64_t base = WIFI_ADDRESSES + wifi_event_at[ring], read, last;
		uint32_t index, moved = 0;

		wifi_out((vm_offset_t)context, sizeof(*context));
		read = (uint64_t)context->read[0] | (uint64_t)context->read[1] << 32;
		if (read < base || read >= base + wifi_event_count[ring] * 16ULL) {
			continue;
		}
		index = (uint32_t)((read - base) / 16);
		wifi_out(wifi_pool + wifi_event_at[ring], wifi_event_count[ring] * 16ULL);
		while (wifi_event_read[ring] != index) {
			const uint32_t *element = (const uint32_t *)(wifi_pool + wifi_event_at[ring] + 16 * wifi_event_read[ring]);
			uint32_t type = element[3] >> 16 & 0xff, code = element[2] >> 24 & 0xff;

			if (type == 0x21) {
				wifi_command_done = code;
			} else if (type == 0x22) {
				wifi_channel_event(element);
			} else {
				printf("wifi: mhi: event ring %u: type %#x, code %#x\n", ring, type, code);
			}
			wifi_event_read[ring] = (wifi_event_read[ring] + 1) % wifi_event_count[ring];
			moved++;
		}
		if (moved != 0) {
			last = base + 16ULL * ((wifi_event_read[ring] + wifi_event_count[ring] - 1) % wifi_event_count[ring]);
			context->write[0] = (uint32_t)last;
			context->write[1] = (uint32_t)(last >> 32);
			wifi_out((vm_offset_t)context, sizeof(*context));
			wifi_wr64(wifi_erdb + ring * 8, last);
		}
	}
}

/* ---- QRTR for the QMI helpers (sys/hakios.qrtr.h) ---- */
static struct hakios_qrtr_port *
wifi_qrtr_open(hakios_qrtr_receive_t receive, hakios_qrtr_server_t server, void *context)
{
	if (wifi_port.open) {
		return NULL;
	}
	wifi_port.receive = receive;
	wifi_port.server = server;
	wifi_port.context = context;
	wifi_port.open = true;
	return &wifi_port;
}

static int
wifi_qrtr_data(__unused struct hakios_qrtr_port *port, uint32_t node, uint32_t to_port, const void *data, size_t length)
{
	if (length == 0 || length > 0x2000 - sizeof(struct wifi_qrtr_header) - 4) {
		return EMSGSIZE;
	}
	wifi_qrtr_send(1, WIFI_OUR_PORT, node, to_port, data, (uint32_t)length);
	return 0;
}

static int
wifi_qrtr_lookup(struct hakios_qrtr_port *port, uint32_t service, uint32_t version, uint32_t instance)
{
	port->service = service;
	port->version = version;
	port->instance = instance;
	__asm__ volatile ("dmb sy");
	port->looked_up = true;
	return 0;
}

static void
wifi_qrtr_close(struct hakios_qrtr_port *port)
{
	port->open = false;
}

static const struct hakios_qrtr_ops wifi_qrtr_ops = { wifi_qrtr_open, wifi_qrtr_data, wifi_qrtr_lookup, wifi_qrtr_close };

bool
hakios_mayfly_wifi_memory(uint64_t address, vm_offset_t memory, uint64_t size)
{
	return wifi_show(address, memory, size);
}

/* Physical memory in one piece behind the chip's addresses (ath11k's pool: hakios.lx.c, lx_os_radio_memory()). */
static bool wifi_map_physical(uint64_t address, uint64_t physical, uint64_t size, uint64_t pte);

bool
hakios_mayfly_wifi_map_physical(uint64_t address, uint64_t physical, uint64_t size)
{
	return wifi_map_physical(address, physical, size, WIFI_PTE);
}

/* A device's registers behind a chip's address: the GIC's, for the radio's MSI (as the redwood's RADIO_PTE_DEVICE) */
bool
hakios_mayfly_wifi_map_device(uint64_t address, uint64_t physical, uint64_t size)
{
	return wifi_map_physical(address, physical, size, 3ULL | 2ULL << 2 | 1ULL << 6 | 1ULL << 10 | 1ULL << 11 | 1ULL << 53 | 1ULL << 54);
}

static bool
wifi_map_physical(uint64_t address, uint64_t physical, uint64_t size, uint64_t pte)
{
	for (uint64_t offset = 0; offset < size; offset += 4096) {
		uint64_t at = address + offset, *level2 = wifi_table_next(&((uint64_t *)wifi_tables)[at >> 30 & 0x1ff]);
		uint64_t *level3 = level2 == NULL ? NULL : wifi_table_next(&level2[at >> 21 & 0x1ff]);

		if (level3 == NULL) {
			return false;
		}
		level3[at >> 12 & 0x1ff] = ((physical + offset) & 0x0000fffffffff000ULL) | pte;
	}
	for (vm_offset_t line = wifi_tables; line < wifi_tables + 4096 * WIFI_SMMU_TABLES; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	wr(wifi_context + 0x610, wifi_bank);
	wr(wifi_context + 0x7f0, 1);
	for (int i = 0; i < 100 && (rd(wifi_context + 0x7f4) & 1) != 0; i++) {
		IOSleep(1);
	}
	return true;
}

/* MHI is in mission mode and QRTR's channels are open: the firmware can be spoken to (ath11k's power_up). */
static volatile bool wifi_link_up;

bool
hakios_mayfly_wifi_up(void)
{
	return wifi_link_up;
}

static void
wifi_talk(__unused void *parameter, __unused wait_result_t result)
{
	extern void hakios_mayfly_wlfw(void);
	extern int hakios_lx_start(void);
	uint32_t lx = 0;

	/* boot-arg wifi_lx=2: Linux's ath11k itself (hakios.lx.c and its object) instead of our stand-in for its qmi.c */
	if (PE_parse_boot_argn("wifi_lx", &lx, sizeof(lx)) && lx != 0) {
		int error;

		printf("wifi: lx: ath11k starts (wifi_lx=%u)\n", lx);
		error = hakios_lx_start();
		printf("wifi: lx: ath11k's start returned %d\n", error);
	} else {
		hakios_mayfly_wlfw();
	}
	thread_terminate(current_thread());
}

static void
wifi_first_firmware(vm_offset_t parf, vm_offset_t dbi, vm_offset_t config, vm_offset_t bar, uint32_t bhi)
{
	extern const uint8_t *hakios_image_file(const char *directory, const char *name, uint64_t *size, int *disk);
	const uint8_t *firmware = NULL;
	uint64_t size = 0;
	int disk = -1;
	uint32_t status = 0, session, entries;
	uint32_t bhie;
	uint64_t *vector;

	wifi_bar = bar;
	for (int i = 0; i < 100 && (firmware = hakios_image_file("fw", "amss20.bin", &size, &disk)) == NULL; i++) {
		IOSleep(100);
	}
	if (firmware == NULL || size < WIFI_SEGMENT || WIFI_OFF_FIRMWARE + size > WIFI_OFF_VECTOR) {
		printf("wifi: firmware: FAILED: no fw/amss20.bin of a size that fits in the small image (%llu bytes)\n", size);
		return;
	}
	if (kmem_alloc(kernel_map, &wifi_pool, round_page(WIFI_POOL_SIZE), KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("wifi: firmware: FAILED: no memory\n");
		return;
	}
	memcpy((void *)(wifi_pool + WIFI_OFF_FIRMWARE), firmware, size);
	vector = (uint64_t *)(wifi_pool + WIFI_OFF_VECTOR);
	entries = (uint32_t)((size + WIFI_SEGMENT - 1) / WIFI_SEGMENT);
	for (uint32_t i = 0; i < entries; i++) {
		uint64_t left = size - (uint64_t)i * WIFI_SEGMENT;

		vector[2 * i] = WIFI_ADDRESSES + WIFI_OFF_FIRMWARE + (uint64_t)i * WIFI_SEGMENT;
		vector[2 * i + 1] = left < WIFI_SEGMENT ? left : WIFI_SEGMENT;
	}
	wifi_out(wifi_pool, WIFI_POOL_SIZE);
	if (!wifi_show(WIFI_ADDRESSES, wifi_pool, WIFI_POOL_SIZE)) {
		printf("wifi: firmware: FAILED: no page table left\n");
		return;
	}
	printf("wifi: firmware: %llu bytes, %u segments; first word %#x; shown at %#llx\n", size, entries, *(const uint32_t *)firmware, WIFI_ADDRESSES);
	/* streams */
	wr(parf + 0x2c00, rd(parf + 0x2c00) & ~1U);
	for (uint32_t i = 0; i < 256; i++) {
		wr(parf + 0x2000 + 4 * i, 0);
	}
	wr(parf + 0x2000 + 4 * (uint32_t)wifi_crc8(0x000), 0x000U << 16 | 0U << 8);
	wr(parf + 0x2000 + 4 * (uint32_t)wifi_crc8(0x100), 0x100U << 16 | 1U << 8);
	/* MSI: capability at 0x50, 32-bit address, a mask per vector */
	wr(config + 0x54, (uint32_t)(WIFI_ADDRESSES + WIFI_OFF_MSI));
	wr(config + 0x58, WIFI_MSI_DATA);
	wr(config + 0x5c, 0);
	wr(config + 0x50, (rd(config + 0x50) & ~(7U << 20)) | 5U << 20 | 1U << 16);
	wr(dbi + 0x04, rd(dbi + 0x04) | 4);
	wr(config + 0x04, rd(config + 0x04) | 4);
	__asm__ volatile ("dsb sy");
	session = ((uint32_t)mach_absolute_time() & 0x3fffffff) | 1;
	wr(bar + bhi + 0x20, 0);
	wr(bar + bhi + 0x2c, 0);
	wr(bar + bhi + 0x0c, (uint32_t)((WIFI_ADDRESSES + WIFI_OFF_FIRMWARE) >> 32));
	wr(bar + bhi + 0x08, (uint32_t)(WIFI_ADDRESSES + WIFI_OFF_FIRMWARE));
	wr(bar + bhi + 0x10, WIFI_SEGMENT);
	wr(bar + bhi + 0x18, session);
	__asm__ volatile ("dsb sy");
	for (int ms = 0; ms < 3000 && (status = rd(bar + bhi + 0x2c)) >> 30 < 2; ms += 10) {
		IOSleep(10);
	}
	printf("wifi: firmware: BHI: status %#x error %#x, EE %#x; MHI status %#x\n", status, rd(bar + bhi + 0x30), rd(bar + bhi + 0x28), rd(bar + 0x48));
	if (status >> 30 != 2) {
		printf("wifi: firmware: FAILED: BHI\n");
		return;
	}
	for (int ms = 0; ms < 3000 && (rd(bar + 0x48) & 1) == 0; ms += 10) {
		IOSleep(10);
	}
	if (wifi_deep < 9) {
		printf("wifi: firmware: end of step 8\n");
		return;
	}
	/* Step 9: contexts (channels stay disabled), event rings, M0 */
	{
		uint32_t chdb = rd(bar + 0x18);
		struct wifi_context *events = (struct wifi_context *)(wifi_pool + WIFI_OFF_EVENT);
		struct wifi_context *command = (struct wifi_context *)(wifi_pool + WIFI_OFF_COMMAND);
		uint64_t pool_end = WIFI_ADDRESSES + 0x10000000 - 1;   /* dt: all the chip's addresses, its own memory and m3 too */

		wifi_erdb = rd(bar + 0x20);
		bhie = rd(bar + 0x2c);
		printf("wifi: mhi: channel doorbells at %#x, event doorbells at %#x, BHIe at %#x\n", chdb, wifi_erdb, bhie);
		if (chdb >= 0x10000 - 8 * 128 || wifi_erdb >= 0x10000 - 16 || bhie >= 0x10000 - 0x80) {
			printf("wifi: mhi: FAILED: a doorbell outside what is mapped\n");
			return;
		}
		for (uint32_t ring = 0; ring < 2; ring++) {
			uint64_t base = WIFI_ADDRESSES + wifi_event_at[ring];

			events[ring].word[0] = 0;
			events[ring].word[1] = 1;
			events[ring].word[2] = 0;
			wifi_context_ring(&events[ring], base, wifi_event_count[ring] * 16ULL, base + 16ULL * (wifi_event_count[ring] - 1));
		}
		wifi_context_ring(command, WIFI_ADDRESSES + WIFI_OFF_RING_COMMAND, 128 * 16ULL, WIFI_ADDRESSES + WIFI_OFF_RING_COMMAND);
		wifi_out(wifi_pool, 0x8000);
		wifi_wr64(chdb + 127 * 8, 0);                   /* device wake off */
		wifi_wr64(0x58, WIFI_ADDRESSES + WIFI_OFF_CHAN);
		wifi_wr64(0x60, WIFI_ADDRESSES + WIFI_OFF_EVENT);
		wifi_wr64(0x68, WIFI_ADDRESSES + WIFI_OFF_COMMAND);
		wr(bar + 0x10, (rd(bar + 0x10) & 0xffff) | 2U << 16);   /* two event rings, none of them hardware's */
		wifi_wr64(0x80, WIFI_ADDRESSES);
		wifi_wr64(0x98, WIFI_ADDRESSES);
		wifi_wr64(0x88, pool_end);
		wifi_wr64(0xa0, pool_end);
		for (uint32_t ring = 0; ring < 2; ring++) {
			wifi_wr64(wifi_erdb + ring * 8, WIFI_ADDRESSES + wifi_event_at[ring] + 16ULL * (wifi_event_count[ring] - 1));
		}
		wr(bar + 0x38, (rd(bar + 0x38) & ~(0xffU << 8)) | 2U << 8);
		__asm__ volatile ("dsb sy");
		for (int ms = 0; ms < 5000 && (rd(bar + 0x48) >> 8 & 0xff) != 2; ms += 10) {
			wifi_events();
			IOSleep(10);
		}
		wifi_events();
		printf("wifi: mhi: control %#x, status %#x (state %u), EE %#x\n", rd(bar + 0x38), rd(bar + 0x48), rd(bar + 0x48) >> 8 & 0xff, rd(bar + bhi + 0x28));
		if ((rd(bar + 0x48) >> 8 & 0xff) != 2) {
			printf("wifi: mhi: FAILED: never M0\n");
			return;
		}
		wifi_wr64(chdb + 127 * 8, 1);                   /* device wake on */
		/* the whole image */
		wr(bar + bhie + 0x30, (uint32_t)((WIFI_ADDRESSES + WIFI_OFF_VECTOR) >> 32));
		wr(bar + bhie + 0x2c, (uint32_t)(WIFI_ADDRESSES + WIFI_OFF_VECTOR));
		wr(bar + bhie + 0x34, entries * 16);
		__asm__ volatile ("dsb sy");
		wr(bar + bhie + 0x3c, session);
		__asm__ volatile ("dsb sy");
		printf("wifi: mhi: BHIe told to fetch %u segments\n", entries);
		for (int ms = 0; ms < 20000; ms += 50) {
			wifi_events();
			status = rd(bar + bhie + 0x44);
			if (status >> 30 >= 2) {
				break;
			}
			IOSleep(50);
		}
		printf("wifi: mhi: BHIe status %#x; EE %#x; MHI status %#x\n", status, rd(bar + bhi + 0x28), rd(bar + 0x48));
		for (int ms = 0; ms < 20000 && rd(bar + bhi + 0x28) != 2; ms += 50) {
			wifi_events();
			IOSleep(50);
		}
		wifi_events();
		printf("wifi: mhi: %s: EE %#x; MHI status %#x; MSI word %#x; fsr %#x\n", rd(bar + bhi + 0x28) == 2 ? "MISSION MODE" : "not in mission mode",
		    rd(bar + bhi + 0x28), rd(bar + 0x48), *(volatile uint32_t *)(wifi_pool + WIFI_OFF_MSI), rd(wifi_context + 0x58));
	}
	if (wifi_deep >= 10 && rd(bar + bhi + 0x28) == 2) {
		wifi_chdb = rd(bar + 0x18);
		for (uint32_t i = 0; i < 2; i++) {
			uint32_t channel = 20 + i, ring_at = i == 0 ? WIFI_OFF_RING_OUT : WIFI_OFF_RING_IN;
			struct wifi_context *context = (struct wifi_context *)(wifi_pool + WIFI_OFF_CHAN) + channel;
			uint32_t *command = (uint32_t *)(wifi_pool + WIFI_OFF_RING_COMMAND + 16 * wifi_command_write);
			uint64_t base = WIFI_ADDRESSES + ring_at, write;

			context->word[0] = 1 | 2U << 8;         /* enabled; no doorbell bursts */
			context->word[1] = i == 0 ? 1 : 2;      /* to the device, from it */
			context->word[2] = 1;                   /* events on ring 1 */
			wifi_context_ring(context, base, WIFI_CHANNEL_RING * 16ULL, base);
			wifi_out((vm_offset_t)context, sizeof(*context));
			command[0] = command[1] = command[2] = 0;
			command[3] = channel << 24 | 0x12U << 16;       /* start channel */
			wifi_out((vm_offset_t)command, 16);
			wifi_command_write = (wifi_command_write + 1) % 128;
			write = WIFI_ADDRESSES + WIFI_OFF_RING_COMMAND + 16ULL * wifi_command_write;
			((struct wifi_context *)(wifi_pool + WIFI_OFF_COMMAND))->write[0] = (uint32_t)write;
			((struct wifi_context *)(wifi_pool + WIFI_OFF_COMMAND))->write[1] = (uint32_t)(write >> 32);
			wifi_out(wifi_pool + WIFI_OFF_COMMAND, sizeof(struct wifi_context));
			wifi_command_done = 0xff;
			wifi_wr64(0x70, write);
			for (int ms = 0; ms < 3000 && wifi_command_done == 0xff; ms += 10) {
				wifi_events();
				IOSleep(10);
			}
			printf("wifi: mhi: channel %u started: completion code %#x\n", channel, wifi_command_done);
			if (wifi_command_done != 1) {
				printf("wifi: mhi: FAILED: channel %u\n", channel);
				return;
			}
		}
		for (uint32_t i = 0; i < WIFI_RECEIVE_COUNT; i++) {
			wifi_receive_give(i);
		}
		wifi_ring_doorbell(21, WIFI_OFF_RING_IN, wifi_in_write);
		wifi_poll_thread = current_thread();
		if (wifi_deep >= 11) {
			thread_t talk;

			hakios_qmi_set_transport(&wifi_qrtr_ops);
			wifi_link_up = true;
			if (kernel_thread_start(wifi_talk, NULL, &talk) == KERN_SUCCESS) {
				thread_deallocate(talk);
			}
			/* from here on this thread is the chip's: it never ends while the run lasts */
			for (uint32_t turn = 0, fsr_said = rd(wifi_context + 0x58);; turn++) {
				wifi_events();
				if ((turn & 255) == 0 && rd(wifi_context + 0x58) != fsr_said) {
					fsr_said = rd(wifi_context + 0x58);
					printf("wifi: smmu: FAULT of the chip: fsr %#x, address %#x%08x, fsynr0 %#x\n", fsr_said, rd(wifi_context + 0x64),
					    rd(wifi_context + 0x60), rd(wifi_context + 0x68));
				}
				if (wifi_service.known && wifi_port.looked_up && !wifi_port.told && wifi_port.server != NULL &&
				    wifi_service.service == wifi_port.service) {
					wifi_port.told = true;
					wifi_port.server(wifi_port.context, true, wifi_service.service, wifi_service.word & 0xff, wifi_service.word >> 8,
					    wifi_service.node, wifi_service.port);
				}
				IOSleep(2);
			}
		}
		for (int ms = 0; ms < 15000; ms += 20) {
			wifi_events();
			IOSleep(20);
		}
		printf("wifi: qrtr: %u messages; the peer is node %u\n", wifi_qrtr_said, wifi_peer);
		printf("wifi: firmware: end of step 10\n");
		return;
	}
	printf("wifi: firmware: end of step 9\n");
}

#define WIFI_SMMU_SCTLR_ON      (1U << 0 | 1U << 1 | 1U << 2 | 1U << 5 | 1U << 8 | 1U << 12)
#define WIFI_SMMU_TCR           (25U | 1U << 8 | 1U << 10 | 3U << 12 | 1U << 23)
#define WIFI_SMMU_TCR2          (1U | 1U << 4 | 7U << 15)
#define WIFI_SMMU_MAIR0         0xf404ff44
#define WIFI_SMMU_FSR_ALL       0xc00001fe
#define WIFI_SMMU_CBAR_STAGE_1  (1U << 16 | 3U << 8 | 0xfU << 12)

/*
 * Step 7: the first writes to the application processors' SMMU, which the hypervisor answers for: a free match register
 * for stream 0x1c01 and a free context with page tables of ours, empty yet. Order and formats as the redwood's
 * radio_space_on() (drivers/wifi/hakios.wifi.c), where the hypervisor grants them. Each is read back. The chip is not
 * allowed to master the bus yet, so it asks nothing of this: the question here is only whether the writes are taken.
 */
static void
wifi_smmu_claim(void)
{
	vm_offset_t smmu = ml_io_map(0x15000000, 0x2000), tables = 0, context;
	uint32_t matches = rd(smmu + 0x20) & 0xff, contexts = rd(smmu + 0x24) & 0xff, match = ~0U, bank = ~0U;
	bool used[256];
	uint64_t root;

	bzero(used, sizeof(used));
	for (uint32_t n = 0; n < matches; n++) {
		uint32_t smr = rd(smmu + 0x800 + 4 * n), s2cr = rd(smmu + 0xc00 + 4 * n);

		if (smr >> 31) {
			printf("wifi: smmu: match %u in use: stream %#x mask %#x, s2cr %#x\n", n, smr & 0x7fff, smr >> 16 & 0x7fff, s2cr);
			used[s2cr & 0xff] = true;
		} else if (match == ~0U) {
			match = n;
		}
	}
	/* from the top, below what the boot chain and the loader's USB use: the low ones are likeliest Android's to come */
	for (uint32_t c = contexts; c-- > 1;) {
		if (!used[c] && (rd(ml_io_map(0x15080000 + c * 0x1000ULL, 0x1000)) & 1) == 0) {
			bank = c;
			break;
		}
	}
	if (match == ~0U || bank == ~0U) {
		printf("wifi: smmu: no match register (%u) or no context (%u) free\n", match, bank);
		return;
	}
	context = ml_io_map(0x15080000 + bank * 0x1000ULL, 0x1000);
	if (kmem_alloc(kernel_map, &tables, round_page(4096 * WIFI_SMMU_TABLES), KMA_KOBJECT | KMA_ZERO | KMA_PERMANENT,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("wifi: smmu: no memory for page tables\n");
		return;
	}
	for (vm_offset_t line = tables; line < tables + 4096 * WIFI_SMMU_TABLES; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	root = kvtophys(tables) | (uint64_t)bank << 48;
	wifi_tables = tables;
	wifi_context = context;
	wifi_bank = bank;
	printf("wifi: smmu: ABOUT TO WRITE: match %u, context %u (cbar %#x, cba2r %#x, sctlr %#x), root %#llx\n", match, bank,
	    rd(smmu + 0x1000 + 4 * bank), rd(smmu + 0x1800 + 4 * bank), rd(context), root);
	IOSleep(2500);
	wr(context + 0x00, 0);
	wr(smmu + 0x1800 + 4 * bank, 1);
	wr(smmu + 0x1000 + 4 * bank, WIFI_SMMU_CBAR_STAGE_1);
	printf("wifi: smmu: context %u type taken: cbar %#x, cba2r %#x\n", bank, rd(smmu + 0x1000 + 4 * bank), rd(smmu + 0x1800 + 4 * bank));
	IOSleep(2500);
	wr(context + 0x10, WIFI_SMMU_TCR2);
	wr(context + 0x30, WIFI_SMMU_TCR);
	wr(context + 0x20, (uint32_t)root);
	wr(context + 0x24, (uint32_t)(root >> 32));
	wr(context + 0x28, 0);
	wr(context + 0x2c, 0);
	wr(context + 0x38, WIFI_SMMU_MAIR0);
	wr(context + 0x3c, 0);
	wr(context + 0x58, WIFI_SMMU_FSR_ALL);
	wr(context + 0x610, bank);
	wr(context + 0x7f0, 1);
	for (int i = 0; i < 100 && (rd(context + 0x7f4) & 1) != 0; i++) {
		IOSleep(1);
	}
	wr(context + 0x00, WIFI_SMMU_SCTLR_ON);
	wr(smmu + 0xc00 + 4 * match, bank);
	wr(smmu + 0x800 + 4 * match, 1U << 31 | 0x1c01);
	__asm__ volatile ("dsb sy");
	printf("wifi: smmu: DONE: smr %#x, s2cr %#x, sctlr %#x, ttbr0 %#x %#x, fsr %#x, global fault %#x\n", rd(smmu + 0x800 + 4 * match),
	    rd(smmu + 0xc00 + 4 * match), rd(context), rd(context + 0x24), rd(context + 0x20), rd(context + 0x58), rd(smmu + 0x48));
}

/*
 * Step 6: the application processors' SMMU, read only: the chip's DMA comes in as stream 0x1c01 (dt: iommu-map; RID 0x100).
 * Which match registers and contexts the boot chain uses, and whether anything covers that stream. Registers as in
 * drivers/wifi/hakios.wifi.c's smmu_look(), where the same block works for the redwood's radio.
 */
static void
wifi_smmu_look(void)
{
	vm_offset_t smmu = ml_io_map(0x15000000, 0x4000);
	uint32_t scr0 = rd(smmu), idr0 = rd(smmu + 0x20), idr1 = rd(smmu + 0x24), matches = idr0 & 0xff, valid = 0;
	uint32_t wide = scr0 & 1U << 3, first_free = 0xffffffff, highest_context = 0;

	printf("wifi: smmu: scr0 %#x, idr0 %#x, idr1 %#x: %u match registers, %u contexts, %u of them stage 2 only; unmatched streams %s\n",
	    scr0, idr0, idr1, matches, idr1 & 0xff, idr1 >> 16 & 0xff, scr0 & 1U << 10 ? "fault" : "pass");
	for (uint32_t n = 0; n < matches && n < 256; n++) {
		uint32_t smr = rd(smmu + 0x800 + 4 * n), s2cr = rd(smmu + 0xc00 + 4 * n);
		uint32_t on = wide ? s2cr & 1U << 10 : smr >> 31, stream = smr & (wide ? 0xffff : 0x7fff), mask = smr >> 16 & (wide ? 0xffff : 0x7fff);

		if (!on) {
			if (first_free == 0xffffffff) {
				first_free = n;
			}
			continue;
		}
		valid++;
		if ((s2cr & 0xff) > highest_context && (s2cr >> 16 & 3) == 0) {
			highest_context = s2cr & 0xff;
		}
		if (((stream ^ 0x1c01) & ~mask) == 0) {
			printf("wifi: smmu: match %u covers the chip: stream %#x mask %#x, s2cr %#x\n", n, stream, mask, s2cr);
		}
	}
	printf("wifi: smmu: %u matches in use, first free %u, highest context in use %u\n", valid, first_free, highest_context);
}

static void
wifi_chip_and_link(vm_offset_t gcc, vm_offset_t parf, vm_offset_t dbi)
{
	static const struct { const char *what, *name; uint32_t millivolts; } rails[] = {
		{ "io", "smpb10", 1800 }, { "antenna switch", "ldoe7", 2800 }, { "aon", "smpe2", 1012 }, { "dig", "smpb11", 966 },
		{ "rfa1", "smpc1", 1900 }, { "rfa2", "smpb12", 1350 },
	};
	uint32_t state = 0, status = 0;

	(void)gcc;
	for (unsigned i = 0; i < sizeof(rails) / sizeof(rails[0]); i++) {
		bool done = hakios_mayfly_vreg_on(rails[i].name, rails[i].millivolts, false);

		printf("wifi: chip: supply %s (%s) at %u mV: %s\n", rails[i].what, rails[i].name, rails[i].millivolts, done ? "asked for" : "FAILED");
		if (!done) {
			return;
		}
	}
	IOSleep(10);
	{
		uint32_t qcabt = 0;

		PE_parse_boot_argn("qcabt", &qcabt, sizeof(qcabt));
		if (qcabt == 0) {
			wifi_pin_out(81, false);                /* BT_EN: Bluetooth stays off (hakios.qca6490bt.c has it otherwise) */
		}
	}
	wifi_pin_out(204, true);                                /* XO_CLK */
	IOSleep(1);
	wifi_pin_out(80, true);                                 /* WLAN_EN */
	IOSleep(20);
	wifi_pin_out(204, false);
	IOSleep(100);
	printf("wifi: chip: WLAN_EN high; SW_CTRL now %u\n", wifi_pin_in(82));
	wifi_pin_out(94, true);                                 /* PERST released */
	IOSleep(10);
	wr(parf + 0x1b0, rd(parf + 0x1b0) | 1U << 8);           /* link training on */
	for (int i = 0; i < 500; i++) {
		state = rd(parf + 0x1b0) & 0x3f;
		status = rd(dbi + 0x80);
		if ((status & (1U << 29)) != 0) {
			break;
		}
		IOSleep(1);
	}
	printf("wifi: link: state %#x, link status %#x: %s, generation %u, width x%u\n", state, status,
	    (status & (1U << 29)) != 0 ? "UP" : "not up", status >> 16 & 0xf, status >> 20 & 0x3f);
	if ((status & (1U << 29)) != 0) {
		vm_offset_t atu = ml_io_map(0x60001000, 0x1000), config = ml_io_map(0x60100000, 0x1000);

		wr(dbi + 0x18, 0x00010100);                     /* buses: this one 0, behind it 1 */
		wr(atu + 0x00, 4);                              /* region 0: type 0 configuration */
		wr(atu + 0x08, 0x60100000);
		wr(atu + 0x0c, 0);
		wr(atu + 0x10, 0x601fffff);
		wr(atu + 0x14, 1U << 24);                       /* bus 1, device 0 */
		wr(atu + 0x18, 0);
		wr(atu + 0x04, 1U << 31);
		__asm__ volatile ("dsb sy");
		IODelay(100);
		printf("wifi: link: the chip says %#x; class %#x\n", rd(config), rd(config + 8));
		if (wifi_deep >= 5 && rd(config) == 0x110317cb) {
			/*
			 * Step 5: the chip's registers, read only: BAR0 at 0x60400000 (2 MB, 64-bit, inside dt's window), a second
			 * outbound region for memory, the bridge's window, memory decoding on. Bus mastering stays off: no DMA yet.
			 */
			vm_offset_t bar;
			uint32_t bhi;

			printf("wifi: chip: BAR0 before %#x %#x; command %#x\n", rd(config + 0x10), rd(config + 0x14), rd(config + 4));
			wr(atu + 0x200 + 0x00, 0);
			wr(atu + 0x200 + 0x08, 0x60300000);
			wr(atu + 0x200 + 0x0c, 0);
			wr(atu + 0x200 + 0x10, 0x63ffffff);
			wr(atu + 0x200 + 0x14, 0x60300000);
			wr(atu + 0x200 + 0x18, 0);
			wr(atu + 0x200 + 0x04, 1U << 31);
			wr(dbi + 0x20, 0x60506040);                     /* the bridge passes 0x60400000-0x605fffff */
			wr(dbi + 0x04, rd(dbi + 0x04) | 2);
			wr(config + 0x10, 0x60400000);
			wr(config + 0x14, 0);
			wr(config + 0x04, rd(config + 0x04) | 2);
			__asm__ volatile ("dsb sy");
			IODelay(100);
			printf("wifi: chip: BAR0 now %#x %#x; command %#x\n", rd(config + 0x10), rd(config + 0x14), rd(config + 4));
			if ((rd(config + 0x10) & ~0xfU) != 0x60400000) {
				return;
			}
			bar = ml_io_map(0x60400000, 0x10000);           /* (the MHI registers and doorbells at the start: 2 MB was more than the kernel had room to map) */
			bhi = rd(bar + 0x28);
			printf("wifi: MHI: length %#x, version %#x, config %#x, BHI at %#x, BHIe at %#x, control %#x, status %#x\n",
			    rd(bar), rd(bar + 8), rd(bar + 0x10), bhi, rd(bar + 0x2c), rd(bar + 0x38), rd(bar + 0x48));
			if (bhi != 0xffffffff && bhi < 0x1000) {
				printf("wifi: MHI: BHI version %#x %#x, status %#x, error %#x, execution environment %#x\n", rd(bar + bhi),
				    rd(bar + bhi + 4), rd(bar + bhi + 0x2c), rd(bar + bhi + 0x30), rd(bar + bhi + 0x28));
			}
			if (wifi_deep >= 6) {
				wifi_smmu_look();
			}
			if (wifi_deep >= 7) {
				wifi_smmu_claim();
			}
			if (wifi_deep >= 8 && wifi_tables != 0) {
				wifi_first_firmware(parf, dbi, config, bar, bhi);
			}
		}
	}
}


static void
wifi_pcie_probe(__unused void *parameter, __unused wait_result_t result)
{
	static const struct { const char *name; uint32_t offset; } clocks[] = {
		{ "aux", 0x7b034 }, { "cfg_ahb", 0x7b030 }, { "mstr_axi", 0x7b028 }, { "slv_axi", 0x7b020 },
		{ "slv_q2a", 0x7b01c }, { "pipe", 0x7b03c }, { "phy_rchng", 0x7b044 }, { "aggre_noc", 0x7b08c },
		{ "cfg_noc", 0x20030 }, { "sf_tbu", 0x9d094 }, { "clkref", 0x9c004 },
	};
	static const struct { const char *name; uint32_t gpio; } pins[] = {
		{ "WLAN_EN", 80 }, { "BT_EN", 81 }, { "SW_CTRL", 82 }, { "PERST", 94 }, { "CLKREQ", 95 }, { "WAKE", 96 },
		{ "XO_CLK", 204 },
	};
	vm_offset_t gcc = ml_io_map(0x00100000, 0x200000);
	uint32_t gdsc, ahb, wifi_step = 1, read_ms = 2500, wait_ms = 15000;

	PE_parse_boot_argn("wifi_pcie", &wifi_step, sizeof(wifi_step));
	wifi_deep = wifi_step;
	/*
	 * One read at a time, said first and with time for the loader to fetch the log: a read that resets the phone is named.
	 * All of them are known harmless now: wifi_read_ms=0 and wifi_wait_ms=N (default 15000) shorten the way to the driver.
	 */
	PE_parse_boot_argn("wifi_read_ms", &read_ms, sizeof(read_ms));
	PE_parse_boot_argn("wifi_wait_ms", &wait_ms, sizeof(wait_ms));
#define WIFI_READ(what, address) ({ \
	printf("wifi: reading %s\n", (what)); \
	if (read_ms != 0) { \
	        IOSleep(read_ms); \
	} \
	rd(address); \
})
	IOSleep(wait_ms);
	gdsc = WIFI_READ("the power domain", gcc + 0x7b004);
	printf("wifi: pcie0: power domain %#x\n", gdsc);
	ahb = WIFI_READ("cfg_ahb", gcc + 0x7b030);
	printf("wifi: pcie0: clock cfg_ahb %#x\n", ahb);
	for (unsigned i = 0; i < sizeof(clocks) / sizeof(clocks[0]); i++) {
		printf("wifi: pcie0: clock %s: %#x\n", clocks[i].name, WIFI_READ(clocks[i].name, gcc + clocks[i].offset));
	}
	for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
		vm_offset_t pin = ml_io_map(0x0f100000 + 0x1000 * (vm_offset_t)pins[i].gpio, 0x1000);   /* pinctrl@f000000 + its registers at 0x100000 */

		printf("wifi: pin %s (gpio %u): control %#x\n", pins[i].name, pins[i].gpio, WIFI_READ(pins[i].name, pin));
		printf("wifi: pin %s: in/out %#x\n", pins[i].name, WIFI_READ(pins[i].name, pin + 4));
	}
	if (wifi_step >= 2) {
		/*
		 * Step 2: the root complex's own supplies at Android's voltages (dt: vreg-1p8 is pm8350c_l10 at 1.2 V, vreg-0p9
		 * is pm8350_l5 at 0.88 V), its power domain, its bus clocks. Nothing of the PHY yet, nor of the Wi-Fi chip.
		 */
		static const struct { const char *name; uint32_t cbcr; } on[] = {
			{ "cfg_ahb", 0x7b030 }, { "mstr_axi", 0x7b028 }, { "slv_axi", 0x7b020 }, { "slv_q2a", 0x7b01c }, { "aux", 0x7b034 },
		};
		bool one = hakios_mayfly_vreg_on("ldoc10", 1200, true), two = hakios_mayfly_vreg_on("ldob5", 880, true);

		printf("wifi: pcie0: supplies asked for: %d %d\n", one, two);
		if (!one || !two) {
			thread_terminate(current_thread());
		}
		wr(gcc + 0x7b004, rd(gcc + 0x7b004) & ~1U);
		for (int i = 0; i < 100 && (rd(gcc + 0x7b004) & (1U << 31)) == 0; i++) {
			IOSleep(1);
		}
		gdsc = rd(gcc + 0x7b004);
		printf("wifi: pcie0: power domain now %#x\n", gdsc);
		if ((gdsc & (1U << 31)) == 0) {
			thread_terminate(current_thread());
		}
		/* votes, as Linux's gcc-sm8450.c: slv_axi, mstr_axi, cfg_ahb, aux, slv_q2a; aggre_noc, cfg_noc, sf_tbu */
		wr(gcc + 0x62008, rd(gcc + 0x62008) | 1U << 0 | 1U << 1 | 1U << 2 | 1U << 3 | 1U << 5);
		wr(gcc + 0x62000, rd(gcc + 0x62000) | 1U << 12 | 1U << 19 | 1U << 20);
		IOSleep(5);
		for (unsigned i = 0; i < sizeof(on) / sizeof(on[0]); i++) {
			printf("wifi: pcie0: clock %s now %#x\n", on[i].name, rd(gcc + on[i].cbcr));
		}
		ahb = rd(gcc + 0x7b030);
	}
	if (wifi_step >= 3 && (rd(gcc + 0x7b030) & (1U << 31)) == 0) {
		/*
		 * Step 3: the PHY, and what stands in front of it (Linux's qcom_pcie_init_2_7_0 and dt's sequence). The link is
		 * not trained and the Wi-Fi chip stays off: PERST stays low. What it shows: the PHY ready, and the root complex's
		 * own identity (17cb:0110) in its configuration registers.
		 */
		vm_offset_t parf = ml_io_map(0x01c00000, 0x3000), phy = ml_io_map(0x01c06000, 0x2000);
		uint32_t ready = 1U << 6;

		wr(gcc + 0x9c004, rd(gcc + 0x9c004) | 1);               /* the reference clock to the PHY */
		wr(gcc + 0x7b048 + 4, 1U << 8 | 11);                    /* phy_rchng: GPLL0 / 6, 100 MHz */
		wr(gcc + 0x7b048, rd(gcc + 0x7b048) | 1);
		for (int i = 0; i < 100 && (rd(gcc + 0x7b048) & 1) != 0; i++) {
			IOSleep(1);
		}
		wr(gcc + 0x62000, rd(gcc + 0x62000) | 1U << 22);
		printf("wifi: pcie0: step 3: clocks asked for; rchng control %#x\n", rd(gcc + 0x7b048));
		wr(gcc + 0x7c01c, 1);                                   /* the PHY's reset */
		wr(gcc + 0x7b000, 1);                                   /* the core's */
		IOSleep(2);
		wr(gcc + 0x7b000, 0);
		wr(gcc + 0x7c01c, 0);
		IOSleep(2);
		wr(parf + 0x1000, 0x4);                                 /* root complex */
		wr(parf + 0x40, rd(parf + 0x40) & ~1U);                 /* the PHY's test power-down off */
		wr(parf + 0x350, 0x60000000);                           /* the configuration registers' window */
		wr(parf + 0x354, 0);
		wr(parf + 0x358, 0x80000000);
		wr(parf + 0x35c, 0);
		wr(parf + 0x634, 0x60001000);
		wr(parf + 0x638, 0);
		wr(parf + 0x00, rd(parf + 0x00) & ~(1U << 29));
		wr(parf + 0x174, rd(parf + 0x174) | 1U << 4);
		for (unsigned i = 0; i < sizeof(pcie0_phy_sequence) / sizeof(pcie0_phy_sequence[0]); i++) {
			wr(phy + pcie0_phy_sequence[i].offset, pcie0_phy_sequence[i].value);
			if (pcie0_phy_sequence[i].wait_us != 0) {
				IODelay(pcie0_phy_sequence[i].wait_us);
			}
		}
		__asm__ volatile ("dsb sy");
		wr(gcc + 0x7b060, 0);                                   /* pipe's source: the PHY */
		wr(gcc + 0x62008, rd(gcc + 0x62008) | 1U << 4);
		for (int i = 0; i < 200 && ((ready = rd(phy + 0x214)) & (1U << 6)) != 0; i++) {
			IOSleep(1);
		}
		printf("wifi: pcie0: step 3: PHY status %#x (%s), pipe clock %#x, power control %#x\n", ready,
		    (ready & (1U << 6)) == 0 ? "ready" : "NOT READY", rd(gcc + 0x7b03c), rd(phy + 0x240));
		if ((ready & (1U << 6)) == 0) {
			vm_offset_t dbi = ml_io_map(0x60000000, 0x1000);

			printf("wifi: pcie0: step 3: root complex's identity %#x, link state %#x\n", rd(dbi), rd(parf + 0x1b0));
			if (wifi_step >= 4) {
				wifi_chip_and_link(gcc, parf, dbi);
			}
		}
		thread_terminate(current_thread());
	}
	if ((gdsc & (1U << 31)) != 0 && (ahb & (1U << 31)) == 0) {
		vm_offset_t parf = ml_io_map(0x01c00000, 0x1000);

		printf("wifi: pcie0: PARF: first word %#x, link state %#x\n", rd(parf), rd(parf + 0x1b0));
	} else {
		printf("wifi: pcie0: off as ABL left it (no power or no bus clock): PARF not read\n");
	}
	thread_terminate(current_thread());
}

/*
 * The phone resets now and then under load (opening applications fast, 1 November), with nothing in the log: a stream
 * an SMMU cannot serve did that before (UFS, the apps SMMU: a reset some 20 s after the fault). Four times a second,
 * what the SMMUs have recorded -- the GPU's own (its 3D core's and its GMU's contexts), the apps SMMU's global fault
 * status and Wi-Fi's context there -- is looked at and told the moment it is not clear, so that it is in the log the
 * host fetches every few seconds before any reset. A fault seen is cleared (the status is write-one-to-clear).
 */
void hakios_mayfly_cpu_ceiling(void);    /* (below: boot-arg cpu_mhz) */
static void thermal_step(uint32_t seconds);     /* (below: the temperature, once a second) */

static void
smmu_watch(__unused void *parameter, __unused wait_result_t result)
{
	vm_offset_t apps = ml_io_map(0x15000000, 0x1000);
	uint32_t told = 0, beats = 0;

	printf("smmu-watch: looking at the SMMUs four times a second\n");
	for (;;) {
		const struct gpu_space *spaces[2] = { &core_space, &gmu_space };
		uint32_t global = rd(apps + 0x48);

		for (unsigned i = 0; i < 2; i++) {
			uint32_t context = smmu_context(spaces[i]), status = rd(smmu.base + context + SMMU_CB_FSR);

			if ((status & SMMU_FSR_ALL) != 0 && told < 40) {
				told++;
				printf("smmu-watch: THE GPU'S SMMU REFUSED %s: status %#x, address %#llx, syndrome %#x\n", spaces[i]->name,
				    status, (uint64_t)rd(smmu.base + context + SMMU_CB_FAR + 4) << 32 | rd(smmu.base + context + SMMU_CB_FAR),
				    rd(smmu.base + context + SMMU_CB_FSYNR0));
				wr(smmu.base + context + SMMU_CB_FSR, status);
			}
		}
		if (wifi_context != 0) {
			uint32_t status = rd(wifi_context + SMMU_CB_FSR);

			if ((status & SMMU_FSR_ALL) != 0 && told < 40) {
				told++;
				printf("smmu-watch: THE APPS SMMU REFUSED WI-FI: status %#x, address %#llx, syndrome %#x\n", status,
				    (uint64_t)rd(wifi_context + SMMU_CB_FAR + 4) << 32 | rd(wifi_context + SMMU_CB_FAR),
				    rd(wifi_context + SMMU_CB_FSYNR0));
				wr(wifi_context + SMMU_CB_FSR, status);
			}
		}
		if (global != 0 && told < 40) {
			told++;
			printf("smmu-watch: THE APPS SMMU HAS A GLOBAL FAULT: sGFSR %#x, syndrome %#x %#x\n", global, rd(apps + 0x50),
			    rd(apps + 0x54));
		}
		if (++beats % 4 == 0) {
			thermal_step(beats / 4);
		}
		if (beats % 240 == 0) {
			printf("smmu-watch: %u s, nothing more\n", beats / 4);
		}
		IOSleep(250);
	}
}

static void
gpu_keep(void)
{
	thread_t probe;

	if (kernel_thread_start(panel_probe, NULL, &probe) == KERN_SUCCESS) {
		thread_deallocate(probe);
	}
	{
		uint32_t wifi_pcie = 0;

		PE_parse_boot_argn("wifi_pcie", &wifi_pcie, sizeof(wifi_pcie));
		if (wifi_pcie != 0 && kernel_thread_start(wifi_pcie_probe, NULL, &probe) == KERN_SUCCESS) {
			thread_deallocate(probe);
		}
	}
	{
		extern bool hakios_mayfly_memory_fast(uint32_t vote);
		uint32_t vote = 0x3fff;

		PE_parse_boot_argn("ddr_vote", &vote, sizeof(vote));
		if (vote != 0) {
			printf("adreno: memory bandwidth vote %u: %d\n", vote, hakios_mayfly_memory_fast(vote));
		}
	}
	gpu_free[0].start = GPU_ADDRESSES_START;
	gpu_free[0].size = GPU_ADDRESSES_SIZE;
	gpu_free_count = 1;
	bzero((void *)core_scratch.memory, 4096);
	to_memory(core_scratch.memory, 4096);
	PE_parse_boot_argn("gpu_log", &gpu_log, sizeof(gpu_log));
	gpu_kept = true;
	hakios_mayfly_cpu_ceiling();
	{
		thread_t watcher;

		if (kernel_thread_start(smmu_watch, NULL, &watcher) == KERN_SUCCESS) {
			thread_deallocate(watcher);
		}
	}
	{
		extern void hakios_cpusample_start(void);       /* (osfmk/kern/hakios_cpusample.c: boot-arg cpu_sample=1) */

		hakios_cpusample_start();
	}
	printf("adreno: THE GPU STAYS ON for the system to draw with: the service HakiosGPU\n");
	HakiosGPUStart();
}

/* ---- all of it ------------------------------------------------------------------------------------------- */

static bool(*const steps[])(void) = {
	step_outside, step_gpucc, step_cx_power, step_clocks, step_first_words, step_rscc, step_smmu, step_smmu_context,
	step_gmu_memory, step_trustzone, step_zap_signature, step_gmu_space, step_gmu_firmware, step_gmu_start,
	step_core_power, step_core_space, step_core_start, step_zap_and_secure,
};
#define STEPS ((uint32_t)(sizeof(steps) / sizeof(steps[0])))

static uint32_t keep;

/* The steps in order; true if all of them were done */
static bool
run_steps(void)
{
	uint32_t done = 0;

	printf("adreno: the Adreno 730's steps 1 to %u; step %u waits %u ms %s%s\n", last_step, last_step, pause_ms,
	    fine ? "before every access" : "after each announcement", spin_waits ? "; waits spin" : "");
	for (step = 1; step <= last_step; step++) {
		if (!steps[step - 1]()) {
			printf("adreno: step %u: FAILED; the steps after it are not attempted\n", step);
			break;
		}
		printf("adreno: step %u: done\n", step);
		done = step;
	}
	printf("adreno: over\n");
	return done == STEPS;
}

static void
adreno7_thread(__unused void *parameter, __unused wait_result_t result)
{
	run_steps();
	thread_terminate(current_thread());
}

static void
adreno7_init(void)
{
	uint32_t gpu = 0;

	/*
	 * Boot-arg gpu=1: all the steps, quickly, and the GPU kept for the window server (adreno7_keep=1 does the same
	 * after adreno7=N). Otherwise adreno7=N alone: the first N steps, slowly, in a thread of their own, for study.
	 */
	if (PE_parse_boot_argn("gpu", &gpu, sizeof(gpu)) && gpu) {
		last_step = STEPS;
		pause_ms = 0;
		keep = 1;
		spin_waits = 1;
	}
	if (!PE_parse_boot_argn("adreno7", &last_step, sizeof(last_step)) && !keep) {
		return;
	}
	if (last_step > STEPS) {
		last_step = STEPS;
	}
	PE_parse_boot_argn("adreno7_pause", &pause_ms, sizeof(pause_ms));
	PE_parse_boot_argn("adreno7_fine", &fine, sizeof(fine));
	PE_parse_boot_argn("adreno7_spin", &spin_waits, sizeof(spin_waits));
	PE_parse_boot_argn("adreno7_keep", &keep, sizeof(keep));
}

STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, adreno7_init);

/* What the rest of the kernel calls in the GPU driver (drivers/core/hakios.stubs.c has them empty without one). */
void hakios_adreno_init(void);
void hakios_adreno_quiesce(void);
void hakios_adreno_stop(void);
void hakios_adreno_minute(void);

/*
 * Called from hakios_input_init(), when the RAM disks are there (the firmware is in one) and before the window
 * server starts: a GPU to be kept is started here and the system waits for it, as on redwood; steps for study go
 * on beside the rest of the start.
 */
void
hakios_adreno_init(void)
{
	thread_t thread;

	if (last_step == 0) {
		return;
	}
	if (keep) {
		if (run_steps()) {
			gpu_keep();
		} else {
			printf("adreno: THE GPU IS NOT STARTED: the system draws with its software renderer\n");
		}
		return;
	}
	if (kernel_thread_start(adreno7_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

/*
 * Once a minute (osfmk/arm64/loader_return.c), what the three CPU clock domains run at: the loader asks each EPSS
 * block for its top regular entry before the kernel starts, when the big cores are still off and say 0 MHz; this
 * is whether they took it. Registers: Linux's qcom-cpufreq-hw.c (epss: domain state 0x20, perf state 0x320).
 */
/*
 * Boot-arg cpu_mhz=N: every clock domain to the highest regular entry of its table at or under N MHz, in place of the
 * loader's top one (the Xiaomi 12S, 1 November: the phone reset under load, the limits hardware had just pulled the big
 * cores down from 2649 and 2995 MHz to 1881 and 2246 -- a supply or heat limit, which a lower ceiling keeps away from).
 */
void hakios_mayfly_cpu_ceiling(void);

static vm_offset_t ceiling_epss;
static uint32_t cpu_ceiling_asked;              /* MHz; 0: the top of each table */

static void
cpu_ceiling_set(uint32_t ceiling, bool tell)
{
	if (ceiling_epss == 0) {
		ceiling_epss = ml_io_map(0x17d91000, 0x3000);
	}
	vm_offset_t epss = ceiling_epss;

	cpu_ceiling_asked = ceiling;
	if (ceiling == 0) {
		ceiling = 100000;
	}
	for (int d = 0; d < 3; d++) {
		uint32_t best = 0, best_mhz = 0, prev_mhz = 0;

		for (uint32_t i = 0; i < 40; i++) {
			uint32_t data = rd(epss + 0x1000 * d + 0x100 + 4 * i);
			uint32_t mhz = data >> 30 ? (data & 0xff) * 192 / 10 : 300;

			if (mhz == prev_mhz) {
				break;
			}
			prev_mhz = mhz;
			if ((data >> 16 & 7) != 1 && mhz <= ceiling && mhz > best_mhz) {
				best = i;
				best_mhz = mhz;
			}
		}
		if (best_mhz != 0) {
			wr(epss + 0x1000 * d + 0x320, best);
		}
		if (tell) {
			printf("adreno: cpu: clock domain %d: at most %u MHz: entry %u, %u MHz\n", d, ceiling, best, best_mhz);
		}
	}
}

void
hakios_mayfly_cpu_ceiling(void)
{
	uint32_t ceiling = 0;

	if (PE_parse_boot_argn("cpu_mhz", &ceiling, sizeof(ceiling)) && ceiling != 0) {
		cpu_ceiling_set(ceiling, true);
	}
}

/*
 * The phone's own temperature sensors (dt tsens0 thermal-sensor@c263000, tsens1 @c265000: 16 sensors each; Linux's
 * tsens-v2.c: TM_SN_STATUS 0xa0 + 4n, the last temperature in [11:0], tenths of a degree, signed, valid in bit 21) and
 * a governor of the simplest kind, there being none at all before (the phone hot to the hand and resetting under load,
 * 1 November): over THERMAL_HOT the cores' ceiling one step down, under THERMAL_COOL for ten seconds one step up.
 * Boot-arg thermal=0: no governor (the readings are still told).
 */
/*
 * (The user, 1 November: 80 degrees is normal for this SoC and short peaks are fine. A step down needs THERMAL_HOT for
 * THERMAL_HOT_FOR seconds running, or THERMAL_HOTTEST once; a step up THERMAL_COOL for ten seconds.)
 */
#define THERMAL_HOT     900             /* tenths of a degree (the user: 90 for half a minute is the trouble, peaks are not) */
#define THERMAL_HOT_FOR 30
#define THERMAL_HOTTEST 1050
#define THERMAL_COOL    750
static const uint32_t thermal_steps[] = { 0, 2400, 2000, 1700, 1400, 1100 };

int hakios_mayfly_hottest(int *sensor);

int
hakios_mayfly_hottest(int *sensor)
{
	static vm_offset_t tm[2];
	int hottest = -1000, where = -1;

	for (int b = 0; b < 2; b++) {
		if (tm[b] == 0) {
			tm[b] = ml_io_map(b == 0 ? 0x0c263000 : 0x0c265000, 0x1000);
		}
		for (int n = 0; n < 16; n++) {
			uint32_t status = rd(tm[b] + 0xa0 + 4 * n);
			int t = (int)(status & 0xfff);

			if (!(status & (1u << 21))) {
				continue;
			}
			if (t & 0x800) {
				t -= 0x1000;
			}
			if (t > hottest && t < 2000) {
				hottest = t;
				where = 16 * b + n;
			}
		}
	}
	if (sensor != NULL) {
		*sensor = where;
	}
	return hottest;
}

static void
thermal_step(uint32_t seconds)
{
	static uint32_t step, cool_for, hot_for, governor = 2, lowest_step;   /* (lowest_step: never above cpu_mhz's ceiling) */
	int sensor, t = hakios_mayfly_hottest(&sensor);

	if (governor == 2) {
		governor = 1;
		PE_parse_boot_argn("thermal", &governor, sizeof(governor));
		if (cpu_ceiling_asked != 0) {
			/* (cpu_mhz= given: the governor starts at the step that is at or under it) */
			for (step = 1; step + 1 < sizeof(thermal_steps) / sizeof(thermal_steps[0]) && thermal_steps[step] > cpu_ceiling_asked;
			    step++) {
			}
			lowest_step = step;
		}
	}
	hot_for = t >= THERMAL_HOT ? hot_for + 1 : 0;
	if (governor && (hot_for >= THERMAL_HOT_FOR || t >= THERMAL_HOTTEST) && step + 1 < sizeof(thermal_steps) / sizeof(thermal_steps[0])) {
		step++;
		cool_for = 0;
		hot_for = 0;
		cpu_ceiling_set(thermal_steps[step], false);
		printf("thermal: %d.%d C (sensor %d): THE CORES DOWN to at most %u MHz\n", t / 10, t % 10, sensor, thermal_steps[step]);
	} else if (governor && t < THERMAL_COOL && step > lowest_step && ++cool_for >= 10) {
		step--;
		cool_for = 0;
		cpu_ceiling_set(thermal_steps[step], false);
		printf("thermal: %d.%d C (sensor %d): the cores up to %s%u MHz\n", t / 10, t % 10, sensor,
		    thermal_steps[step] ? "at most " : "the top, ", thermal_steps[step] ? thermal_steps[step] : 0);
	} else if (t >= THERMAL_COOL) {
		cool_for = 0;
	}
	if (seconds % 10 == 0) {
		printf("thermal: %u s: the hottest sensor %d.%d C (sensor %d), the cores' ceiling %s%u MHz\n", seconds, t / 10,
		    t % 10, sensor, thermal_steps[step] ? "" : "the top, ", thermal_steps[step]);
	}
}

/* For the display driver's numbers on the screen (MiniIOMobileFramebuffer.cpp, fb_hud=1): the hottest sensor, the clocks. */
void hakios_mayfly_hud_numbers(int *temperature, uint32_t mhz[3], uint32_t *ceiling);
uint32_t hakios_mayfly_gpu_mhz(void);

uint32_t
hakios_mayfly_gpu_mhz(void)
{
	return gpu_kept ? gpu_mhz_asked : 0;
}

void
hakios_mayfly_hud_numbers(int *temperature, uint32_t mhz[3], uint32_t *ceiling)
{
	if (ceiling_epss == 0) {
		ceiling_epss = ml_io_map(0x17d91000, 0x3000);
	}
	*temperature = hakios_mayfly_hottest(NULL);
	for (int d = 0; d < 3; d++) {
		mhz[d] = (rd(ceiling_epss + 0x1000 * d + 0x20) & 0xff) * 192 / 10;
	}
	*ceiling = cpu_ceiling_asked;
}

void
hakios_adreno_minute(void)
{
	static vm_offset_t epss;
	uint32_t mhz[3], asked[3];

	if (epss == 0) {
		epss = ml_io_map(0x17d91000, 0x3000);
	}
	for (int d = 0; d < 3; d++) {
		mhz[d] = (rd(epss + 0x1000 * d + 0x20) & 0xff) * 192 / 10;
		asked[d] = rd(epss + 0x1000 * d + 0x320);
	}
	printf("adreno: cpu clocks now: cores 0-3 %u MHz, 4-6 %u MHz, 7 %u MHz (entries asked %u %u %u)\n", mhz[0], mhz[1],
	    mhz[2], asked[0], asked[1], asked[2]);
}
