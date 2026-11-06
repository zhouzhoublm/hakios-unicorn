/*
 * The touch screen of the Xiaomi 12S (mayfly): an STMicroelectronics FingerTip ("FTS", FTM5 family, the
 * vendor's "SALIXP" variant) on the SPI bus of the SoC's serial engine 4, as a source of touches for
 * /dev/hakios-touch (sys/hakios.input.h). Only with boot-arg touchscreen=1: every address below
 * belongs to this phone.
 *
 * There is no clock, pin, regulator or SPI framework in this kernel, so the driver does all of it
 * itself, in the order the vendor's Linux driver does:
 *   power    vdd (1.8 V, LDO2 of the PM8350C, "ldoc2") and then avdd (3.2 V, LDO7, "ldoc7"), each
 *            switched through RPMh: one request written into the application processor's command slots
 *   clocks   the serial engine's clock and the QUP wrapper's bus clocks, by vote in the global clock
 *            controller
 *   pins     gpio16-19 to the serial engine ("qup4"), reset on gpio20, the interrupt line on gpio21
 *   SPI      the serial engine ("GENI") in FIFO mode, polled
 *   chip     reset by the pin, wait for "controller ready" in the event FIFO, read the chip ID, turn
 *            sensing on, then read the FIFO while the interrupt line is low
 * No interrupt is used: a thread looks at the interrupt line every 8 ms (boot-arg touch_poll_us=N).
 * The chip keeps its own firmware in its own flash; this driver never writes to that flash.
 *
 * Where the numbers come from (none of them measured on the phone yet):
 *   the phone's device tree (LineageOS android_kernel_xiaomi_sm8450-devicetrees, mayfly-sm8475.dtsi,
 *     waipio-qupv3.dtsi, cape-regulators.dtsi, waipio.dtsi): spi@990000, fts@0 "st,spi", gpio 16-21,
 *     avdd-supply = L7C (3.2 V on this phone), vdd-supply = L2C, apps_rsc drv-2 at 0x17a20000 with
 *     its command slots at 0xd00, cmd-db at 0x80860000, "fts,x-max" 10800 and "fts,y-max" 24000 with
 *     "fts,support-super-resolution"
 *   Xiaomi's Linux driver (drivers/input/touchscreen/fts_spi, ST's FTS v5.2.1): ftsHardware.h (op codes,
 *     dummy bytes, register addresses, chip ID 0x36 0x40), ftsIO.c (framing), ftsCore.c (reset, event
 *     polling), fts.c (power order, SPI4 mode, sense on, the event layout)
 *   Linux drivers/clk/qcom/gcc-waipio.c (the votes and the serial engine's clock registers) and
 *     drivers/pinctrl/qcom/pinctrl-cape.h (the pins' function numbers)
 *   the SPI, RPMh and command-database code of hakios.goodix.c (redwood), which is the same hardware
 *     at other addresses
 *
 * Status: written and compiled; not yet run on the phone.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/startup.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);

/* RPMh's command slots: the other drivers that write them take the same lock (drivers/core/hakios.platform.c) */
extern lck_mtx_t hakios_rsc_lock;

static vm_offset_t tlmm, gcc, se, rsc, cmd_db;

static inline uint32_t
rd(vm_offset_t address)
{
	return *(volatile uint32_t *)address;
}

static inline void
wr(vm_offset_t address, uint32_t value)
{
	*(volatile uint32_t *)address = value;
}

static void
sleep_us(uint32_t us)
{
	delay_for_interval(us, NSEC_PER_USEC);
}

static void
sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

/* ---- pins (TLMM) ------------------------------------------------------------------------------- */

#define TLMM_PHYS               0x0f100000UL    /* pinctrl@f000000 + REG_BASE 0x100000 */
#define TLMM_SIZE               0x100000UL
#define GPIO_CFG(n)             (tlmm + 0x1000UL * (n))         /* [1:0] pull, [5:2] function, [8:6] drive, [9] output */
#define GPIO_IO(n)              (tlmm + 0x1000UL * (n) + 4)     /* bit 0 in, bit 1 out */
#define PULL_NONE               0
#define PULL_UP                 3

#define PIN_MISO                16      /* the serial engine's lines 0-3: in, out, clock, chip select */
#define PIN_MOSI                17
#define PIN_CLK                 18
#define PIN_CS                  19
#define PIN_RESET               20      /* low = in reset */
#define PIN_INT                 21      /* low = the chip has events waiting (IRQ_TYPE_LEVEL_LOW) */
#define PIN_FUNCTION_QUP4       1       /* pinctrl-cape.h: PINGROUP(16..19, qup4, ...) */

static void
gpio_configure(unsigned pin, unsigned function, unsigned pull, unsigned ma, bool output)
{
	wr(GPIO_CFG(pin), pull | function << 2 | (ma / 2 - 1) << 6 | (output ? 1u << 9 : 0));
}

static void
gpio_set(unsigned pin, bool high)
{
	wr(GPIO_IO(pin), high ? 2 : 0);
}

static bool
gpio_get(unsigned pin)
{
	return rd(GPIO_IO(pin)) & 1;
}

static void
gpio_output(unsigned pin, bool high, unsigned pull, unsigned ma)
{
	gpio_set(pin, high);            /* the level first, so that the pin never drives the wrong one */
	gpio_configure(pin, 0, pull, ma, true);
}

/* ---- the supplies (RPMh) ------------------------------------------------------------------------- */

#define CMD_DB_PHYS             0x80860000UL
#define CMD_DB_SIZE             0x20000UL
#define RSC_PHYS                0x17a20000UL    /* apps_rsc, "drv-2": the application processor's */
#define RSC_SIZE                0x10000UL
#define TCS(reg)                (rsc + 0xd00 + (reg))           /* qcom,tcs-offset: command slot set 0, an "active" one */
#define TCS_IRQ_ENABLE          0x00
#define TCS_IRQ_STATUS          0x04
#define TCS_IRQ_CLEAR           0x08
#define TCS_WAIT_FOR_COMPLETION 0x10
#define TCS_CONTROL             0x14
#define TCS_CONTROL_ENABLE      (1u << 16)
#define TCS_CONTROL_TRIGGER     (1u << 24)
#define TCS_STATUS              0x18            /* 0 while busy */
#define TCS_CMD_ENABLE          0x1c
#define TCS_CMD_MSGID(n)        (0x30 + 20 * (n))
#define TCS_CMD_ADDR(n)         (0x34 + 20 * (n))
#define TCS_CMD_DATA(n)         (0x38 + 20 * (n))
#define MSGID_WRITE_4_BYTES     0x10008
#define MSGID_RESPONSE          0x100

#define VRM_VOLTAGE             0               /* a regulator's registers, from its address: millivolts */
#define VRM_ENABLE              4
#define VRM_MODE                8
#define VRM_MODE_LDO_HPM        7

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

/* The RPMh address of a resource by its name in the command database the boot loader left; 0 = none. */
static uint32_t
rpmh_address(const char *name)
{
	const struct cmd_db_header *db = (const struct cmd_db_header *)cmd_db;
	char id[8] = { 0 };

	if (db->magic[0] != 0xdb || db->magic[1] != 0x30 || db->magic[2] != 0x03 || db->magic[3] != 0x0c) {
		printf("stfts: no command database (magic %02x %02x %02x %02x)\n", db->magic[0], db->magic[1], db->magic[2],
		    db->magic[3]);
		return 0;
	}
	strlcpy(id, name, sizeof(id));
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(db->data + slave->header_offset);

		if (slave->id == 0 || slave->header_offset >= CMD_DB_SIZE - 0x1000 || slave->count > 512) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			if (memcmp(entry[i].id, id, sizeof(id)) == 0) {
				return entry[i].address;
			}
		}
	}
	return 0;
}

static bool
rpmh_wait(uint32_t reg, uint32_t mask, uint32_t value, const char *what)
{
	for (int i = 0; i < 2000; i++) {
		if ((rd(TCS(reg)) & mask) == value) {
			return true;
		}
		sleep_us(100);
	}
	printf("stfts: RPMh: timeout waiting for %s (register %#x = %#x)\n", what, reg, rd(TCS(reg)));
	return false;
}

/* Send up to three register writes to RPMh and wait until they are done. */
static bool
rpmh_write(const uint32_t *address, const uint32_t *data, unsigned count)
{
	uint32_t irq_enable;
	bool done;

	lck_mtx_lock(&hakios_rsc_lock);
	irq_enable = rd(TCS(TCS_IRQ_ENABLE));
	/* the slot set must be idle, and switched off the way the hardware wants it: trigger, then enable */
	for (int i = 0; rd(TCS(TCS_STATUS)) == 0; i++) {
		if (i > 2000) {
			printf("stfts: RPMh: the command slots stay busy\n");
			lck_mtx_unlock(&hakios_rsc_lock);
			return false;
		}
		sleep_us(100);
	}
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_TRIGGER);
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_ENABLE);
	wr(TCS(TCS_IRQ_CLEAR), 1);
	for (unsigned i = 0; i < count; i++) {
		wr(TCS(TCS_CMD_MSGID(i)), MSGID_WRITE_4_BYTES | MSGID_RESPONSE);
		wr(TCS(TCS_CMD_ADDR(i)), address[i]);
		wr(TCS(TCS_CMD_DATA(i)), data[i]);
	}
	wr(TCS(TCS_WAIT_FOR_COMPLETION), (1u << count) - 1);
	wr(TCS(TCS_CMD_ENABLE), (1u << count) - 1);
	wr(TCS(TCS_IRQ_ENABLE), irq_enable | 1);        /* the status bit follows the enable; the interrupt itself stays masked */
	wr(TCS(TCS_CONTROL), TCS_CONTROL_ENABLE);
	wr(TCS(TCS_CONTROL), TCS_CONTROL_ENABLE | TCS_CONTROL_TRIGGER);

	done = rpmh_wait(TCS_IRQ_STATUS, 1, 1, "the request");
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_TRIGGER);
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_ENABLE);
	wr(TCS(TCS_CMD_ENABLE), 0);
	wr(TCS(TCS_WAIT_FOR_COMPLETION), 0);
	wr(TCS(TCS_IRQ_CLEAR), 1);
	wr(TCS(TCS_IRQ_ENABLE), irq_enable);
	lck_mtx_unlock(&hakios_rsc_lock);
	return done;
}

/*
 * A regulator on at `millivolts` for another driver of this phone's (Wi-Fi's root complex): false if the supplies'
 * blocks are not mapped yet, which is to say touch has not come up.
 */
bool
hakios_mayfly_vreg_on(const char *name, uint32_t millivolts, bool high_power)
{
	uint32_t base, address[3], data[3];

	if (rsc == 0 || cmd_db == 0 || (base = rpmh_address(name)) == 0 || (base >> 16 & 0xf) != 4) {
		printf("wifi: RPMh: no regulator \"%s\"\n", name);
		return false;
	}
	address[0] = base + VRM_VOLTAGE;
	address[1] = base + VRM_ENABLE;
	address[2] = base + VRM_MODE;
	data[0] = millivolts;
	data[1] = 1;
	data[2] = VRM_MODE_LDO_HPM;
	return rpmh_write(address, data, high_power ? 3 : 2);
}

/*
 * The memory's bandwidth (boot-arg ddr_vote=N; 0: no vote): as drivers/power/hakios.cpufreq.c does on the redwood, whose
 * RPMh block is elsewhere: the most on the memory controller's clock manager and the shared hub's, and the memory kept
 * on for this processor. A command is commit, valid, average and peak, 14 bits each.
 */
bool
hakios_mayfly_memory_fast(uint32_t vote)
{
	uint32_t mc0, sh0, acv, address[3], data[3], command;

	if (rsc == 0 || cmd_db == 0 || (mc0 = rpmh_address("MC0")) == 0 || (sh0 = rpmh_address("SH0")) == 0 ||
	    (acv = rpmh_address("ACV")) == 0) {
		return false;
	}
	command = 1U << 30 | 1U << 29 | (vote & 0x3fff) << 14 | (vote & 0x3fff);
	address[0] = mc0;
	address[1] = sh0;
	address[2] = acv;
	data[0] = command;
	data[1] = command;
	data[2] = 1U << 30 | 1U << 29 | 1U << 3;
	return rpmh_write(address, data, 3);
}

/* One of the PM8350C's LDOs on, at `millivolts`, in high-power mode. */
static bool
ldo_on(const char *name, uint32_t millivolts)
{
	uint32_t base = rpmh_address(name);
	uint32_t address[3] = { base + VRM_VOLTAGE, base + VRM_ENABLE, base + VRM_MODE };
	uint32_t data[3] = { millivolts, 1, VRM_MODE_LDO_HPM };

	/* addresses of regulators ("VRM") have 4 in bits 19:16; anything else is not what we are looking for */
	if (base == 0 || (base >> 16 & 0xf) != 4) {
		printf("stfts: RPMh: no regulator \"%s\" (address %#x)\n", name, base);
		return false;
	}
	if (!rpmh_write(address, data, 3)) {
		printf("stfts: RPMh: %s (at %#x) did not take the request\n", name, base);
		return false;
	}
	printf("stfts: %s on, %u mV (RPMh %#x)\n", name, millivolts, base);
	return true;
}

/* ---- clocks (GCC) -------------------------------------------------------------------------------- */

#define GCC_PHYS                0x00100000UL
#define GCC_SIZE                0x70000UL
#define GCC_VOTE                (gcc + 0x62008)         /* the application processor's votes */
#define VOTE_QUP0_M_AHB         (1u << 6)
#define VOTE_QUP0_S_AHB         (1u << 7)
#define VOTE_QUP0_CORE          (1u << 8)
#define VOTE_QUP0_CORE_2X       (1u << 9)
#define VOTE_QUP0_SE4           (1u << 14)
#define GCC_QUP0_M_AHB_CBCR     (gcc + 0x27004)         /* bit 31: the clock is off */
#define GCC_QUP0_S_AHB_CBCR     (gcc + 0x27008)
#define GCC_QUP0_SE4_CBCR       (gcc + 0x274dc)
#define GCC_QUP0_SE4_CMD        (gcc + 0x274e4)         /* bit 0: take the new configuration */
#define GCC_QUP0_SE4_CFG        (gcc + 0x274e8)         /* [4:0] 2*divider-1, [10:8] source, [13:12] mode */
#define GCC_QUP0_SE4_DFSR       (gcc + 0x274f8)         /* bit 0: the engine chooses among preset rates itself */
#define GCC_QUP0_SE4_PERF_CFG(l) (gcc + 0x27500 + 4 * (l))
#define GCC_QUP0_SE4_PERF_M(l)  (gcc + 0x27540 + 4 * (l))
#define GCC_QUP0_SE4_PERF_N(l)  (gcc + 0x27580 + 4 * (l))
#define CLOCK_OFF               (1u << 31)
#define XO_HZ                   19200000

static uint32_t se_source_hz, se_clock_select;

/* gcc_parent_map_0 of gcc-waipio.c: 0 the crystal, 1 GPLL0 (600 MHz), 6 GPLL0 even (300 MHz) */
static uint32_t
rcg_rate(uint32_t cfg, uint32_t m, uint32_t not_n_minus_m)
{
	static const uint32_t parent[8] = { XO_HZ, 600000000, 0, 0, 0, 0, 300000000, 0 };
	uint64_t hz = parent[cfg >> 8 & 7];
	uint32_t divider = cfg & 0x1f, mode = cfg >> 12 & 3;

	if (divider) {
		hz = hz * 2 / (divider + 1);
	}
	if (mode) {
		uint32_t n = (~not_n_minus_m & 0xffff) + (m & 0xffff);

		if (n) {
			hz = hz * (m & 0xffff) / n;
		}
	}
	return (uint32_t)hz;
}

static bool
clocks_on(void)
{
	wr(GCC_VOTE, rd(GCC_VOTE) | VOTE_QUP0_CORE_2X | VOTE_QUP0_CORE | VOTE_QUP0_M_AHB | VOTE_QUP0_S_AHB);
	if (rd(GCC_QUP0_SE4_DFSR) & 1) {
		/* preset rates: the lowest that is at least 4 MHz */
		for (uint32_t level = 0; level < 8; level++) {
			uint32_t hz = rcg_rate(rd(GCC_QUP0_SE4_PERF_CFG(level)), rd(GCC_QUP0_SE4_PERF_M(level)),
			    rd(GCC_QUP0_SE4_PERF_N(level)));

			if (hz >= 4000000 && (se_source_hz == 0 || hz < se_source_hz)) {
				se_source_hz = hz;
				se_clock_select = level;
			}
		}
		if (se_source_hz == 0) {
			printf("stfts: no usable preset clock rate\n");
			return false;
		}
	} else {
		/* the crystal, undivided: nothing else has to be running */
		wr(GCC_QUP0_SE4_CFG, (rd(GCC_QUP0_SE4_CFG) & ~0x371fu) | 1);
		wr(GCC_QUP0_SE4_CMD, rd(GCC_QUP0_SE4_CMD) | 1);
		for (int i = 0; i < 500 && (rd(GCC_QUP0_SE4_CMD) & 1); i++) {
			sleep_us(10);
		}
		if (rd(GCC_QUP0_SE4_CMD) & 1) {
			printf("stfts: the serial engine's clock does not take its configuration (%#x)\n", rd(GCC_QUP0_SE4_CMD));
			return false;
		}
		se_source_hz = XO_HZ;
		se_clock_select = 0;
	}
	wr(GCC_VOTE, rd(GCC_VOTE) | VOTE_QUP0_SE4);
	/* a root that nobody has asked for yet stays off, and with it the engine's clock: ask (CMD_RCGR.ROOT_EN) */
	wr(GCC_QUP0_SE4_CMD, rd(GCC_QUP0_SE4_CMD) | 2);
	for (int i = 0; i < 1000; i++) {
		/* not the two AHB clocks: they are under hardware control (gcc-waipio.c, hwcg_bit) and read off while idle */
		if (!(rd(GCC_QUP0_SE4_CBCR) & CLOCK_OFF)) {
			printf("stfts: serial engine's clock on: %#x, root %#x\n", rd(GCC_QUP0_SE4_CBCR), rd(GCC_QUP0_SE4_CMD));
			return true;
		}
		sleep_us(10);
	}
	printf("stfts: the serial engine's clock reads off (%#x, root %#x, ahb %#x %#x): trying the engine anyway\n", rd(GCC_QUP0_SE4_CBCR),
	    rd(GCC_QUP0_SE4_CMD), rd(GCC_QUP0_M_AHB_CBCR), rd(GCC_QUP0_S_AHB_CBCR));
	return true;
}

/* ---- SPI through the serial engine (GENI) ---------------------------------------------------------- */

#define SE_PHYS                 0x00990000UL    /* qupv3_se4_spi: spi@990000 */
#define SE_SIZE                 0x4000UL
#define GENI_FORCE_DEFAULT      (se + 0x020)
#define GENI_OUTPUT_CTRL        (se + 0x024)
#define GENI_CGC_CTRL           (se + 0x028)
#define GENI_STATUS             (se + 0x040)    /* bit 0: a command is running */
#define GENI_SER_M_CLK_CFG      (se + 0x048)    /* bit 0 enable, [15:4] divider */
#define GENI_IF_DISABLE_RO      (se + 0x064)    /* bit 0: no FIFO interface */
#define GENI_FW_REVISION_RO     (se + 0x068)    /* [15:8] protocol: 1 = SPI */
#define GENI_CLK_SEL            (se + 0x07c)
#define SPI_CPHA                (se + 0x224)
#define SPI_LOOPBACK            (se + 0x22c)
#define SPI_CPOL                (se + 0x230)
#define SPI_DEMUX_OUTPUT_INV    (se + 0x24c)
#define SPI_DEMUX_SEL           (se + 0x250)
#define GENI_BYTE_GRAN          (se + 0x254)
#define GENI_DMA_MODE_EN        (se + 0x258)
#define SPI_TRANS_CFG           (se + 0x25c)
#define GENI_TX_PACKING_CFG0    (se + 0x260)
#define GENI_TX_PACKING_CFG1    (se + 0x264)
#define SPI_WORD_LEN            (se + 0x268)
#define SPI_TX_TRANS_LEN        (se + 0x26c)
#define SPI_RX_TRANS_LEN        (se + 0x270)
#define SPI_DELAY_COUNTERS      (se + 0x278)
#define GENI_RX_PACKING_CFG0    (se + 0x284)
#define GENI_RX_PACKING_CFG1    (se + 0x288)
#define GENI_M_CMD0             (se + 0x600)    /* [31:27] operation */
#define GENI_M_CMD_CTRL         (se + 0x604)    /* bit 2 cancel, bit 1 abort */
#define GENI_M_IRQ_STATUS       (se + 0x610)
#define GENI_M_IRQ_EN           (se + 0x614)
#define GENI_M_IRQ_CLEAR        (se + 0x618)
#define GENI_S_IRQ_CLEAR        (se + 0x648)
#define GENI_TX_FIFO            (se + 0x700)
#define GENI_RX_FIFO            (se + 0x780)
#define GENI_TX_FIFO_STATUS     (se + 0x800)    /* [27:0] words */
#define GENI_RX_FIFO_STATUS     (se + 0x804)    /* bit 31 last, [30:28] bytes of the last word, [24:0] words */
#define GENI_TX_WATERMARK       (se + 0x80c)
#define GENI_RX_WATERMARK       (se + 0x810)
#define GENI_RX_RFR_WATERMARK   (se + 0x814)
#define SE_DMA_TX_IRQ_CLR       (se + 0xc44)
#define SE_DMA_RX_IRQ_CLR       (se + 0xd44)
#define SE_GSI_EVENT_EN         (se + 0xe18)
#define SE_IRQ_EN               (se + 0xe1c)
#define SE_HW_PARAM_0           (se + 0xe24)    /* [21:16] FIFO depth in words */
#define SE_DMA_GENERAL_CFG      (se + 0xe30)

#define M_CMD_DONE              (1u << 0)
#define M_CMD_CANCEL            (1u << 4)
#define M_CMD_ABORT             (1u << 5)
#define M_ERRORS                ((1u << 1) | (1u << 2) | (1u << 3) | (1u << 24) | (1u << 25) | (1u << 28) | (1u << 29))
#define SPI_FULL_DUPLEX         (3u << 27)

static uint32_t fifo_depth;

/* NULL = ready; otherwise why the engine cannot be used */
static const char *
geni_spi_init(uint32_t hz)
{
	uint32_t divider;

	if ((rd(GENI_FW_REVISION_RO) >> 8 & 0xff) != 1) {
		return "the serial engine has no SPI firmware";
	}
	if (rd(GENI_IF_DISABLE_RO) & 1) {
		return "the serial engine's FIFO interface is disabled";
	}
	fifo_depth = rd(SE_HW_PARAM_0) >> 16 & 0x3f;
	if (fifo_depth < 4) {
		return "the serial engine reports no FIFO";
	}
	wr(SE_GSI_EVENT_EN, 0);
	wr(GENI_M_IRQ_CLEAR, 0xffffffff);
	wr(GENI_S_IRQ_CLEAR, 0xffffffff);
	wr(SE_DMA_TX_IRQ_CLR, 0xffffffff);
	wr(SE_DMA_RX_IRQ_CLR, 0xffffffff);
	wr(GENI_CGC_CTRL, rd(GENI_CGC_CTRL) | 0x7f);
	wr(SE_DMA_GENERAL_CFG, rd(SE_DMA_GENERAL_CFG) | 0xf);
	wr(GENI_OUTPUT_CTRL, 0x7f);
	wr(GENI_FORCE_DEFAULT, 1);
	wr(GENI_DMA_MODE_EN, rd(GENI_DMA_MODE_EN) & ~1u);
	wr(GENI_RX_WATERMARK, fifo_depth - 3);
	wr(GENI_RX_RFR_WATERMARK, fifo_depth - 2);
	wr(GENI_TX_WATERMARK, 0);
	/* the status bits we poll follow these enables; the interrupt itself stays masked at the GIC */
	wr(SE_IRQ_EN, rd(SE_IRQ_EN) | 0xf);
	wr(GENI_M_IRQ_EN, rd(GENI_M_IRQ_EN) | 0x7fc0007f);

	/* mode 0, chip select 0, 8 bits, most significant first, four bytes in a FIFO word (FTS: SPI mode 0) */
	wr(SPI_LOOPBACK, 0);
	wr(SPI_DEMUX_SEL, 0);
	wr(SPI_CPHA, 0);
	wr(SPI_CPOL, 0);
	wr(SPI_DEMUX_OUTPUT_INV, 0);
	wr(SPI_TRANS_CFG, rd(SPI_TRANS_CFG) & ~2u);
	wr(SPI_DELAY_COUNTERS, 0);
	wr(GENI_TX_PACKING_CFG0, 0x0007f8fe);
	wr(GENI_TX_PACKING_CFG1, 0x000ffefe);
	wr(GENI_RX_PACKING_CFG0, 0x0007f8fe);
	wr(GENI_RX_PACKING_CFG1, 0x000ffefe);
	wr(GENI_BYTE_GRAN, 0);
	wr(SPI_WORD_LEN, 8 - 4);

	divider = (se_source_hz + hz - 1) / hz;
	wr(GENI_CLK_SEL, se_clock_select & 7);
	wr(GENI_SER_M_CLK_CFG, (divider ? divider : 1) << 4 | 1);
	return NULL;
}

static void
geni_drain(uint8_t *rx, uint32_t length, uint32_t *received)
{
	uint32_t status = rd(GENI_RX_FIFO_STATUS), words = status & 0x01ffffff, bytes = words * 4;

	if (status & 0x80000000u) {
		uint32_t valid = status >> 28 & 7;

		if (valid && valid < 4) {
			bytes -= 4 - valid;
		}
	}
	for (uint32_t i = 0; i < words; i++) {
		uint32_t word = rd(GENI_RX_FIFO);

		for (int j = 0; j < 4 && bytes; j++, bytes--) {
			if (*received < length) {
				if (rx) {
					rx[*received] = (uint8_t)(word >> (8 * j));
				}
				(*received)++;
			}
		}
	}
}

static void
geni_recover(void)
{
	wr(GENI_M_CMD_CTRL, 1u << 2);
	for (int i = 0; i < 2000 && !(rd(GENI_M_IRQ_STATUS) & M_CMD_CANCEL); i++) {
		sleep_us(100);
	}
	if (!(rd(GENI_M_IRQ_STATUS) & M_CMD_CANCEL)) {
		wr(GENI_M_CMD_CTRL, 1u << 1);
		for (int i = 0; i < 2000 && !(rd(GENI_M_IRQ_STATUS) & M_CMD_ABORT); i++) {
			sleep_us(100);
		}
	}
	for (int i = 0; i < 64 && (rd(GENI_RX_FIFO_STATUS) & 0x01ffffff); i++) {
		(void)rd(GENI_RX_FIFO);
	}
	wr(GENI_M_IRQ_CLEAR, 0xffffffff);
}

/*
 * SPI by hand on the same pins, for when the serial engine cannot be driven from here: mayfly's TrustZone
 * loads engine 4's firmware with the FIFO interface disabled (GENI_IF_DISABLE_RO), so Android reaches it only
 * through GPI DMA. Mode 0: the clock idles low, both sides change data while it is low and sample on the rising
 * edge. A pin write takes a few hundred nanoseconds, so the bus runs at a few hundred kHz, which is plenty for
 * 8-byte events.
 */
static bool bitbang;
static uint32_t bitbang_ticks = 1;      /* half a clock period, in timebase ticks (19.2 MHz) */

static inline void
bitbang_wait(void)
{
	uint64_t until = mach_absolute_time() + bitbang_ticks;

	while (mach_absolute_time() < until) {
	}
}

static void
bitbang_pins(void)
{
	gpio_output(PIN_CS, true, PULL_NONE, 8);
	gpio_output(PIN_CLK, false, PULL_NONE, 8);
	gpio_output(PIN_MOSI, false, PULL_NONE, 8);
	gpio_configure(PIN_MISO, 0, PULL_NONE, 2, false);
}

static bool
bitbang_transfer(const uint8_t *tx, uint8_t *rx, uint32_t length)
{
	gpio_set(PIN_CS, false);
	bitbang_wait();
	for (uint32_t i = 0; i < length; i++) {
		uint8_t out = tx ? tx[i] : 0, in = 0;

		for (int bit = 7; bit >= 0; bit--) {
			gpio_set(PIN_MOSI, out >> bit & 1);
			bitbang_wait();
			gpio_set(PIN_CLK, true);
			bitbang_wait();
			in = (uint8_t)(in << 1 | gpio_get(PIN_MISO));
			gpio_set(PIN_CLK, false);
		}
		if (rx) {
			rx[i] = in;
		}
	}
	bitbang_wait();
	gpio_set(PIN_CS, true);
	sleep_us(10);                   /* SPI_DELAY_CS */
	return true;
}

/* One transfer with the chip selected throughout: `length` bytes out (zeros without tx) and as many in. */
static bool
spi_transfer(const uint8_t *tx, uint8_t *rx, uint32_t length)
{
	static unsigned complaints;
	uint32_t sent = 0, received = 0, status = 0;
	uint64_t deadline;

	if (bitbang) {
		return bitbang_transfer(tx, rx, length);
	}

	for (int i = 0; rd(GENI_STATUS) & 1; i++) {
		if (i > 2000) {
			geni_recover();
			break;
		}
		sleep_us(100);
	}
	wr(GENI_M_IRQ_CLEAR, 0xffffffff);
	wr(SPI_TX_TRANS_LEN, length);
	wr(SPI_RX_TRANS_LEN, length);
	wr(GENI_M_CMD0, SPI_FULL_DUPLEX);
	clock_interval_to_deadline(200, NSEC_PER_MSEC, &deadline);
	while (mach_absolute_time() < deadline) {
		while (sent < length && (rd(GENI_TX_FIFO_STATUS) & 0x0fffffff) + 1 < fifo_depth) {
			uint32_t word = 0;

			for (int j = 0; j < 4 && sent < length; j++, sent++) {
				word |= (uint32_t)(tx ? tx[sent] : 0) << (8 * j);
			}
			wr(GENI_TX_FIFO, word);
		}
		status = rd(GENI_M_IRQ_STATUS);
		geni_drain(rx, length, &received);
		if (status & M_ERRORS) {
			break;
		}
		if (status & M_CMD_DONE) {
			geni_drain(rx, length, &received);
			wr(GENI_M_IRQ_CLEAR, status);
			if (received == length) {
				sleep_us(10);   /* SPI_DELAY_CS: the chip wants 10 us before it is selected again */
				return true;
			}
			break;
		}
	}
	if (complaints++ < 8) {
		printf("stfts: SPI transfer of %u bytes failed: status %#x, sent %u, received %u\n", length, status, sent,
		    received);
	}
	geni_recover();
	return false;
}

/* ---- the FTS protocol (ftsHardware.h, ftsIO.c; SPI4, so every read starts with a dummy byte) ------- */

#define FTS_CMD_HW_REG_R        0xfb
#define FTS_CMD_HW_REG_W        0xfa
#define FTS_CMD_SCAN_MODE       0xa0
#define FIFO_CMD_READONE        0x87
#define FIFO_EVENT_SIZE         8
#define FIFO_DEPTH              32

#define ADDR_DCHIP_ID           0x20000000u     /* two bytes: 0x36 0x40 for the SALIXP variant */
#define ADDR_DCHIP_FW_VER       0x20000004u
#define ADDR_ICR                0x2000002du     /* bit 1: four-wire SPI */
#define ADDR_GPIO_DIRECTION     0x20000032u
#define ADDR_GPIO_PULLUP        0x20000034u
#define ADDR_GPIO_CONFIG_REG0   0x2000003du
#define ADDR_GPIO_CONFIG_REG3   0x2000003eu
#define DCHIP_ID_0              0x36
#define DCHIP_ID_1              0x40

#define EVT_ID_NOEVENT          0x00
#define EVT_ID_CONTROLLER_READY 0x03
#define EVT_ID_ENTER_POINT      0x13
#define EVT_ID_MOTION_POINT     0x23
#define EVT_ID_LEAVE_POINT      0x33
#define EVT_ID_STATUS_UPDATE    0x43
#define EVT_ID_USER_REPORT      0x53
#define EVT_ID_ERROR            0xf3

#define TOUCH_TYPE_FINGER       0x01
#define TOUCH_TYPE_GLOVE        0x02
#define TOUCH_TYPE_STYLUS       0x03
#define TOUCH_TYPE_PALM         0x04
#define TOUCH_TYPE_HOVER        0x05

#define MAX_TOUCHES             10              /* TOUCH_ID_MAX */
#define XFER_MAX                32

/* Read `length` bytes from a hardware register of the chip: op code, a 32-bit address, a dummy byte, the data. */
static bool
fts_reg_read(uint32_t address, uint8_t *data, uint32_t length)
{
	uint8_t tx[XFER_MAX] = { FTS_CMD_HW_REG_R, (uint8_t)(address >> 24), (uint8_t)(address >> 16), (uint8_t)(address >> 8),
		                 (uint8_t)address };
	uint8_t rx[XFER_MAX];

	if (length > XFER_MAX - 6 || !spi_transfer(tx, rx, 6 + length)) {
		return false;
	}
	memcpy(data, rx + 6, length);
	return true;
}

static bool
fts_reg_write(uint32_t address, uint8_t value)
{
	uint8_t tx[6] = { FTS_CMD_HW_REG_W, (uint8_t)(address >> 24), (uint8_t)(address >> 16), (uint8_t)(address >> 8),
		          (uint8_t)address, value };

	return spi_transfer(tx, NULL, sizeof(tx));
}

/* The oldest event in the chip's FIFO (all zeros, EVT_ID_NOEVENT, when there is none). */
static bool
fts_read_event(uint8_t event[FIFO_EVENT_SIZE])
{
	uint8_t tx[2 + FIFO_EVENT_SIZE] = { FIFO_CMD_READONE };
	uint8_t rx[2 + FIFO_EVENT_SIZE];

	if (!spi_transfer(tx, rx, sizeof(rx))) {
		return false;
	}
	memcpy(event, rx + 2, FIFO_EVENT_SIZE);
	return true;
}

/* Wait for an event with this ID, up to `ms` milliseconds; the events read meanwhile are dropped. */
static bool
fts_wait_event(uint8_t id, uint32_t ms, uint8_t event[FIFO_EVENT_SIZE])
{
	for (uint32_t waited = 0; waited < ms; waited += 2) {
		if (!fts_read_event(event)) {
			return false;
		}
		if (event[0] == id) {
			return true;
		}
		if (event[0] == EVT_ID_ERROR) {
			printf("stfts: error event %02x %02x %02x %02x %02x %02x %02x %02x\n", event[0], event[1], event[2],
			    event[3], event[4], event[5], event[6], event[7]);
		}
		sleep_ms(2);
	}
	return false;
}

/* fts_spi4_mode_set() of the vendor driver: what it does when the chip does not answer after a reset. */
static bool
fts_spi4_mode_set(void)
{
	return fts_reg_write(ADDR_GPIO_DIRECTION, 0x10) && fts_reg_write(ADDR_GPIO_PULLUP, 0x02) &&
	       fts_reg_write(ADDR_GPIO_CONFIG_REG3, 0x07) && fts_reg_write(ADDR_GPIO_CONFIG_REG0, 0x30) &&
	       fts_reg_write(ADDR_ICR, 0x02);
}

/* Reset by the pin and wait for "controller ready", as fts_system_reset() does with a reset GPIO. */
static bool
fts_reset(void)
{
	uint8_t event[FIFO_EVENT_SIZE];

	gpio_set(PIN_RESET, false);
	sleep_ms(10);
	gpio_set(PIN_RESET, true);
	return fts_wait_event(EVT_ID_CONTROLLER_READY, 1000, event);
}

static bool
fts_probe(void)
{
	uint8_t id[2] = { 0 }, version[4] = { 0 };
	bool ready = false;

	for (int attempt = 0; attempt < 3 && !ready; attempt++) {
		ready = fts_reset();
		if (!ready) {
			printf("stfts: no \"controller ready\" after reset %d; setting the chip's interface to four-wire SPI\n",
			    attempt + 1);
			if (!fts_spi4_mode_set()) {
				return false;
			}
			sleep_ms(1);
		}
	}
	if (!fts_reg_read(ADDR_DCHIP_ID, id, sizeof(id))) {
		return false;
	}
	(void)fts_reg_read(ADDR_DCHIP_FW_VER, version, sizeof(version));
	printf("stfts: chip ID %02x %02x (%s), firmware bytes %02x %02x %02x %02x, %s\n", id[0], id[1],
	    id[0] == DCHIP_ID_0 && id[1] == DCHIP_ID_1 ? "FTM5 SALIXP" : "NOT the expected 36 40", version[0], version[1],
	    version[2], version[3], ready ? "ready" : "never said it was ready");
	return ready && id[0] == DCHIP_ID_0 && id[1] == DCHIP_ID_1;
}

/* setScanMode(SCAN_MODE_ACTIVE, 0x01): "sense on", what fts_mode_handler() sends when the screen is on */
static bool
fts_sense_on(void)
{
	uint8_t tx[3] = { FTS_CMD_SCAN_MODE, 0x00, 0x01 };

	return spi_transfer(tx, NULL, sizeof(tx));
}

/* ---- touches --------------------------------------------------------------------------------------- */

/*
 * "fts,support-super-resolution": 16-bit coordinates, ten steps per pixel (fts,x-max 10800, fts,y-max 24000).
 * Measured on the phone: taps in the top left corner gave x 625-815, y 575; taps on the right edge x 9895-10075.
 * Boot-arg touch_sr=0 switches to the chip's other format (12-bit pixel coordinates, 1080 x 2400).
 */
static uint32_t super_resolution = 1;
static uint32_t max_x = 10800, max_y = 24000;
static uint32_t flip;                           /* boot-arg touch_flip: 1 = mirror left-right, 2 = mirror top-bottom */

static struct {
	bool down;
	uint32_t x, y;
} contact[MAX_TOUCHES];

static struct {
	uint32_t polls, events, bad, touches, errors, resets, last_x, last_y;
} seen;

static void
contact_down_or_move(uint32_t id, uint32_t x, uint32_t y)
{
	static unsigned shown;

	if (x > max_x - 1) {
		x = max_x - 1;
	}
	if (y > max_y - 1) {
		y = max_y - 1;
	}
	if (flip & 1) {
		x = max_x - 1 - x;
	}
	if (flip & 2) {
		y = max_y - 1 - y;
	}
	x = (uint32_t)(((uint64_t)x * 65535) / (max_x - 1));
	y = (uint32_t)(((uint64_t)y * 65535) / (max_y - 1));
	seen.touches++;
	seen.last_x = x;
	seen.last_y = y;
	if (!contact[id].down) {
		contact[id].down = true;
		hakios_touch_post(HAKIOS_TOUCH_DOWN, id, x, y);
	} else if (contact[id].x != x || contact[id].y != y) {
		hakios_touch_post(HAKIOS_TOUCH_MOVE, id, x, y);
	}
	contact[id].x = x;
	contact[id].y = y;
	if (shown++ < 8) {
		printf("stfts: finger %u at %u, %u\n", id, x, y);
	}
}

static void
contact_up(uint32_t id)
{
	if (contact[id].down) {
		contact[id].down = false;
		hakios_touch_post(HAKIOS_TOUCH_UP, id, contact[id].x, contact[id].y);
	}
}

static void
all_up(void)
{
	for (uint32_t id = 0; id < MAX_TOUCHES; id++) {
		contact_up(id);
	}
}

/* One event of the FIFO (fts_enter_pointer_event_handler(), fts_leave_pointer_event_handler()). */
static void
handle_event(const uint8_t *e)
{
	uint32_t id = e[1] >> 4, type = e[1] & 0x0f;
	static unsigned downs;

	if (e[0] == EVT_ID_ENTER_POINT && downs++ < 8) {
		printf("stfts: down %u, raw %02x %02x %02x %02x\n", downs, e[2], e[3], e[4], e[5]);
	}
	switch (e[0]) {
	case EVT_ID_ENTER_POINT:
	case EVT_ID_MOTION_POINT:
		if (id >= MAX_TOUCHES) {
			break;
		}
		if (type == TOUCH_TYPE_FINGER || type == TOUCH_TYPE_GLOVE || type == TOUCH_TYPE_STYLUS || type == TOUCH_TYPE_PALM) {
			if (super_resolution) {
				/* 16-bit coordinates, x in bytes 2-3, y in bytes 4-5 */
				contact_down_or_move(id, (uint32_t)e[3] << 8 | e[2], (uint32_t)e[5] << 8 | e[4]);
			} else {
				/* 12 bits each: x = byte 2 and the low half of byte 3, y = byte 4 and the high half of byte 3 */
				contact_down_or_move(id, ((uint32_t)e[3] & 0x0f) << 8 | e[2], (uint32_t)e[4] << 4 | e[3] >> 4);
			}
		}
		break;
	case EVT_ID_LEAVE_POINT:
		/* 0xb5 in the second byte is Xiaomi's fingerprint-area report, not a finger of its own */
		if (e[1] != 0xb5 && id < MAX_TOUCHES) {
			contact_up(id);
		}
		break;
	case EVT_ID_CONTROLLER_READY:
		/* the chip has reset itself (ESD, a watchdog): it comes back with sensing off */
		seen.resets++;
		all_up();
		printf("stfts: the chip reset itself; sensing on again\n");
		(void)fts_sense_on();
		break;
	case EVT_ID_ERROR:
		if (seen.errors++ < 8) {
			printf("stfts: error event %02x %02x %02x %02x %02x %02x %02x %02x\n", e[0], e[1], e[2], e[3], e[4], e[5],
			    e[6], e[7]);
		}
		break;
	default:
		break;
	}
}

/* Everything waiting in the FIFO, up to its depth. */
static void
poll_events(void)
{
	static unsigned bad;
	uint8_t event[FIFO_EVENT_SIZE];

	seen.polls++;
	for (int i = 0; i < FIFO_DEPTH; i++) {
		if (!fts_read_event(event)) {
			seen.bad++;
			if (bad++ < 8) {
				printf("stfts: SPI read of the event FIFO failed\n");
			}
			return;
		}
		if (event[0] == EVT_ID_NOEVENT) {
			return;
		}
		seen.events++;
		if (seen.events <= 4) {
			printf("stfts: event %02x %02x %02x %02x %02x %02x %02x %02x\n", event[0], event[1], event[2], event[3],
			    event[4], event[5], event[6], event[7]);
		}
		handle_event(event);
	}
}

/* ---- all of it together ---------------------------------------------------------------------------- */

static void
show_state(const char *when)
{
	printf("stfts: %s: votes %#x, clocks %#x %#x %#x, rate cfg %#x dfs %#x; pins miso/mosi/clk/cs cfg %#x %#x %#x %#x,"
	    " reset %#x/%u, int %#x/%u\n", when, rd(GCC_VOTE), rd(GCC_QUP0_M_AHB_CBCR), rd(GCC_QUP0_S_AHB_CBCR),
	    rd(GCC_QUP0_SE4_CBCR), rd(GCC_QUP0_SE4_CFG), rd(GCC_QUP0_SE4_DFSR), rd(GPIO_CFG(PIN_MISO)), rd(GPIO_CFG(PIN_MOSI)),
	    rd(GPIO_CFG(PIN_CLK)), rd(GPIO_CFG(PIN_CS)), rd(GPIO_CFG(PIN_RESET)), rd(GPIO_IO(PIN_RESET)), rd(GPIO_CFG(PIN_INT)),
	    rd(GPIO_IO(PIN_INT)));
}

/* Power the chip up from nothing, in the vendor driver's order (fts_enable_reg(), then 3 ms). */
static bool
power_on(void)
{
	gpio_output(PIN_RESET, false, PULL_UP, 8);
	gpio_configure(PIN_INT, 0, PULL_UP, 8, false);
	if (!ldo_on("ldoc2", 1800)) {
		return false;
	}
	/* without its digital supply the chip must not get the analog one */
	if (!ldo_on("ldoc7", 3200)) {
		return false;
	}
	sleep_ms(5);
	return true;
}

static void
use_engine_pins(void)
{
	gpio_configure(PIN_MISO, PIN_FUNCTION_QUP4, PULL_NONE, 6, false);
	gpio_configure(PIN_MOSI, PIN_FUNCTION_QUP4, PULL_NONE, 6, false);
	gpio_configure(PIN_CLK, PIN_FUNCTION_QUP4, PULL_NONE, 6, false);
	gpio_configure(PIN_CS, PIN_FUNCTION_QUP4, PULL_UP, 16, false);
}

static bool
bring_up(void)
{
	const char *problem;
	uint32_t spi_hz = 1000000;

	show_state("as found");
	if (!power_on()) {
		printf("stfts: the supplies are not on; the chip is left alone\n");
		return false;
	}
	if (!clocks_on()) {
		return false;
	}
	printf("stfts: serial engine: firmware %#x, interface %#x, parameters %#x, source clock %u Hz (select %u)\n",
	    rd(GENI_FW_REVISION_RO), rd(GENI_IF_DISABLE_RO), rd(SE_HW_PARAM_0), se_source_hz, se_clock_select);
	/* boot-arg touch_spi_hz=N: the bus at N Hz (Android's device tree gives the chip 12 MHz) */
	PE_parse_boot_argn("touch_spi_hz", &spi_hz, sizeof(spi_hz));
	if (spi_hz < 100000 || spi_hz > 12000000) {
		spi_hz = 1000000;
	}
	problem = geni_spi_init(spi_hz);
	if (problem != NULL) {
		/* boot-arg touch_bitbang_ticks=N: half a clock period in 19.2 MHz ticks */
		PE_parse_boot_argn("touch_bitbang_ticks", &bitbang_ticks, sizeof(bitbang_ticks));
		printf("stfts: %s; SPI by hand on gpio16-19 instead (half period %u ticks)\n", problem, bitbang_ticks);
		bitbang = true;
		bitbang_pins();
	} else {
		use_engine_pins();
		printf("stfts: the bus at %u Hz\n", spi_hz);
	}
	show_state("powered, on the serial engine");
	if (!fts_probe()) {
		show_state("no answer");
		return false;
	}
	if (!fts_sense_on()) {
		printf("stfts: \"sense on\" could not be sent\n");
		return false;
	}
	return true;
}

static void
stfts_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint32_t poll_us = 8000;
	unsigned idle = 0, idle_looks;

	PE_parse_boot_argn("touch_poll_us", &poll_us, sizeof(poll_us));
	if (poll_us < 250 || poll_us > 100000) {
		poll_us = 8000;
	}
	idle_looks = 80000 / poll_us;   /* the FIFO read every 80 ms even with the line high, in case it is missed */

	tlmm = ml_io_map(TLMM_PHYS, TLMM_SIZE);
	gcc = ml_io_map(GCC_PHYS, GCC_SIZE);
	se = ml_io_map(SE_PHYS, SE_SIZE);
	rsc = ml_io_map(RSC_PHYS, RSC_SIZE);
	cmd_db = ml_io_map_wcomb(CMD_DB_PHYS, CMD_DB_SIZE);      /* memory, not registers: no alignment rules */
	if (!tlmm || !gcc || !se || !rsc || !cmd_db) {
		printf("stfts: could not map the hardware\n");
		thread_terminate(current_thread());
		return;
	}
	if (!bring_up()) {
		printf("stfts: no touch screen\n");
		thread_terminate(current_thread());
		return;
	}
	printf("stfts: touch screen ready; the interrupt line is looked at every %u us\n", poll_us);
	for (unsigned tick = 1;; tick++) {
		if (!gpio_get(PIN_INT)) {
			idle = 0;
			poll_events();
		} else if (++idle >= idle_looks) {
			idle = 0;
			poll_events();
		}
		if (tick % (60 * (1000000 / poll_us)) == 0) {
			printf("stfts: so far %u events in %u looks (%u bad, %u errors, %u resets), %u finger positions, the last at"
			    " %u, %u; interrupt line %s\n", seen.events, seen.polls, seen.bad, seen.errors, seen.resets, seen.touches,
			    seen.last_x, seen.last_y, gpio_get(PIN_INT) ? "high" : "low");
		}
		sleep_us(poll_us);
	}
}

static void
stfts_init(void)
{
	uint32_t enabled = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("touchscreen", &enabled, sizeof(enabled)) || !enabled) {
		return;                 /* nothing is touched unless a boot-argument asks */
	}
	PE_parse_boot_argn("touch_flip", &flip, sizeof(flip));
	if (PE_parse_boot_argn("touch_sr", &super_resolution, sizeof(super_resolution)) && !super_resolution) {
		max_x = 1080;
		max_y = 2400;
	}
	if (kernel_thread_start(stfts_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, stfts_init);
