/*
 * Bluetooth half of the Xiaomi 12S Pro's QCA6490, over its UART: the chip brought up and talking HCI. Boot-arg qcabt=N:
 *
 *   1  clocks of QUP serial engine 20 on, what the engine is, how the pins stand (nothing goes to the chip)
 *   2  + the engine set up as a UART, in its internal loopback: bytes sent must come back (still nothing to the chip)
 *   3  + supplies, BT_EN, the real pins; the chip's version and board id (Qualcomm's "EDL" vendor commands)
 *   4  + rampatch and NVM downloaded, HCI reset, the chip's address and version read
 *   5  + a few seconds of BLE scanning, to see the radio hear something
 *   6  + the chip handed to iOS: bluetoothd's transport (hakios.qcabt_h4.c), the Broadcom shim (hakios.qcabt_shim.c)
 *
 * From the phone's device tree: qupv3_se20_4uart at 0x894000 (gpio76 CTS, 77 RTS, 78 TX, 79 RX, all function qup20, which
 * is function 1), bt-reset gpio81, sw-ctrl gpio82, the same supplies as the Wi-Fi half.
 * From Linux: drivers/tty/serial/qcom_geni_serial.c (FIFO mode UART), drivers/bluetooth/{hci_qca,btqca}.c (WCN6855's
 * path: the QCA6490 is the same "HSP" family, hpbtfw21.tlv + hpnv21.*), drivers/clk/qcom/gcc-waipio.c.
 *
 * Polled: no interrupt, no DMA, so no SMMU either. The chip starts at 115200 baud and is moved to qcabt_baud (3000000; 115200: stays),
 * as hci_qca.c does it, before the rampatch; at that speed polling carries a 64 byte FIFO a millisecond, about 64 KB/s, enough for A2DP.
 * Unlike Linux the NVM's in-band sleep and deep sleep bits are cleared: the chip stays awake and nothing here has to wake it.
 *
 * Boot-args: qcabt=N, qcabt_delay=<s> before beginning (RPMh's regulators belong to touch's start; 20).
 * Firmware: fw/hpbtfw21.tlv and fw/hpnv21* in the small image (userland/build.sh, from ../fw-bt/image).
 */
#include <stdint.h>
#include <string.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/hakios.qcabt.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);
extern kern_return_t kernel_thread_start(thread_continue_t continuation, void *parameter, thread_t *new_thread);
extern void thread_deallocate(thread_t thread);
extern void IOSleep(unsigned milliseconds);
extern void IODelay(unsigned microseconds);
extern void loader_log_putc(char c);
extern bool hakios_unicorn_vreg_on(const char *name, uint32_t millivolts, bool high_power);
extern const uint8_t *hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);

void hakios_qcabt_init(void);

static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[240];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	return printf("%s", line);
}

static vm_offset_t tlmm, gcc, se, qup;
static uint32_t level;

static inline uint32_t
rd(vm_offset_t address)
{
	return *(volatile uint32_t *)address;
}

static inline void
wr(vm_offset_t address, uint32_t value)
{
	*(volatile uint32_t *)address = value;
	__asm__ volatile ("dsb sy");
}

/* ---- pins ------------------------------------------------------------------------------------------- */

#define GPIO_CFG(n)             (tlmm + 0x1000UL * (n))         /* [1:0] pull, [5:2] function, [8:6] drive, [9] output */
#define GPIO_IO(n)              (tlmm + 0x1000UL * (n) + 4)     /* bit 0 in, bit 1 out */
#define PIN_CTS                 76
#define PIN_RTS                 77
#define PIN_TX                  78
#define PIN_RX                  79
#define PIN_BT_EN               81
#define PIN_SW_CTRL             82
#define FUNC_QUP20              1
#define PULL_NONE               0
#define PULL_DOWN               1
#define PULL_UP                 3
#define DRIVE_2MA               0

static void
pin_look(const char *when)
{
	static const uint32_t pins[] = { PIN_CTS, PIN_RTS, PIN_TX, PIN_RX, PIN_BT_EN, PIN_SW_CTRL };

	for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
		uint32_t cfg = rd(GPIO_CFG(pins[i])), io = rd(GPIO_IO(pins[i]));

		say("qcabt: %s: gpio%u: function %u, pull %u, %s, reads %u\n", when, pins[i], cfg >> 2 & 15, cfg & 3,
		    (cfg & 1U << 9) ? ((io & 2) ? "drives high" : "drives low") : "input", io & 1);
	}
}

static void
pin_function(uint32_t pin, uint32_t pull)
{
	wr(GPIO_CFG(pin), DRIVE_2MA << 6 | FUNC_QUP20 << 2 | pull);
}

static void
pin_output(uint32_t pin, bool high)
{
	wr(GPIO_IO(pin), high ? 2 : 0);
	wr(GPIO_CFG(pin), (rd(GPIO_CFG(pin)) & ~(15U << 2)) | 1U << 9);
	wr(GPIO_IO(pin), high ? 2 : 0);
}

/* ---- clocks (gcc-waipio.c: gcc_qupv3_wrap2_s5_clk and its root, wrapper 2's clocks) ------------------- */

#define GCC_PHYS                0x00100000UL
#define GCC_SIZE                0x70000UL
#define GCC_VOTE                (gcc + 0x62010)
#define VOTE_CORE               (1U << 0)
#define VOTE_S_AHB              (1U << 1)
#define VOTE_M_AHB              (1U << 2)
#define VOTE_CORE_2X            (1U << 3)
#define VOTE_SE20               (1U << 9)
#define SE20_CBCR               (gcc + 0x2e610)
#define SE20_CMD                (gcc + 0x2e618)
#define SE20_CFG                (gcc + 0x2e61c)
#define SE20_M                  (gcc + 0x2e620)
#define SE20_N                  (gcc + 0x2e624)
#define SE20_D                  (gcc + 0x2e628)
#define SE20_DFSR               (gcc + 0x2e62c)
#define SE20_PERF_CFG(l)        (gcc + 0x2e634 + 4 * (l))
#define SE20_PERF_M(l)          (gcc + 0x2e674 + 4 * (l))
#define SE20_PERF_N(l)          (gcc + 0x2e6b4 + 4 * (l))
#define CLOCK_OFF               (1U << 31)
#define XO_HZ                   19200000U
#define UART_BAUD               115200U

static uint32_t se_hz, se_select;

/* gcc_parent_map_0: 0 the crystal, 1 GPLL0 (600 MHz), 6 GPLL0 even (300 MHz) */
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

/* A rate of the frequency table made of GPLL0 even (300 MHz) * m / n, dual edge */
static bool
root_rate(uint32_t hz, uint32_t m, uint32_t n)
{
	wr(SE20_M, m);
	wr(SE20_N, ~(n - m) & 0xffff);
	wr(SE20_D, ~n & 0xffff);
	wr(SE20_CFG, (rd(SE20_CFG) & ~0x371fU) | 6U << 8 | 2U << 12);
	wr(SE20_CMD, rd(SE20_CMD) | 1);
	for (int i = 0; i < 50 && (rd(SE20_CMD) & 1); i++) {
		IOSleep(1);
	}
	if (rd(SE20_CMD) & 1) {
		say("qcabt: the engine's clock does not take its configuration (%#x)\n", rd(SE20_CMD));
		return false;
	}
	se_hz = hz;
	return true;
}

static bool
clocks_on(uint32_t wanted_hz)
{
	wr(GCC_VOTE, rd(GCC_VOTE) | VOTE_CORE | VOTE_CORE_2X | VOTE_M_AHB | VOTE_S_AHB);
	if (rd(SE20_DFSR) & 1) {
		/* preset rates, chosen by the engine: the lowest that divides down to the wanted one exactly */
		for (uint32_t l = 0; l < 8; l++) {
			uint32_t hz = rcg_rate(rd(SE20_PERF_CFG(l)), rd(SE20_PERF_M(l)), rd(SE20_PERF_N(l)));

			say("qcabt: clock preset %u: %u Hz\n", l, hz);
			if (hz != 0 && hz % wanted_hz == 0 && (se_hz == 0 || hz < se_hz)) {
				se_hz = hz;
				se_select = l;
			}
		}
		if (se_hz == 0) {
			say("qcabt: no preset clock divides to %u Hz\n", wanted_hz);
			return false;
		}
	} else {
		if (!root_rate(7372800, 384, 15625)) {
			return false;
		}
		se_select = 0;
	}
	wr(GCC_VOTE, rd(GCC_VOTE) | VOTE_SE20);
	wr(SE20_CMD, rd(SE20_CMD) | 2);                         /* ROOT_EN: a root nobody asked for stays off */
	for (int i = 0; i < 50; i++) {
		if (!(rd(SE20_CBCR) & CLOCK_OFF)) {
			say("qcabt: engine clock on at %u Hz (select %u): cbcr %#x, root %#x\n", se_hz, se_select, rd(SE20_CBCR), rd(SE20_CMD));
			return true;
		}
		IOSleep(1);
	}
	say("qcabt: engine clock reads off (cbcr %#x, root %#x): NOT touching the engine, its registers would hang the bus\n",
	    rd(SE20_CBCR), rd(SE20_CMD));
	return false;
}

/* ---- the UART (qcom_geni_serial.c, FIFO mode) ------------------------------------------------------- */

#define SE_PHYS                 0x00894000UL
#define SE_SIZE                 0x4000UL
#define QUP_PHYS                0x008c0000UL
#define QUP_HW_VER              (qup + 0x4)
#define GENI_FORCE_DEFAULT      (se + 0x020)
#define GENI_OUTPUT_CTRL        (se + 0x024)
#define GENI_CGC_CTRL           (se + 0x028)
#define GENI_STATUS             (se + 0x040)
#define GENI_SER_M_CLK_CFG      (se + 0x048)
#define GENI_SER_S_CLK_CFG      (se + 0x04c)
#define GENI_IF_DISABLE_RO      (se + 0x064)
#define GENI_FW_REVISION_RO     (se + 0x068)    /* [15:8] protocol: 2 = UART */
#define GENI_CLK_SEL            (se + 0x07c)
#define UART_LOOPBACK           (se + 0x22c)
#define GENI_BYTE_GRAN          (se + 0x254)
#define GENI_DMA_MODE_EN        (se + 0x258)
#define UART_TX_TRANS_CFG       (se + 0x25c)    /* bit 1: ignore CTS */
#define UART_MANUAL_RFR         (se + 0x2ac)    /* bit 31 manual, bit 1 not ready */
#define GENI_TX_PACKING_CFG0    (se + 0x260)
#define GENI_TX_PACKING_CFG1    (se + 0x264)
#define UART_TX_WORD_LEN        (se + 0x268)
#define UART_TX_STOP_BIT_LEN    (se + 0x26c)
#define UART_TX_TRANS_LEN       (se + 0x270)
#define UART_RX_TRANS_CFG       (se + 0x280)
#define GENI_RX_PACKING_CFG0    (se + 0x284)
#define GENI_RX_PACKING_CFG1    (se + 0x288)
#define UART_RX_WORD_LEN        (se + 0x28c)
#define UART_RX_STALE_CNT       (se + 0x294)
#define UART_TX_PARITY_CFG      (se + 0x2a4)
#define UART_RX_PARITY_CFG      (se + 0x2a8)
#define GENI_M_CMD0             (se + 0x600)
#define GENI_M_CMD_CTRL         (se + 0x604)    /* bit 2 cancel */
#define GENI_M_IRQ_STATUS       (se + 0x610)
#define GENI_M_IRQ_EN           (se + 0x614)
#define GENI_M_IRQ_CLEAR        (se + 0x618)
#define GENI_S_CMD0             (se + 0x630)
#define GENI_S_IRQ_STATUS       (se + 0x640)
#define GENI_S_IRQ_EN           (se + 0x644)
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
#define SE_HW_PARAM_1           (se + 0xe28)
#define SE_DMA_GENERAL_CFG      (se + 0xe30)

#define M_CMD_DONE              (1U << 0)
#define M_CMD_CANCEL            (1U << 4)
#define M_TX_START              (1U << 27)
#define S_RX_START              (1U << 27)

static uint32_t fifo_depth;

/* What the engine is: only reads, with its clocks on. False if it can't be a FIFO UART. */
static bool
engine_look(void)
{
	uint32_t revision = rd(GENI_FW_REVISION_RO), disabled = rd(GENI_IF_DISABLE_RO);

	fifo_depth = rd(SE_HW_PARAM_0) >> 16 & 0x3f;
	say("qcabt: QUP version %#x; engine: firmware %#x (protocol %u), FIFO interface %s, FIFO %u words, param1 %#x, status %#x\n",
	    rd(QUP_HW_VER), revision, revision >> 8 & 0xff, (disabled & 1) ? "DISABLED" : "there", fifo_depth, rd(SE_HW_PARAM_1), rd(GENI_STATUS));
	if ((revision >> 8 & 0xff) != 2) {
		say("qcabt: the engine has no UART firmware\n");
		return false;
	}
	if ((disabled & 1) || fifo_depth < 4) {
		say("qcabt: the engine's FIFO cannot be used\n");
		return false;
	}
	return true;
}

static void
uart_setup(uint32_t loopback)
{
	/* from QUP 2.5 on the engine oversamples 16 times, before that 32 */
	uint32_t oversampling = (rd(QUP_HW_VER) >= 0x20050000) ? 16 : 32;
	uint32_t divider = se_hz / (UART_BAUD * oversampling);

	wr(SE_GSI_EVENT_EN, 0);
	wr(GENI_M_IRQ_CLEAR, 0xffffffff);
	wr(GENI_S_IRQ_CLEAR, 0xffffffff);
	wr(SE_DMA_TX_IRQ_CLR, 0xffffffff);
	wr(SE_DMA_RX_IRQ_CLR, 0xffffffff);
	wr(GENI_CGC_CTRL, rd(GENI_CGC_CTRL) | 0x7f);
	wr(SE_DMA_GENERAL_CFG, rd(SE_DMA_GENERAL_CFG) | 0xf);
	wr(GENI_OUTPUT_CTRL, 0x7f);
	wr(GENI_FORCE_DEFAULT, 1);
	wr(GENI_DMA_MODE_EN, rd(GENI_DMA_MODE_EN) & ~1U);
	wr(GENI_RX_WATERMARK, 2);
	wr(GENI_RX_RFR_WATERMARK, fifo_depth - 2);
	wr(GENI_TX_WATERMARK, 2);
	/* the status bits we poll follow these enables; the interrupt itself stays masked at the GIC */
	wr(SE_IRQ_EN, rd(SE_IRQ_EN) | 0xf);
	wr(GENI_M_IRQ_EN, rd(GENI_M_IRQ_EN) | 0x7fc0007f);
	wr(GENI_S_IRQ_EN, rd(GENI_S_IRQ_EN) | 0x0f001e37);

	/* geni_se_config_packing(8 bits, 4 bytes in a word, least significant first) */
	wr(GENI_TX_PACKING_CFG0, 0x0004380e);
	wr(GENI_TX_PACKING_CFG1, 0x000c3e0e);
	wr(GENI_RX_PACKING_CFG0, 0x0004380e);
	wr(GENI_RX_PACKING_CFG1, 0x000c3e0e);
	wr(GENI_BYTE_GRAN, 0);

	wr(UART_RX_STALE_CNT, 10 * 16);
	wr(UART_LOOPBACK, loopback);
	wr(UART_TX_TRANS_CFG, 0);                               /* no parity; CTS honoured */
	wr(UART_RX_TRANS_CFG, 0);
	wr(UART_TX_PARITY_CFG, 0);
	wr(UART_RX_PARITY_CFG, 0);
	wr(UART_TX_WORD_LEN, 8);
	wr(UART_RX_WORD_LEN, 8);
	wr(UART_TX_STOP_BIT_LEN, 0);
	wr(GENI_CLK_SEL, se_select & 7);
	wr(GENI_SER_M_CLK_CFG, divider << 4 | 1);
	wr(GENI_SER_S_CLK_CFG, divider << 4 | 1);
	wr(GENI_S_CMD0, S_RX_START);
	say("qcabt: UART at %u baud: source %u Hz / %u oversampling / divider %u; loopback %u\n", UART_BAUD, se_hz, oversampling, divider, loopback);
}

/* All of `data` out; false if the chip's CTS holds it longer than a second, the transfer cancelled then. */
static bool
uart_send(const uint8_t *data, uint32_t length)
{
	uint32_t at = 0;
	int waited = 0;

	wr(GENI_M_IRQ_CLEAR, M_CMD_DONE);
	wr(UART_TX_TRANS_LEN, length);
	wr(GENI_M_CMD0, M_TX_START);
	while (at < length) {
		uint32_t used = rd(GENI_TX_FIFO_STATUS) & 0x0fffffff;

		if (used >= fifo_depth) {
			if (++waited > 5000) {
				goto stuck;
			}
			IODelay(200);   /* (at 3 Mbaud a FIFO goes in 200 us: no sleeping through it) */
			continue;
		}
		for (; used < fifo_depth && at < length; used++) {
			uint32_t word = 0;

			for (int j = 0; j < 4 && at < length; j++) {
				word |= (uint32_t)data[at++] << (8 * j);
			}
			wr(GENI_TX_FIFO, word);
		}
		waited = 0;
	}
	while (!(rd(GENI_M_IRQ_STATUS) & M_CMD_DONE)) {
		if (++waited > 5000) {
			goto stuck;
		}
		IODelay(200);
	}
	wr(GENI_M_IRQ_CLEAR, M_CMD_DONE);
	return true;
stuck:
	say("qcabt: sending stuck after %u of %u bytes: tx fifo %#x, m irq %#x, status %#x, CTS gpio%u reads %u\n", at, length,
	    rd(GENI_TX_FIFO_STATUS), rd(GENI_M_IRQ_STATUS), rd(GENI_STATUS), PIN_CTS, rd(GPIO_IO(PIN_CTS)) & 1);
	wr(GENI_M_CMD_CTRL, 1U << 2);
	for (int i = 0; i < 100 && !(rd(GENI_M_IRQ_STATUS) & M_CMD_CANCEL); i++) {
		IOSleep(1);
	}
	wr(GENI_M_IRQ_CLEAR, 0xffffffff);
	return false;
}

static uint32_t line_baud = UART_BAUD;

/* The line's speed on our side. The engine's clock: 115200 from 7.3728 MHz / 4, 3000000 from 48 MHz / 1, at 16 times oversampling. */
static bool
line_set(uint32_t baud)
{
	uint32_t divider;

	if (baud == 3000000) {
		if (!root_rate(48000000, 4, 25)) {
			return false;
		}
		divider = 1;
	} else {
		if (!root_rate(7372800, 384, 15625)) {
			return false;
		}
		divider = 4;
		baud = UART_BAUD;
	}
	wr(GENI_SER_M_CLK_CFG, divider << 4 | 1);
	wr(GENI_SER_S_CLK_CFG, divider << 4 | 1);
	line_baud = baud;
	return true;
}

static uint8_t rx_ring[4096];
static uint32_t rx_head, rx_tail;
static uint64_t rx_total;

static void
uart_drain(void)
{
	uint32_t status = rd(GENI_RX_FIFO_STATUS), words = status & 0x01ffffff;

	while (words != 0) {
		uint32_t bytes = 4;

		/* no room for a whole word: it stays in the FIFO, RFR holds the chip back, nothing is lost */
		if ((rx_tail + sizeof(rx_ring) - rx_head - 1) % sizeof(rx_ring) < 4) {
			break;
		}

		/* the last word of a stale or finished read may be partial */
		if (words == 1 && (status & 0x80000000U) && (status >> 28 & 7) != 0) {
			bytes = status >> 28 & 7;
		}
		uint32_t word = rd(GENI_RX_FIFO);

		for (uint32_t j = 0; j < bytes && j < 4; j++) {
			uint32_t next = (rx_head + 1) % sizeof(rx_ring);

			if (next != rx_tail) {
				rx_ring[rx_head] = (uint8_t)(word >> (8 * j));
				rx_head = next;
			}
			rx_total++;
		}
		words--;
	}
}

/* One byte within `ms` milliseconds */
static bool
uart_byte(uint8_t *byte, uint32_t ms)
{
	for (uint32_t waited = 0;; waited++) {
		uart_drain();
		if (rx_tail != rx_head) {
			*byte = rx_ring[rx_tail];
			rx_tail = (rx_tail + 1) % sizeof(rx_ring);
			return true;
		}
		if (waited >= ms) {
			return false;
		}
		IOSleep(1);
	}
}

/* Internal loopback: proves the engine, its clock and its rate without the chip (compares what returns). */
static bool
loopback_test(void)
{
	static const uint8_t pattern[] = { 0x55, 0xaa, 0x01, 0x80, 0xc3, 0x3c, 0xff, 0x00, 0x12, 0x34, 0x56 };
	bool good = uart_send(pattern, sizeof(pattern));

	for (unsigned i = 0; good && i < sizeof(pattern); i++) {
		uint8_t byte = 0;

		if (!uart_byte(&byte, 200) || byte != pattern[i]) {
			say("qcabt: loopback: byte %u is %#x, should be %#x (rx status %#x, s irq %#x)\n", i, byte, pattern[i],
			    rd(GENI_RX_FIFO_STATUS), rd(GENI_S_IRQ_STATUS));
			good = false;
		}
	}
	say("qcabt: loopback %s\n", good ? "GOOD: THE UART WORKS" : "FAILED");
	return good;
}

/* ---- HCI over H4 ------------------------------------------------------------------------------------- */

#define H4_COMMAND              0x01
#define H4_EVENT                0x04
#define EV_COMMAND_COMPLETE     0x0e
#define EV_COMMAND_STATUS       0x0f
#define EV_LE_META              0x3e
#define EV_VENDOR               0xff
#define EDL_PATCH_OPCODE        0xfc00
#define EDL_PATCH_VER_REQ       0x19
#define EDL_PATCH_TLV_REQ       0x1e
#define EDL_GET_BUILD_INFO      0x20
#define EDL_GET_BID_REQ         0x23
#define MAX_TLV_SEGMENT         243
#define EDL_TAG_HCI             17
#define EDL_TAG_DEEP_SLEEP      27

static uint8_t event[260];              /* code, length, parameters */
static uint32_t ibs_wakes, ibs_sleeps;

static void
hex_line(const char *what, const uint8_t *data, uint32_t length)
{
	char text[3 * 48 + 1];
	uint32_t n = length < 48 ? length : 48;

	for (uint32_t i = 0; i < n; i++) {
		snprintf(text + 3 * i, 4, "%02x ", data[i]);
	}
	text[3 * n] = 0;
	say("qcabt: %s (%u):%s %s\n", what, length, length > n ? " first" : "", text);
}

/* The next HCI event into event[], skipping what isn't one. Returns its parameters' length, or -1. */
static int
event_next(uint32_t ms)
{
	for (;;) {
		uint8_t type;

		if (!uart_byte(&type, ms)) {
			return -1;
		}
		if (type == H4_EVENT) {
			break;
		}
		/*
		 * In-band sleep's single bytes (hci_qca.c): the chip repeats "wake" (0xfd) until it gets its ack (0xfc), and only
		 * then sends what it has. It does this even with the NVM's bits cleared, so it is answered, as Linux does.
		 */
		if (type == 0xfd) {
			static const uint8_t wake_ack = 0xfc;

			ibs_wakes++;
			uart_send(&wake_ack, 1);
			continue;
		}
		if (type == 0xfe) {
			ibs_sleeps++;
			continue;
		}
		if (type == 0xfc) {
			continue;
		}
		say("qcabt: skipping byte %#x on the line\n", type);
	}
	if (!uart_byte(&event[0], ms) || !uart_byte(&event[1], ms)) {
		return -1;
	}
	for (uint32_t i = 0; i < event[1]; i++) {
		if (!uart_byte(&event[2 + i], ms)) {
			say("qcabt: event %#x cut short at %u of %u\n", event[0], i, event[1]);
			return -1;
		}
	}
	return event[1];
}

static bool
command_send(uint16_t opcode, const uint8_t *parameters, uint8_t length)
{
	uint8_t packet[4 + 255];

	packet[0] = H4_COMMAND;
	packet[1] = opcode & 0xff;
	packet[2] = opcode >> 8;
	packet[3] = length;
	if (length != 0) {
		memcpy(packet + 4, parameters, length);
	}
	return uart_send(packet, 4U + length);
}

/*
 * A command and its answer: the parameters of a command complete for it (after the opcode), or of the vendor event,
 * which old chips answer with instead. A pointer into event[] and the length, or NULL.
 */
static const uint8_t *
command(uint16_t opcode, const uint8_t *parameters, uint8_t length, int *answer_length, uint32_t ms)
{
	if (!command_send(opcode, parameters, length)) {
		return NULL;
	}
	for (;;) {
		int got = event_next(ms);

		if (got < 0) {
			say("qcabt: no answer to command %#x in %u ms (received %llu bytes so far)\n", opcode, ms, rx_total);
			return NULL;
		}
		if (event[0] == EV_COMMAND_COMPLETE && got >= 3 && (event[3] | event[4] << 8) == opcode) {
			*answer_length = got - 3;
			return event + 5;
		}
		if (event[0] == EV_VENDOR && got >= 2) {
			*answer_length = got;
			return event + 2;
		}
		hex_line("other event while waiting", event, 2U + (uint32_t)got);
	}
}

static uint16_t chip_rom, chip_board;
static uint32_t chip_soc;

static bool
chip_version(void)
{
	uint8_t request = EDL_PATCH_VER_REQ;
	int length = 0;
	const uint8_t *answer = command(EDL_PATCH_OPCODE, &request, 1, &length, 1000);

	if (answer == NULL) {
		return false;
	}
	hex_line("version answer", answer, (uint32_t)length);
	if (length < 15) {
		say("qcabt: version answer too short\n");
		return false;
	}
	/* cresp, rtype, one more byte, then product id (4), patch version (2), ROM version (2), SoC id (4) */
	const uint8_t *v = answer + 3;
	uint32_t product = v[0] | v[1] << 8 | v[2] << 16 | (uint32_t)v[3] << 24;
	uint16_t patch = (uint16_t)(v[4] | v[5] << 8);

	chip_rom = (uint16_t)(v[6] | v[7] << 8);
	chip_soc = v[8] | v[9] << 8 | v[10] << 16 | (uint32_t)v[11] << 24;
	say("qcabt: THE CHIP ANSWERS: product %#x, patch %#x, ROM %#x, SoC %#x\n", product, patch, chip_rom, chip_soc);
	return chip_soc != 0 && chip_rom != 0;
}

static void
chip_board_id(void)
{
	uint8_t request = EDL_GET_BID_REQ;
	int length = 0;
	const uint8_t *answer = command(EDL_PATCH_OPCODE, &request, 1, &length, 500);

	if (answer != NULL && length >= 5 && answer[0] == 0 && answer[1] == EDL_GET_BID_REQ) {
		chip_board = (uint16_t)(answer[3] << 8 | answer[4]);
	}
	say("qcabt: board id %#x\n", chip_board);
}

/* One file's segments. The last is always answered; the others only as the patch's download mode says (mode 1, 3: not). */
static bool
send_segments(const uint8_t *data, uint32_t size, uint8_t mode)
{
	uint8_t request[2 + MAX_TLV_SEGMENT];

	for (uint32_t at = 0; at < size;) {
		uint32_t piece = size - at < MAX_TLV_SEGMENT ? size - at : MAX_TLV_SEGMENT;
		bool last = at + piece == size || piece < MAX_TLV_SEGMENT;

		request[0] = EDL_PATCH_TLV_REQ;
		request[1] = (uint8_t)piece;
		memcpy(request + 2, data + at, piece);
		if (!last && (mode == 1 || mode == 3)) {
			if (!command_send(EDL_PATCH_OPCODE, request, (uint8_t)(piece + 2))) {
				return false;
			}
		} else {
			int length = 0;
			const uint8_t *answer = command(EDL_PATCH_OPCODE, request, (uint8_t)(piece + 2), &length, 3000);

			if (answer == NULL) {
				say("qcabt: segment at %u of %u has no answer\n", at, size);
				return false;
			}
			if (length >= 1 && answer[0] != 0) {
				hex_line("segment refused", answer, (uint32_t)length);
				return false;
			}
		}
		at += piece;
	}
	return true;
}

static bool
download_patch(void)
{
	char name[32];
	uint64_t size = 0;
	const uint8_t *file;
	uint8_t rom = (uint8_t)(((chip_rom & 0xf00) >> 4) | (chip_rom & 0xf));
	uint64_t began = mach_absolute_time(), nanoseconds;

	snprintf(name, sizeof(name), "hpbtfw%02x.tlv", rom);
	file = hakios_image_file("fw", name, &size, NULL);
	if (file == NULL || size < 28) {
		say("qcabt: no fw/%s in the small image\n", name);
		return false;
	}
	say("qcabt: rampatch %s, %llu bytes, type %u, download mode %u, product %#x, ROM %#x\n", name, size, file[0], file[14],
	    file[16] | file[17] << 8, file[18] | file[19] << 8);
	if (!send_segments(file, (uint32_t)size, file[14])) {
		return false;
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - began, &nanoseconds);
	say("qcabt: RAMPATCH TAKEN in %llu ms\n", nanoseconds / 1000000);
	return true;
}

static uint8_t nvm_copy[32 * 1024];

static bool
download_nvm(void)
{
	char name[32];
	uint64_t size = 0;
	const uint8_t *file = NULL;
	uint8_t rom = (uint8_t)(((chip_rom & 0xf00) >> 4) | (chip_rom & 0xf));
	const char *variant = (chip_soc & 0xff00) == 0x1200 ? "g" : "";    /* QCA_HSP_GF_SOC_ID */

	if (chip_board != 0) {
		snprintf(name, sizeof(name), "hpnv%02x%s.%x", rom, variant, chip_board);
		file = hakios_image_file("fw", name, &size, NULL);
	}
	if (file == NULL) {
		snprintf(name, sizeof(name), "hpnv%02x%s.bin", rom, variant);
		file = hakios_image_file("fw", name, &size, NULL);
	}
	if (file == NULL || size < 4 || size > sizeof(nvm_copy)) {
		say("qcabt: no usable fw/hpnv%02x%s.* (board %#x)\n", rom, variant, chip_board);
		return false;
	}
	memcpy(nvm_copy, file, (size_t)size);

	/* qca_tlv_check_data: the first set of tags; ours keeps the chip awake and at the speed we are at */
	uint32_t type_length = nvm_copy[0] | nvm_copy[1] << 8 | nvm_copy[2] << 16 | (uint32_t)nvm_copy[3] << 24;
	uint32_t base = 4, tags = type_length >> 8;

	if ((type_length & 0xff) == 4 && size >= 8) {
		type_length = nvm_copy[4] | nvm_copy[5] << 8 | nvm_copy[6] << 16 | (uint32_t)nvm_copy[7] << 24;
		base = 8;
		tags = type_length >> 8;
	}
	say("qcabt: NVM %s, %llu bytes, type %u\n", name, size, type_length & 0xff);
	if ((type_length & 0xff) == 2 && base + tags <= size) {
		for (uint32_t at = 0; at + 12 <= tags;) {
			uint32_t id = nvm_copy[base + at] | nvm_copy[base + at + 1] << 8, length = nvm_copy[base + at + 2] | nvm_copy[base + at + 3] << 8;
			uint8_t *value = nvm_copy + base + at + 12;

			if (at + 12 + length > tags) {
				say("qcabt: NVM tag %u at %u runs past the end\n", id, at);
				return false;
			}
			if (id == EDL_TAG_HCI && length >= 3) {
				say("qcabt: NVM HCI tag was %02x %02x %02x\n", value[0], value[1], value[2]);
				value[0] &= ~0x80;      /* no in-band sleep */
				value[1] = line_baud == 3000000 ? 0x0e : 0;     /* QCA_BAUDRATE_3000000, _115200: where the line is */
			} else if (id == EDL_TAG_DEEP_SLEEP && length >= 1) {
				value[0] &= ~0x01;      /* no deep sleep */
			}
			at += 12 + length;
		}
	}
	if (!send_segments(nvm_copy, (uint32_t)size, 0)) {
		return false;
	}
	say("qcabt: NVM TAKEN\n");
	return true;
}

static bool
hci_simple(const char *what, uint16_t opcode, const uint8_t *parameters, uint8_t length)
{
	int answer_length = 0;
	const uint8_t *answer = command(opcode, parameters, length, &answer_length, 2000);

	if (answer == NULL) {
		say("qcabt: %s: no answer\n", what);
		return false;
	}
	hex_line(what, answer, (uint32_t)answer_length);
	return answer_length == 0 || answer[0] == 0;
}

static void
ble_scan(uint32_t seconds)
{
	static const uint8_t mask[8] = { 0xff, 0xff, 0xfb, 0xff, 0x07, 0xf8, 0xbf, 0x3d };
	static const uint8_t parameters[7] = { 0x00, 0x60, 0x00, 0x30, 0x00, 0x00, 0x00 };     /* passive, 60 ms, 30 ms */
	static const uint8_t on[2] = { 1, 0 }, off[2] = { 0, 0 };
	uint32_t reports = 0;
	uint64_t deadline, now;

	if (!hci_simple("set event mask", 0x0c01, mask, 8) || !hci_simple("LE scan parameters", 0x200b, parameters, 7) ||
	    !hci_simple("LE scan on", 0x200c, on, 2)) {
		return;
	}
	clock_interval_to_deadline(seconds, NSEC_PER_SEC, &deadline);
	for (now = mach_absolute_time(); now < deadline; now = mach_absolute_time()) {
		int got = event_next(200);

		/* event[]: code, length, then LE meta's subevent 2, count, event type, address type, address (6, last byte first) */
		if (got >= 10 && event[0] == EV_LE_META && event[2] == 2) {
			if (reports++ < 12) {
				say("qcabt: heard %02x:%02x:%02x:%02x:%02x:%02x (type %u)\n", event[11], event[10], event[9], event[8], event[7], event[6], event[5]);
			}
		}
	}
	say("qcabt: %u advertising reports in %u s%s (in-band sleep: %u wakes, %u sleeps)\n", reports, seconds,
	    reports != 0 ? ": THE RADIO HEARS" : "", ibs_wakes, ibs_sleeps);
	hci_simple("LE scan off", 0x200c, off, 2);
}

/* ---- the whole ---------------------------------------------------------------------------------------- */

static void
chip_power(void)
{
	static const struct { const char *what, *name; uint32_t millivolts; } rails[] = {
		{ "io", "smpb10", 1800 }, { "antenna switch", "ldoe7", 2800 }, { "aon", "smpe2", 1012 }, { "dig", "smpb11", 966 },
		{ "rfa1", "smpc1", 1900 }, { "rfa2", "smpb12", 1350 },
	};

	/* (the regulators' blocks come with touch's start: until then a request fails) */
	for (unsigned tries = 0; tries < 120; tries++) {
		bool all = true;

		for (unsigned i = 0; i < sizeof(rails) / sizeof(rails[0]); i++) {
			all = hakios_unicorn_vreg_on(rails[i].name, rails[i].millivolts, false) && all;
		}
		if (all) {
			break;
		}
		IOSleep(500);
	}
	IOSleep(10);
	pin_output(PIN_BT_EN, false);
	IOSleep(50);
	pin_output(PIN_BT_EN, true);
	IOSleep(100);
	say("qcabt: BT_EN high; SW_CTRL reads %u, CTS reads %u\n", rd(GPIO_IO(PIN_SW_CTRL)) & 1, rd(GPIO_IO(PIN_CTS)) & 1);
}

/* The address of the device tree's /arm-io/bluetooth (xnuloader/loader.c), in display order, as 0xfc14 wants it. */
static const uint8_t chip_address[6] = { 0x02, 0x52, 0x44, 0x57, 0x00, 0x02 };

/* hci_qca.c qca_set_speed(), the WCN6855 branch: our RFR not ready while the command 0xfc48 goes out, both sides move, RFR back. */
static bool
chip_speed_up(void)
{
	uint32_t baud = 3000000;
	uint8_t code = 0x0e;    /* QCA_BAUDRATE_3000000 */
	int got;

	PE_parse_boot_argn("qcabt_baud", &baud, sizeof(baud));
	if (baud != 3000000) {
		return true;
	}
	wr(UART_MANUAL_RFR, 1U << 31 | 1U << 1);
	wr(UART_TX_TRANS_CFG, 1U << 1);
	if (!command_send(0xfc48, &code, 1)) {
		wr(UART_MANUAL_RFR, 0);
		wr(UART_TX_TRANS_CFG, 0);
		return false;
	}
	IOSleep(10);
	if (!line_set(baud)) {
		return false;
	}
	wr(UART_MANUAL_RFR, 0);
	wr(UART_TX_TRANS_CFG, 0);
	got = event_next(100);
	if (got >= 0) {
		hex_line("answer to the speed", event, 2U + (uint32_t)got);
	}
	if (!chip_version()) {
		say("qcabt: the chip does not answer at %u baud\n", baud);
		return false;
	}
	say("qcabt: THE LINE RUNS AT %u BAUD\n", baud);
	return true;
}

/* Power to HCI ready. 0 or errno. Nobody else reads the UART meanwhile. */
static int
chip_bring_up(void)
{
	rx_head = rx_tail = 0;
	chip_board = 0;
	(void)line_set(UART_BAUD);              /* (off and on again, the chip is back at its first speed) */
	chip_power();
	pin_look("chip on");
	if (!chip_version()) {
		pin_look("no answer");
		return EIO;
	}
	chip_board_id();
	if (level < 4) {
		return 0;
	}
	if (!chip_speed_up()) {
		return EIO;
	}
	if (!download_patch()) {
		return EIO;
	}
	IOSleep(10);
	if (!download_nvm()) {
		return EIO;
	}
	IOSleep(10);
	if (!hci_simple("HCI reset", 0x0c03, NULL, 0)) {
		return EIO;
	}
	hci_simple("local version", 0x1001, NULL, 0);
	if (level >= 6) {
		int length = 0;

		if (command(0xfc14, chip_address, sizeof(chip_address), &length, 1000) == NULL) {
			say("qcabt: the chip does not take its address\n");
		}
	}
	hci_simple("address", 0x1009, NULL, 0);
	say("qcabt: BLUETOOTH IS UP over HCI\n");
	return 0;
}

static void
run(void)
{
	tlmm = ml_io_map(0x0f100000UL, 0x100000UL);
	gcc = ml_io_map(GCC_PHYS, GCC_SIZE);
	se = ml_io_map(SE_PHYS, SE_SIZE);
	qup = ml_io_map(QUP_PHYS, 0x4000UL);
	if (tlmm == 0 || gcc == 0 || se == 0 || qup == 0) {
		say("qcabt: could not map the registers\n");
		return;
	}
	pin_look("as found");
	if (!clocks_on(UART_BAUD * 32) || !engine_look() || level < 2) {
		return;
	}
	uart_setup(3);
	if (!loopback_test() || level < 3) {
		return;
	}
	/* the line stays quiet for the chip: engine first, its pins second, the chip last */
	wr(GENI_M_IRQ_CLEAR, 0xffffffff);
	wr(UART_LOOPBACK, 0);
	rx_tail = rx_head;
	pin_function(PIN_CTS, PULL_NONE);
	pin_function(PIN_RTS, PULL_DOWN);
	pin_function(PIN_TX, PULL_UP);
	pin_function(PIN_RX, PULL_NONE);
	if (chip_bring_up() != 0) {
		if (level >= 6) {
			hakios_qcabt_h4_fail("the chip did not come up");
		}
		return;
	}
	if (level >= 6) {
		hakios_qcabt_h4_start();
	} else if (level >= 5) {
		ble_scan(8);
	}
}

/* ---- the seam to the transport (sys/hakios.qcabt.h) ------------------------------------------------- */

int
hakios_qcabt_uart_write(const uint8_t *buf, uint32_t len)
{
	return uart_send(buf, len) ? 0 : EIO;
}

uint32_t
hakios_qcabt_uart_read(uint8_t *buf, uint32_t max)
{
	uint32_t n = 0;

	uart_drain();
	while (n < max && rx_tail != rx_head) {
		buf[n++] = rx_ring[rx_tail];
		rx_tail = (rx_tail + 1) % sizeof(rx_ring);
	}
	return n;
}

int
hakios_qcabt_uart_set_baud(__unused uint32_t baud)
{
	return ENOTSUP;         /* (115200 for now) */
}

void
hakios_qcabt_uart_flow(__unused bool on)
{
}

uint32_t
hakios_qcabt_uart_baud(void)
{
	return line_baud;
}

uint64_t
hakios_qcabt_us(void)
{
	uint64_t nanoseconds;

	absolutetime_to_nanoseconds(mach_absolute_time(), &nanoseconds);
	return nanoseconds / 1000;
}

void
hakios_qcabt_sleep_ms(uint32_t ms)
{
	IOSleep(ms);
}

uint32_t
hakios_qcabt_arg(const char *name, uint32_t def)
{
	char full[40];
	uint32_t value = def;

	snprintf(full, sizeof(full), "qcabt_%s", name);
	PE_parse_boot_argn(full, &value, sizeof(value));
	return value;
}

void
hakios_qcabt_chip_off(void)
{
	pin_output(PIN_BT_EN, false);
}

int
hakios_qcabt_chip_on(void)
{
	int error = chip_bring_up();

	if (error != 0) {
		pin_output(PIN_BT_EN, false);
	}
	return error;
}

static void
qcabt_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint32_t delay_s = 20;

	PE_parse_boot_argn("qcabt_delay", &delay_s, sizeof(delay_s));
	if (level >= 6) {
		hakios_qcabt_h4_attach();       /* (bluetoothd may come before the chip: its sends wait) */
	}
	IOSleep(delay_s * 1000);
	say("qcabt: level %u begins\n", level);
	run();
	say("qcabt: done\n");
	thread_terminate(current_thread());
}

/* Called while the system starts (drivers/input/hakios.input.c). */
void
hakios_qcabt_init(void)
{
	thread_t thread;

	if (!PE_parse_boot_argn("qcabt", &level, sizeof(level)) || level == 0) {
		return;
	}
	if (kernel_thread_start(qcabt_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
