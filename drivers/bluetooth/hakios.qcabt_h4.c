/*
 * Adapted for the Xiaomi 12S from iOSPlus10 (github.com/akshaynexus/iOSPlus10, patches/xnu/0332-0338), which is
 * Copyright (c) 2026 iOSPlus10 contributors, MIT License: see THIRD-PARTY-NOTICE in this directory.
 */
/*
 * QCABT (OnePlus 10 Pro 5G, SM8450): QCA6490 Bluetooth, stage B (patch 0332): the H4 transport that iOS
 * bluetoothd and BlueTool open, on top of the GENI UART of patch 0330 after the firmware download of patch 0331.
 * qcabt=6 only. Plan: iOSPlus10 docs/bluetooth-plan.md section 4; README-bluetooth.md section 0332.
 *
 * What iOS 16 (20H392, iPhone10,3 userland) opens [V, addresses in usr/sbin/bluetoothd]:
 *   - main 0x100026af4..0x100026b68: access("/dev/cu.bluetooth"), access("/dev/uart.bluetooth"), else an
 *     IORegistry service named "marconi-bt" or "bluetooth"; none: "No bluetooth on this device", exit 0.
 *   - transport choice 0x100156160: env BT_HCI_TRANSPORT ("H4BC:POSIX:com.apple.uart.bluetooth,3000000"), else
 *     the ADT u32 transport-encoding (3 = H4BC).
 *   - POSIX open 0x100066bdc: socket(PF_SYSTEM, SOCK_STREAM, SYSPROTO_CONTROL), CTLIOCGINFO on the control name
 *     (default "com.apple.uart.sk.bluetooth" = Skywalk, not made here), connect; for a plain name:
 *     getsockopt(SYSPROTO_CONTROL, TIOCGETA 0x40487413, 72 bytes), cfmakeraw + cfsetspeed (ADT transport-speed,
 *     default 2400000), c_cflag 0x38b00 (CS8 CREAD HUPCL CLOCAL CRTSCTS), setsockopt(TIOCSETA 0x80487414) whose
 *     return value is not checked (0x1000671c8). Then plain read/write of H4 bytes.
 *   - H4BC opens /dev/btwake (0x100066b24); a failure is only logged.
 * So this file makes:
 *   1. the kernel control "com.apple.uart.bluetooth" (SOCK_STREAM, not privileged: bluetoothd and BlueTool run as
 *      mobile), TIOCGETA/TIOCSETA as socket options;
 *   2. /dev/uart.bluetooth and /dev/cu.bluetooth (one char device: read/write raw H4 bytes, select/poll/kqueue,
 *      TIOCGETA/TIOCSETA/FIONREAD) and /dev/btwake (open/close do nothing: the kernel owns BT_EN);
 *   3. one client at a time (control or device), EBUSY for a second one. A client change empties the queues.
 * Data path:
 *   RX: a kernel thread polls the UART (0330 helpers; the GENI SE has no interrupt line wired here). Poll period 1 ms
 *       for 200 ms after traffic, 5 ms up to 5 s, then 20 ms; RFR follows the RX FIFO, so the controller waits while
 *       the host does not read (no loss, no busy loop). H4 framing (type 04 event, 02 ACL, 03 SCO, 05 ISO), the QCA
 *       in-band sleep bytes (hci_qca.c: 0xfe SLEEP_IND, 0xfd WAKE_IND -> answer 0xfc WAKE_ACK, 0xfc WAKE_ACK) are
 *       handled here and never reach userland. Whole packets go to a 64 KiB queue; the UART is not read while the
 *       queue has less than one maximal packet + one read of room (flow control by RFR).
 *   TX: userland bytes are framed into H4 packets; each packet goes through the HCI shim (hakios_qcabt_shim.c,
 *       patch 0333) and then to the UART. When the controller sent SLEEP_IND, a WAKE_IND goes first and the send
 *       waits up to 100 ms per try for WAKE_ACK (3 tries, IBS_WAKE_RETRANS_TIMEOUT_MS of hci_qca.c).
 *   Termios from userland is stored and logged; the line stays at the rate stage A set (the controller would not
 *   follow a host-only change) with RTS/CTS on.
 * Limits: a send is a bounded busy-wait in the 0330 helper (delay(2) polls of the TX FIFO: wire time + 100 ms for CTS);
 * RX throughput is bounded by the GENI RX FIFO depth per poll period (the controller waits on RFR in between).
 * Before the chip is ready (stage A + HCI stage still running), a send waits up to qcabt_wait_ms (default 30000);
 * after a stage A/HCI failure it fails with EIO. Every wait is bounded.
 * Boot-args: qcabt_h4log=N (log the first N packets per direction, default 300; HW errors, IBS and the shim are
 *   always logged), qcabt_wait_ms=MS.
 * 0335 (power follows the client, dead-chip answers):
 *   - With a working chip, the last client closing starts a timer (qcabt_off_ms, default 3000); when no client
 *     came back, a power worker drives BT_EN low (hakios_qcabt_chip_off). The next client (bluetoothd after a toggle
 *     on, or its restart) powers the chip on again: BT_EN sequence + the whole HCI stage (hakios_qcabt_chip_on); its
 *     sends wait for that (qcabt_wait_ms). qcabt_pwrfollow=0 keeps the chip on.
 *   - With a failed chip (stage A or HCI stage failed), every host command gets a local Command Complete with status
 *     0x03 (Hardware Failure) instead of a send error (EIO), so bluetoothd's stack init ends with an HCI error and
 *     not a hung power-on (owner bug 2026-10-27: the Bluetooth switch stayed on). qcabt_dead=0: EIO as before;
 *     qcabt_dead=2: the connect is refused (bluetoothd then stops with "Transport layer initialization failed").
 * Log lines start "qcabt: h4 ". Proof of a working path: "h4 control ... connected", "h4 tx cmd 0x0c03",
 * "h4 rx evt 0x0e ... op 0x0c03 status 0".
 */
#if 1
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/conf.h>
#include <sys/uio.h>
#include <sys/fcntl.h>
#include <sys/proc.h>
#include <sys/select.h>
#include <sys/termios.h>
#include <sys/ttycom.h>
#include <sys/filio.h>
#include <sys/kern_control.h>
#include <sys/kpi_mbuf.h>
#include <sys/vnode.h>                  /* IO_NDELAY */
#include <miscfs/devfs/devfs.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.qcabt.h>

extern kern_return_t kernel_thread_start(thread_continue_t, void *, thread_t *);

#define SAY(...)        printf("qcabt: h4 " __VA_ARGS__)

#define H4_CTL_NAME     "com.apple.uart.bluetooth"
#define H4_CMD          0x01u
#define H4_ACL          0x02u
#define H4_SCO          0x03u
#define H4_EVT          0x04u
#define H4_ISO          0x05u
#define IBS_SLEEP_IND   0xfeu
#define IBS_WAKE_IND    0xfdu
#define IBS_WAKE_ACK    0xfcu
#define PKT_MAX         4104u           /* largest H4 frame accepted in each direction (ACL 5 + 4095 + slack) */
#define RXQ_SIZE        (64u << 10)
#define RD_CHUNK        512u            /* UART bytes per poll */
#define RXQ_ROOM        (PKT_MAX + RD_CHUNK + 64u)
#define CTL_CHUNK       2048u
#define DUMP_MAX        16u

#define OWNER_NONE      0u
#define OWNER_CTL       1u
#define OWNER_DEV       2u

#define MINOR_UART      0
#define MINOR_CU        1
#define MINOR_WAKE      2

static LCK_GRP_DECLARE(h4_grp, "hakios_qcabt_h4");
static LCK_MTX_DECLARE(h4_lock, &h4_grp);       /* state, queues, owner */
static LCK_MTX_DECLARE(h4_uart, &h4_grp);       /* the 0330 UART helpers (not reentrant) */
static LCK_MTX_DECLARE(h4_tx, &h4_grp);         /* TX framing + packet order */

static struct {
	bool attached, ready, failed, kick, ctl_full, ctrl_awake, ibs_seen, ackd;
	uint32_t owner, unit, log_tx, log_rx, wait_ms;
	kern_ctl_ref ref;
	int major;
	uint64_t last_act, last_stats;
	/* RX (thread only) */
	uint8_t rx[PKT_MAX];
	uint32_t rxn;
	/* RX queue toward userland (h4_lock) */
	uint8_t q[RXQ_SIZE];
	uint32_t qh, qt, qn;
	struct selinfo rsel;
	/* TX framing (h4_tx) */
	uint8_t tx[PKT_MAX];
	uint32_t txn;
	struct termios tio;
	/* counters */
	uint32_t n_tx, n_rx, n_local, n_drop_noowner, n_drop_full, n_drop_shim, n_garbage_rx, n_garbage_tx;
	uint32_t n_ibs_rx, n_ibs_wake, n_ibs_timeout, n_tx_err, n_wake_open;
	/* 0335 power worker (h4_lock) */
	bool off, powering, worker, rx_running, rx_reset;
	uint32_t dead_mode, pwr_follow, off_ms, n_dead, n_off, n_on, n_on_fail;
	uint64_t gone_at;
	uint64_t b_tx, b_rx, b_user;
	char why[64];
	/* 0338: a shim command deferred from the RX thread while the controller sleeps (h4_lock) */
	bool shim_pend;
	uint32_t shim_len, n_shim_defer;
	uint8_t shim_pkt[64];
} t;

static uint8_t ctl_buf[CTL_CHUNK];              /* RX thread only */

static inline uint16_t
le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | p[1] << 8);
}

static uint64_t
now_us(void)
{
	return hakios_qcabt_us();
}

/* " xx xx ..." of the first DUMP_MAX bytes into s (caller's buffer: TX and RX run on different threads) */
static const char *
hexs(char *s, size_t sz, const uint8_t *b, uint32_t n)
{
	uint32_t i, m = MIN(n, DUMP_MAX);
	s[0] = 0;
	for (i = 0; i < m && 3 * i + 4 <= sz; i++) {
		snprintf(s + 3 * i, 4, " %02x", b[i]);
	}
	if (n > m && 3 * i + 5 <= sz) {
		strlcpy(s + 3 * i, " ...", 5);
	}
	return s;
}

static const char *
type_name(uint8_t ty)
{
	switch (ty) {
	case H4_CMD: return "cmd";
	case H4_ACL: return "acl";
	case H4_SCO: return "sco";
	case H4_EVT: return "evt";
	case H4_ISO: return "iso";
	default: return "?";
	}
}

/*
 * Bytes of the H4 frame that starts at p (n bytes there): 0 = not known yet (need more bytes), else the frame
 * length. dir_tx: host -> controller (command header 3 bytes), else controller -> host (event header 2 bytes).
 * Unknown type: returns UINT32_MAX.
 */
static uint32_t
frame_len(const uint8_t *p, uint32_t n, bool dir_tx)
{
	switch (p[0]) {
	case H4_CMD:
		if (!dir_tx) {
			return UINT32_MAX;
		}
		return n < 4 ? 0 : 4u + p[3];
	case H4_EVT:
		if (dir_tx) {
			return UINT32_MAX;
		}
		return n < 3 ? 0 : 3u + p[2];
	case H4_ACL:
		return n < 5 ? 0 : 5u + le16(p + 3);
	case H4_SCO:
		return n < 4 ? 0 : 4u + p[3];
	case H4_ISO:
		return n < 5 ? 0 : 5u + (le16(p + 3) & 0x3fffu);
	default:
		return UINT32_MAX;
	}
}

static void
log_pkt(const char *dir, const uint8_t *p, uint32_t n)
{
	char hx[3 * DUMP_MAX + 8], what[64];
	what[0] = 0;
	if (p[0] == H4_CMD && n >= 4) {
		snprintf(what, sizeof(what), " %#06x plen %u", le16(p + 1), p[3]);
	} else if (p[0] == H4_EVT && n >= 3) {
		const uint8_t *e = p + 3;
		uint32_t pl = p[2];
		if (p[1] == 0x0e && pl >= 4) {
			snprintf(what, sizeof(what), " 0x0e plen %u (Command Complete op %#06x status %#x)", pl, le16(e + 1), e[3]);
		} else if (p[1] == 0x0e && pl >= 3) {
			snprintf(what, sizeof(what), " 0x0e plen %u (Command Complete op %#06x)", pl, le16(e + 1));
		} else if (p[1] == 0x0f && pl >= 4) {
			snprintf(what, sizeof(what), " 0x0f plen %u (Command Status op %#06x status %#x)", pl, le16(e + 2), e[0]);
		} else if (p[1] == 0x10 && pl >= 1) {
			snprintf(what, sizeof(what), " 0x10 plen %u (Hardware Error code %#x)", pl, e[0]);
		} else if (p[1] == 0x3e && pl >= 1) {
			snprintf(what, sizeof(what), " 0x3e plen %u (LE Meta sub %#x)", pl, e[0]);
		} else {
			snprintf(what, sizeof(what), " %#04x plen %u", p[1], pl);
		}
	} else if ((p[0] == H4_ACL || p[0] == H4_ISO) && n >= 5) {
		snprintf(what, sizeof(what), " handle %#x len %u", le16(p + 1) & 0x0fffu, le16(p + 3));
	} else if (p[0] == H4_SCO && n >= 4) {
		snprintf(what, sizeof(what), " handle %#x len %u", le16(p + 1) & 0x0fffu, p[3]);
	}
	SAY("%s %s%s:%s\n", dir, type_name(p[0]), what, hexs(hx, sizeof(hx), p, n));
}

/* ---- RX queue (h4_lock held) ---- */

static void
q_put(const uint8_t *p, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++) {
		t.q[t.qh] = p[i];
		t.qh = (t.qh + 1u) % RXQ_SIZE;
	}
	t.qn += n;
}

static uint32_t
q_peek(uint8_t *p, uint32_t max)
{
	uint32_t n = MIN(max, t.qn), at = t.qt;
	for (uint32_t i = 0; i < n; i++) {
		p[i] = t.q[at];
		at = (at + 1u) % RXQ_SIZE;
	}
	return n;
}

static void
q_drop(uint32_t n)
{
	n = MIN(n, t.qn);
	t.qt = (t.qt + n) % RXQ_SIZE;
	t.qn -= n;
}

static void
q_flush(void)
{
	t.qh = t.qt = t.qn = 0;
	t.ctl_full = false;
}

/* wake the RX thread (fast poll, ctl delivery) */
static void
kick(void)
{
	lck_mtx_lock(&h4_lock);
	t.kick = true;
	lck_mtx_unlock(&h4_lock);
	wakeup(&t.kick);
}

/* one whole packet toward the current client (any thread) */
static void
deliver(const uint8_t *p, uint32_t n, bool local)
{
	bool owner_dev = false, ok = false;

	lck_mtx_lock(&h4_lock);
	if (t.owner == OWNER_NONE) {
		t.n_drop_noowner++;
	} else if (RXQ_SIZE - t.qn < n) {
		t.n_drop_full++;
	} else {
		q_put(p, n);
		ok = true;
		owner_dev = t.owner == OWNER_DEV;
		if (local) {
			t.n_local++;
		}
	}
	t.kick = true;
	lck_mtx_unlock(&h4_lock);
	if (ok && owner_dev) {
		selwakeup(&t.rsel);
		wakeup(&t.q);
	}
	wakeup(&t.kick);
}

void
hakios_qcabt_h4_inject(const uint8_t *pkt, uint32_t len)
{
	if (len == 0 || len > PKT_MAX) {
		return;
	}
	if (t.log_rx > 0) {
		log_pkt("rx(local)", pkt, len);
	}
	deliver(pkt, len, true);
}

/* ---- UART side ---- */

static int
uart_send(const uint8_t *p, uint32_t n)
{
	lck_mtx_lock(&h4_uart);
	int error = hakios_qcabt_uart_write(p, n);
	lck_mtx_unlock(&h4_uart);
	return error;
}

/* TX side of in-band sleep: wake the controller first when it said SLEEP_IND (h4_tx held, not h4_lock) */
static void
ibs_wake(void)
{
	lck_mtx_lock(&h4_lock);
	bool awake = t.ctrl_awake;
	lck_mtx_unlock(&h4_lock);
	if (awake) {
		return;
	}
	for (uint32_t try = 0; try < 3; try++) {
		uint8_t b = IBS_WAKE_IND;
		lck_mtx_lock(&h4_lock);
		t.ackd = false;
		lck_mtx_unlock(&h4_lock);
		(void)uart_send(&b, 1);
		kick();
		lck_mtx_lock(&h4_lock);
		if (!t.ackd) {
			struct timespec ts = { 0, 100 * 1000 * 1000 };  /* IBS_WAKE_RETRANS_TIMEOUT_MS, hci_qca.c:49 */
			(void)msleep(&t.ackd, &h4_lock, PZERO, "nbtibs", &ts);
		}
		bool ok = t.ackd;
		t.n_ibs_wake++;
		lck_mtx_unlock(&h4_lock);
		if (ok) {
			return;
		}
	}
	lck_mtx_lock(&h4_lock);
	t.n_ibs_timeout++;
	t.ctrl_awake = true;            /* send anyway; the next SLEEP_IND sets it again */
	lck_mtx_unlock(&h4_lock);
	SAY("IBS: no WAKE_ACK after 3 x 100 ms; the packet goes out anyway\n");
}

/*
 * 0338: a command that the shim makes (legacy -> extended LE translation). from_rx = false: the TX path calls it with
 * h4_tx held (IBS wake as for a host packet). from_rx = true: the RX thread sends the next command of a chain right
 * after the controller's answer to the previous one. It does not take h4_tx (the TX path can wait in ibs_wake for
 * a WAKE_ACK that only this thread reads) and does not wait for WAKE_ACK: the controller sent an event just now,
 * so it is awake [I]. uart_send keeps each packet whole (h4_uart).
 */
int
hakios_qcabt_h4_send_cmd(const uint8_t *p, uint32_t n, bool from_rx)
{
	if (n < 4 || n > PKT_MAX) {
		return EINVAL;
	}
	if (t.log_tx > 0) {
		t.log_tx--;
		log_pkt(from_rx ? "tx(shim, rx thread)" : "tx(shim)", p, n);
	}
	t.n_tx++;
	t.b_tx += n;
	if (!from_rx) {
		ibs_wake();
	} else {
		lck_mtx_lock(&h4_lock);
		bool awake = t.ctrl_awake;
		lck_mtx_unlock(&h4_lock);
		if (!awake && n <= sizeof(t.shim_pkt)) {
			/* review F5: the controller sleeps; the power worker sends it after ibs_wake (WAKE_ACK) with h4_tx */
			lck_mtx_lock(&h4_lock);
			memcpy(t.shim_pkt, p, n);
			t.shim_len = n;
			t.shim_pend = true;
			t.n_shim_defer++;
			lck_mtx_unlock(&h4_lock);
			wakeup(&t.worker);
			SAY("tx(shim): controller said SLEEP_IND; %#06x goes out from the power worker after WAKE_ACK (#%u)\n",
			    le16(p + 1), t.n_shim_defer);
			return 0;
		}
	}
	int error = uart_send(p, n);
	t.last_act = now_us();
	kick();
	if (error && t.n_tx_err++ < 16) {
		SAY("tx(shim): UART write of %u bytes failed (%d)\n", n, error);
	}
	return error ? EIO : 0;
}

/* one complete packet from the controller (RX thread) */
static void
rx_packet(uint8_t *p, uint32_t n)
{
	t.n_rx++;
	t.b_rx += n;
	bool hw_err = p[0] == H4_EVT && n >= 3 && p[1] == 0x10;
	if (t.log_rx > 0 || hw_err) {
		if (t.log_rx > 0) {
			t.log_rx--;
		}
		log_pkt("rx", p, n);
	}
	if (hakios_qcabt_shim_rx(p, n) == HAKIOS_QCABT_SHIM_DROP) {
		t.n_drop_shim++;
		return;
	}
	deliver(p, n, false);
}

static void
rx_bytes(const uint8_t *b, uint32_t n)
{
	while (n > 0) {
		uint32_t m = MIN(n, PKT_MAX - t.rxn);
		memcpy(t.rx + t.rxn, b, m);
		t.rxn += m;
		b += m;
		n -= m;
		while (t.rxn > 0) {
			uint8_t c = t.rx[0];
			uint32_t need;
			if (c == IBS_SLEEP_IND || c == IBS_WAKE_IND || c == IBS_WAKE_ACK) {
				lck_mtx_lock(&h4_lock);
				t.ibs_seen = true;
				t.ctrl_awake = c != IBS_SLEEP_IND;
				if (c == IBS_WAKE_ACK) {
					t.ackd = true;
				}
				uint32_t k = t.n_ibs_rx++;
				lck_mtx_unlock(&h4_lock);
				if (c == IBS_WAKE_ACK) {
					wakeup(&t.ackd);
				}
				if (c == IBS_WAKE_IND) {
					uint8_t ack = IBS_WAKE_ACK;
					(void)uart_send(&ack, 1);
				}
				if (k < 32) {
					SAY("IBS %s from the controller%s\n", c == IBS_SLEEP_IND ? "SLEEP_IND" :
					    c == IBS_WAKE_IND ? "WAKE_IND" : "WAKE_ACK", c == IBS_WAKE_IND ? " (WAKE_ACK sent)" : "");
				}
				memmove(t.rx, t.rx + 1, --t.rxn);
				continue;
			}
			need = frame_len(t.rx, t.rxn, false);
			if (need == UINT32_MAX || need > PKT_MAX) {
				if (t.n_garbage_rx++ < 32) {
					SAY("rx: byte %#x is no packet start (or the frame is too long), dropped\n", c);
				}
				memmove(t.rx, t.rx + 1, --t.rxn);
				continue;
			}
			if (need == 0 || t.rxn < need) {
				break;
			}
			rx_packet(t.rx, need);
			t.rxn -= need;
			memmove(t.rx, t.rx + need, t.rxn);
		}
	}
}

/* queued bytes -> the kernel control client (RX thread only; no lock held around ctl_*) */
static void
deliver_ctl(void)
{
	for (;;) {
		lck_mtx_lock(&h4_lock);
		if (t.owner != OWNER_CTL || t.qn == 0 || t.ctl_full) {
			lck_mtx_unlock(&h4_lock);
			return;
		}
		uint32_t unit = t.unit;
		uint32_t n = q_peek(ctl_buf, CTL_CHUNK);
		kern_ctl_ref ref = t.ref;
		lck_mtx_unlock(&h4_lock);

		size_t space = 0;
		errno_t e = ctl_getenqueuespace(ref, unit, &space);
		if (e == 0 && space == 0) {
			e = ENOBUFS;
		}
		if (e == 0) {
			n = (uint32_t)MIN((size_t)n, space);
			e = ctl_enqueuedata(ref, unit, ctl_buf, n, 0);
		}
		lck_mtx_lock(&h4_lock);
		if (e == 0) {
			if (t.owner == OWNER_CTL && t.unit == unit) {
				q_drop(n);
			}
			t.b_user += n;
		} else if (e == ENOBUFS) {
			t.ctl_full = true;      /* ctl_rcvd clears it */
		}
		lck_mtx_unlock(&h4_lock);
		if (e) {
			if (e != ENOBUFS) {
				SAY("control unit %u: enqueue error %d\n", unit, e);
			}
			return;
		}
	}
}

static void
stats(bool force)
{
	uint64_t now = now_us();
	if (!force && now - t.last_stats < 60ull * 1000000u) {
		return;
	}
	t.last_stats = now;
	SAY("stats: tx %u pkts %llu B, rx %u pkts %llu B, to user %llu B, local answers %u, dropped (no client %u, "
	    "queue full %u, shim %u), garbage rx %u tx %u, IBS rx %u wake %u timeouts %u, tx errors %u\n",
	    t.n_tx, t.b_tx, t.n_rx, t.b_rx, t.b_user, t.n_local, t.n_drop_noowner, t.n_drop_full, t.n_drop_shim,
	    t.n_garbage_rx, t.n_garbage_tx, t.n_ibs_rx, t.n_ibs_wake, t.n_ibs_timeout, t.n_tx_err);
}

static void
h4_rx_thread(void *arg __unused, wait_result_t w __unused)
{
	uint8_t buf[RD_CHUNK];
	uint32_t last_counts = 0;

	SAY("RX thread running (poll 1/5/20 ms, queue %u KiB)\n", RXQ_SIZE >> 10);
	for (;;) {
		uint32_t n = 0;
		lck_mtx_lock(&h4_lock);
		bool room = RXQ_SIZE - t.qn >= RXQ_ROOM && !t.off && !t.powering && !t.failed;
		bool reset = t.rx_reset;
		t.rx_reset = false;
		lck_mtx_unlock(&h4_lock);
		if (reset) {
			t.rxn = 0;       /* 0335: no partial frame across a chip off/on */
		}
		if (room) {
			lck_mtx_lock(&h4_uart);
			n = hakios_qcabt_uart_read(buf, sizeof(buf));
			lck_mtx_unlock(&h4_uart);
		}
		if (n) {
			t.last_act = now_us();
			rx_bytes(buf, n);
		}
		deliver_ctl();
		if (t.n_tx + t.n_rx != last_counts) {
			last_counts = t.n_tx + t.n_rx;
			stats(false);
		}
		if (n == sizeof(buf)) {
			continue;               /* more bytes are waiting in the FIFO */
		}
		uint64_t idle = now_us() - t.last_act;
		uint32_t ms = idle < 200000u ? 1u : idle < 5000000u ? 5u : 20u;
		lck_mtx_lock(&h4_lock);
		if (!t.kick) {
			struct timespec ts = { 0, (long)ms * 1000 * 1000 };
			(void)msleep(&t.kick, &h4_lock, PZERO, "nbth4rx", &ts);
		}
		t.kick = false;
		lck_mtx_unlock(&h4_lock);
	}
}

/* ---- TX side (userland -> controller) ---- */

static int
wait_ready(void)
{
	uint64_t end = now_us() + (uint64_t)t.wait_ms * 1000u;
	int error = 0;

	lck_mtx_lock(&h4_lock);
	while (!t.ready && !t.failed) {
		if (now_us() >= end) {
			error = ETIMEDOUT;
			break;
		}
		struct timespec ts = { 1, 0 };
		int r = msleep(&t.ready, &h4_lock, PZERO | PCATCH, "nbth4up", &ts);
		if (r == EINTR || r == ERESTART) {
			error = r;
			break;
		}
	}
	if (!error && !t.ready) {
		error = EIO;
	}
	lck_mtx_unlock(&h4_lock);
	return error;
}

/*
 * 0335: commands whose controller answer is a Command Status event (Core spec Vol 4 Part E 7.x "Command Status
 * event shall be generated"), so the dead-chip answer has the event type the host waits for.
 */
static bool
uses_cmd_status(uint16_t op)
{
	static const uint16_t ops[] = {
		/* 0337 review F2: 0x0408 (Create_Connection_Cancel) answers with Command Complete; 0x205e, 0x042a,
		 * 0x0810, 0x2044, 0x2064, 0x2068, 0x206b, 0x206d added */
		0x0401, 0x0405, 0x0406, 0x0409, 0x040a, 0x040f, 0x0411, 0x0413, 0x0415, 0x0417, 0x0419, 0x041b,
		0x041c, 0x041d, 0x041f, 0x0428, 0x0429, 0x042a, 0x043d, 0x043e, 0x0801, 0x0803, 0x0804, 0x0807,
		0x080b, 0x0810, 0x200d, 0x2013, 0x2016, 0x2019, 0x2025, 0x2026, 0x2032, 0x2043, 0x2044, 0x205e,
		0x2064, 0x2068, 0x206b, 0x206d,
	};
	for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
		if (ops[i] == op) {
			return true;
		}
	}
	return false;
}

/* one complete host packet (h4_tx held) */
static int
tx_packet(uint8_t *p, uint32_t n)
{
	t.n_tx++;
	t.b_tx += n;
	if (t.log_tx > 0) {
		t.log_tx--;
		log_pkt("tx", p, n);
	}
	if (t.failed) {
		/* 0335: dead chip: a local Command Complete, status 0x03 Hardware Failure; other packets dropped */
		if (p[0] == H4_CMD && n >= 4) {
			bool cs = uses_cmd_status(le16(p + 1));
			uint8_t cc[7] = { H4_EVT, 0x0e, 4, 1, p[1], p[2], 0x03 };
			uint8_t st[7] = { H4_EVT, 0x0f, 4, 0x03, 1, p[1], p[2] };      /* Command Status: status, ncmd, op */
			if (t.n_dead++ < 16) {
				SAY("dead chip: cmd %#06x answered locally with %s status 0x03 (Hardware Failure)\n", le16(p + 1),
				    cs ? "Command Status" : "Command Complete");
			}
			hakios_qcabt_h4_inject(cs ? st : cc, 7);
		}
		return 0;
	}
	if (hakios_qcabt_shim_tx(p, n) == HAKIOS_QCABT_SHIM_DONE) {
		return 0;
	}
	ibs_wake();
	int error = uart_send(p, n);
	t.last_act = now_us();
	kick();                         /* the answer comes soon: fast poll */
	if (error) {
		if (t.n_tx_err++ < 16) {
			SAY("tx: UART write of %u bytes failed (%d)\n", n, error);
		}
		return EIO;
	}
	return 0;
}

static int
tx_bytes(const uint8_t *b, uint32_t n)
{
	int error = 0;
	while (n > 0 && !error) {
		uint32_t m = MIN(n, PKT_MAX - t.txn);
		memcpy(t.tx + t.txn, b, m);
		t.txn += m;
		b += m;
		n -= m;
		while (t.txn > 0 && !error) {
			uint32_t need = frame_len(t.tx, t.txn, true);
			if (need == UINT32_MAX || need > PKT_MAX) {
				if (t.n_garbage_tx++ < 32) {
					SAY("tx: byte %#x is no host packet start (or the frame is too long), dropped\n", t.tx[0]);
				}
				memmove(t.tx, t.tx + 1, --t.txn);
				continue;
			}
			if (need == 0 || t.txn < need) {
				break;
			}
			error = tx_packet(t.tx, need);
			t.txn -= need;
			memmove(t.tx, t.tx + need, t.txn);
		}
	}
	return error;
}

static int
send_user(const uint8_t *b, uint32_t n)
{
	int error = wait_ready();
	if (error == EIO && t.failed && t.dead_mode == 1) {
		error = 0;                      /* 0335: tx_packet answers each command locally */
	}
	if (error) {
		return error;
	}
	lck_mtx_lock(&h4_tx);
	error = tx_bytes(b, n);
	lck_mtx_unlock(&h4_tx);
	return error;
}

/* ---- termios (socket option and ioctl) ---- */

static void
tio_default(void)
{
	bzero(&t.tio, sizeof(t.tio));
	t.tio.c_cflag = CS8 | CREAD | CLOCAL | CRTSCTS;
	t.tio.c_ispeed = t.tio.c_ospeed = hakios_qcabt_uart_baud() ? hakios_qcabt_uart_baud() : 3000000u;
}

static void
tio_set(const struct termios *n, const char *via)
{
	uint32_t line = hakios_qcabt_uart_baud();
	SAY("%s: termios speed %lu, c_cflag %#lx (%s flow control), c_iflag %#lx; the line stays at %u baud, RTS/CTS on\n",
	    via, (unsigned long)n->c_ospeed, (unsigned long)n->c_cflag,
	    (n->c_cflag & CRTSCTS) == CRTSCTS ? "RTS/CTS" : "no", (unsigned long)n->c_iflag, line);
	if (n->c_ospeed && n->c_ospeed != line) {
		SAY("%s: speed %lu asked, %u kept (the controller was set to %u in stage A)\n", via,
		    (unsigned long)n->c_ospeed, line, line);
	}
	t.tio = *n;
}

/* ---- session (one client) ---- */

static void
session_reset(void)         /* h4_lock held */
{
	q_flush();
	t.log_tx = t.log_rx = hakios_qcabt_arg("h4log", 300);
	tio_default();
}

/* ---- 0335: power follows the client ---- */

/* a client came or went (h4_lock held); the worker decides */
static void
power_client_changed(bool came)
{
	if (!came) {
		t.gone_at = now_us();
	} else if (t.off && t.failed) {
		t.failed = false;       /* a runtime power-on failed before: this connect tries once more */
	}
	t.kick = true;
	wakeup(&t.worker);
}

static void
h4_power_thread(void *arg __unused, wait_result_t w __unused)
{
	SAY("power worker running (off %u ms after the last client, power follows the client %s)\n", t.off_ms,
	    t.pwr_follow ? "on" : "off");
	for (;;) {
		lck_mtx_lock(&h4_lock);
		bool client = t.owner != OWNER_NONE;
		bool want_on = client && t.off && !t.failed && !t.powering;
		bool want_off = !client && t.ready && t.pwr_follow && now_us() - t.gone_at >= (uint64_t)t.off_ms * 1000u;
		bool pending = !client && t.ready && t.pwr_follow;
		if (want_on || want_off) {
			t.powering = true;
			t.ready = false;        /* sends wait (power on) or find no client (power off) */
		}
		lck_mtx_unlock(&h4_lock);

		if (want_off) {
			lck_mtx_lock(&h4_tx);
			lck_mtx_lock(&h4_uart);
			hakios_qcabt_chip_off();
			lck_mtx_unlock(&h4_uart);
			lck_mtx_unlock(&h4_tx);
			lck_mtx_lock(&h4_lock);
			t.off = true;
			t.powering = false;
			t.rx_reset = true;
			t.shim_pend = false;            /* 0338: no deferred shim command across a chip off */
			t.n_off++;
			q_flush();
			lck_mtx_unlock(&h4_lock);
			SAY("power off #%u: no client for %u ms; the next client powers the chip on\n", t.n_off, t.off_ms);
			continue;
		}
		if (want_on) {
			SAY("power on #%u: a client is back\n", t.n_on + 1u);
			lck_mtx_lock(&h4_uart);
			int e = hakios_qcabt_chip_on();
			lck_mtx_unlock(&h4_uart);
			lck_mtx_lock(&h4_lock);
			t.powering = false;
			t.n_on++;
			t.rx_reset = true;
			if (e == 0) {
				t.off = false;
				t.failed = false;
				t.ready = true;
				t.ctrl_awake = true;
				q_flush();      /* nothing from the power-on reaches the client */
			} else {
				t.n_on_fail++;
				t.failed = true;        /* t.off stays: the next connect retries once (power_client_changed) */
				snprintf(t.why, sizeof(t.why), "power on failed (%d)", e);
			}
			t.last_act = now_us();
			lck_mtx_unlock(&h4_lock);
			wakeup(&t.ready);
			kick();
			SAY("power on #%u: %s\n", t.n_on, e ? "FAILED: dead-chip answers from now on" : "transport ready again");
			continue;
		}
		lck_mtx_lock(&h4_lock);
		bool sp = t.shim_pend && t.ready;
		uint8_t pkt[sizeof(t.shim_pkt)];
		uint32_t pn = t.shim_len;
		if (sp) {
			memcpy(pkt, t.shim_pkt, pn);
			t.shim_pend = false;
		}
		lck_mtx_unlock(&h4_lock);
		if (sp) {
			/* 0338: a deferred shim command: wake the controller (IBS) as the TX path does, then send */
			lck_mtx_lock(&h4_tx);
			int e = hakios_qcabt_h4_send_cmd(pkt, pn, false);
			lck_mtx_unlock(&h4_tx);
			SAY("tx(shim): deferred %#06x sent by the power worker (error %d)\n", le16(pkt + 1), e);
			continue;
		}
		lck_mtx_lock(&h4_lock);
		struct timespec ts = { 0, 200 * 1000 * 1000 };
		if (!(t.shim_pend && t.ready)) {
			(void)msleep(&t.worker, &h4_lock, PZERO, "nbth4pw", pending ? &ts : NULL);
		}
		lck_mtx_unlock(&h4_lock);
	}
}

/* ---- kernel control ---- */

static errno_t
ctl_connect_cb(kern_ctl_ref ref __unused, struct sockaddr_ctl *sac, void **unitinfo)
{
	char name[32];
	proc_selfname(name, sizeof(name));
	lck_mtx_lock(&h4_lock);
	if (t.owner != OWNER_NONE) {
		uint32_t o = t.owner;
		lck_mtx_unlock(&h4_lock);
		SAY("control connect from %s[%d] refused: busy (%s client)\n", name, proc_selfpid(),
		    o == OWNER_CTL ? "control" : "/dev");
		return EBUSY;
	}
	if (t.failed && t.dead_mode == 2) {
		lck_mtx_unlock(&h4_lock);
		SAY("control connect from %s[%d] refused: chip FAILED (qcabt_dead=2)\n", name, proc_selfpid());
		return ECONNREFUSED;
	}
	t.owner = OWNER_CTL;
	t.unit = sac->sc_unit;
	session_reset();
	power_client_changed(true);
	bool ready = t.ready, failed = t.failed, off = t.off;
	lck_mtx_unlock(&h4_lock);
	lck_mtx_lock(&h4_tx);
	t.txn = 0;
	lck_mtx_unlock(&h4_tx);
	hakios_qcabt_shim_session();
	*unitinfo = NULL;
	SAY("control %s connected (unit %u) by %s[%d]; chip %s\n", H4_CTL_NAME, sac->sc_unit, name, proc_selfpid(),
	    ready ? "ready" : failed ? (t.dead_mode == 1 ? "FAILED (commands answered with status 0x03)" : "FAILED") :
	    off ? "off: powering on" : "not ready yet");
	return 0;
}

static errno_t
ctl_disconnect_cb(kern_ctl_ref ref __unused, u_int32_t unit, void *unitinfo __unused)
{
	lck_mtx_lock(&h4_lock);
	bool mine = t.owner == OWNER_CTL && t.unit == unit;
	if (mine) {
		t.owner = OWNER_NONE;
		q_flush();
		power_client_changed(false);
	}
	lck_mtx_unlock(&h4_lock);
	if (mine) {
		SAY("control unit %u disconnected%s\n", unit, t.ready && t.pwr_follow ? "; power off follows if no client comes back" : "");
		stats(true);
	}
	return 0;
}

static errno_t
ctl_send_cb(kern_ctl_ref ref __unused, u_int32_t unit, void *unitinfo __unused, mbuf_t m, int flags __unused)
{
	errno_t error = 0;

	lck_mtx_lock(&h4_lock);
	bool mine = t.owner == OWNER_CTL && t.unit == unit;
	lck_mtx_unlock(&h4_lock);
	if (!mine) {
		mbuf_freem(m);
		return ENOTCONN;
	}
	for (mbuf_t n = m; n != NULL && !error; n = mbuf_next(n)) {
		if (mbuf_len(n) > 0) {
			error = send_user((const uint8_t *)mbuf_data(n), (uint32_t)mbuf_len(n));
		}
	}
	mbuf_freem(m);
	return error;
}

static errno_t
ctl_setopt_cb(kern_ctl_ref ref __unused, u_int32_t unit __unused, void *unitinfo __unused, int opt, void *data,
    size_t len)
{
	if ((unsigned)opt == (unsigned)TIOCSETA || (unsigned)opt == (unsigned)TIOCSETAW ||
	    (unsigned)opt == (unsigned)TIOCSETAF) {
		if (data == NULL || len < sizeof(struct termios)) {
			return EINVAL;
		}
		tio_set((const struct termios *)data, "setsockopt");
		return 0;
	}
	SAY("setsockopt %#x (%zu bytes) ignored\n", (unsigned)opt, len);
	return 0;
}

static errno_t
ctl_getopt_cb(kern_ctl_ref ref __unused, u_int32_t unit __unused, void *unitinfo __unused, int opt, void *data,
    size_t *len)
{
	if ((unsigned)opt == (unsigned)TIOCGETA) {
		if (data == NULL) {
			*len = sizeof(struct termios);
			return 0;
		}
		if (*len < sizeof(struct termios)) {
			return EINVAL;
		}
		memcpy(data, &t.tio, sizeof(struct termios));
		*len = sizeof(struct termios);
		return 0;
	}
	SAY("getsockopt %#x (%zu bytes): not supported\n", (unsigned)opt, *len);
	return ENOTSUP;
}

static void
ctl_rcvd_cb(kern_ctl_ref ref __unused, u_int32_t unit, void *unitinfo __unused, int flags __unused)
{
	lck_mtx_lock(&h4_lock);
	if (t.owner == OWNER_CTL && t.unit == unit) {
		t.ctl_full = false;
		t.kick = true;
	}
	lck_mtx_unlock(&h4_lock);
	wakeup(&t.kick);
}

/* ---- /dev/uart.bluetooth, /dev/cu.bluetooth, /dev/btwake ---- */

static int
h4_open(dev_t dev, int flags __unused, int devtype __unused, struct proc *p __unused)
{
	char name[32];
	proc_selfname(name, sizeof(name));
	if (minor(dev) == MINOR_WAKE) {
		if (t.n_wake_open++ < 8) {
			SAY("/dev/btwake opened by %s[%d] (BT_WAKE: nothing to do, BT_EN is the kernel's)\n", name,
			    proc_selfpid());
		}
		return 0;
	}
	if (minor(dev) > MINOR_CU) {
		return ENXIO;
	}
	lck_mtx_lock(&h4_lock);
	if (t.owner != OWNER_NONE) {
		lck_mtx_unlock(&h4_lock);
		SAY("/dev open by %s[%d] refused: busy\n", name, proc_selfpid());
		return EBUSY;
	}
	if (t.failed && t.dead_mode == 2) {
		lck_mtx_unlock(&h4_lock);
		SAY("/dev open by %s[%d] refused: chip FAILED (qcabt_dead=2)\n", name, proc_selfpid());
		return ENXIO;
	}
	t.owner = OWNER_DEV;
	session_reset();
	power_client_changed(true);
	lck_mtx_unlock(&h4_lock);
	lck_mtx_lock(&h4_tx);
	t.txn = 0;
	lck_mtx_unlock(&h4_tx);
	hakios_qcabt_shim_session();
	SAY("/dev/%s opened by %s[%d]\n", minor(dev) == MINOR_UART ? "uart.bluetooth" : "cu.bluetooth", name,
	    proc_selfpid());
	return 0;
}

static int
h4_close(dev_t dev, int flags __unused, int devtype __unused, struct proc *p __unused)
{
	if (minor(dev) == MINOR_WAKE) {
		return 0;
	}
	lck_mtx_lock(&h4_lock);
	bool mine = t.owner == OWNER_DEV;
	if (mine) {
		t.owner = OWNER_NONE;
		q_flush();
		power_client_changed(false);
	}
	lck_mtx_unlock(&h4_lock);
	wakeup(&t.q);                   /* a reader blocked in h4_read sees the closed client (EIO) */
	selwakeup(&t.rsel);
	if (mine) {
		SAY("/dev client closed\n");
		stats(true);
	}
	return 0;
}

static int
h4_read(dev_t dev, struct uio *uio, int ioflag)
{
	uint8_t b[1024];
	int error = 0;

	if (minor(dev) > MINOR_CU) {
		return ENODEV;
	}
	lck_mtx_lock(&h4_lock);
	while (t.qn == 0) {
		if (t.owner != OWNER_DEV) {
			error = EIO;
			break;
		}
		if (ioflag & IO_NDELAY) {
			error = EWOULDBLOCK;
			break;
		}
		error = msleep(&t.q, &h4_lock, PZERO | PCATCH, "nbth4rd", NULL);
		if (error) {
			break;
		}
	}
	uint32_t n = 0;
	if (!error) {
		n = q_peek(b, (uint32_t)MIN((user_ssize_t)sizeof(b), uio_resid(uio)));
		q_drop(n);
		t.b_user += n;
		t.kick = true;
	}
	lck_mtx_unlock(&h4_lock);
	wakeup(&t.kick);
	if (!error && n) {
		error = uiomove((const char *)b, (int)n, uio);
	}
	return error;
}

static int
h4_write(dev_t dev, struct uio *uio, int ioflag __unused)
{
	uint8_t b[1024];
	int error = 0;

	if (minor(dev) > MINOR_CU) {
		return ENODEV;
	}
	while (uio_resid(uio) > 0 && !error) {
		int n = (int)MIN((user_ssize_t)sizeof(b), uio_resid(uio));
		error = uiomove((char *)b, n, uio);
		if (!error) {
			error = send_user(b, (uint32_t)n);
		}
	}
	return error;
}

static int
h4_ioctl(dev_t dev, u_long cmd, caddr_t data, int flag __unused, struct proc *p __unused)
{
	if (minor(dev) > MINOR_CU) {
		return ENOTTY;
	}
	switch (cmd) {
	case TIOCGETA:
		memcpy(data, &t.tio, sizeof(struct termios));
		return 0;
	case TIOCSETA:
	case TIOCSETAW:
	case TIOCSETAF:
		tio_set((const struct termios *)data, "ioctl");
		return 0;
	case FIONREAD:
		lck_mtx_lock(&h4_lock);
		*(int *)data = (int)t.qn;
		lck_mtx_unlock(&h4_lock);
		return 0;
	case FIONBIO:
	case FIOASYNC:
	case TIOCEXCL:
	case TIOCNXCL:
		return 0;
	case TIOCFLUSH:
		if (*(int *)data == 0 || (*(int *)data & FREAD)) {
			lck_mtx_lock(&h4_lock);
			q_flush();
			lck_mtx_unlock(&h4_lock);
		}
		return 0;
	default:
		return ENOTTY;
	}
}

static int
h4_select(dev_t dev, int which, void *wql, struct proc *p)
{
	int ready = 0;
	if (minor(dev) > MINOR_CU) {
		return 0;
	}
	lck_mtx_lock(&h4_lock);
	if (which == FREAD) {
		ready = t.qn > 0 || t.owner != OWNER_DEV;
		if (!ready) {
			selrecord(p, &t.rsel, wql);
		}
	} else if (which == FWRITE) {
		ready = 1;
	}
	lck_mtx_unlock(&h4_lock);
	return ready;
}

static const struct cdevsw h4_cdevsw = {
	.d_open = h4_open,
	.d_close = h4_close,
	.d_read = h4_read,
	.d_write = h4_write,
	.d_ioctl = h4_ioctl,
	.d_stop = eno_stop,
	.d_reset = eno_reset,
	.d_ttys = NULL,
	.d_select = h4_select,
	.d_mmap = eno_mmap,
	.d_strategy = eno_strat,
	.d_type = 0,
};

/* ---- stage hooks (hakios.qca6490bt.c) ---- */

void
hakios_qcabt_h4_attach(void)
{
	struct kern_ctl_reg reg;
	errno_t e;

	if (t.attached) {
		return;
	}
	t.attached = true;
	t.wait_ms = MIN(hakios_qcabt_arg("wait_ms", 30000), 120000u);
	t.dead_mode = MIN(hakios_qcabt_arg("dead", 1), 2u);
	t.pwr_follow = hakios_qcabt_arg("pwrfollow", 1) != 0;
	t.off_ms = MIN(MAX(hakios_qcabt_arg("off_ms", 3000), 100u), 600000u);
	t.ctrl_awake = true;
	lck_mtx_lock(&h4_lock);
	session_reset();
	lck_mtx_unlock(&h4_lock);
	hakios_qcabt_shim_init();

	bzero(&reg, sizeof(reg));
	strlcpy(reg.ctl_name, H4_CTL_NAME, sizeof(reg.ctl_name));
	reg.ctl_flags = CTL_FLAG_REG_SOCK_STREAM | CTL_FLAG_REG_EXTENDED;       /* not CTL_FLAG_PRIVILEGED */
	reg.ctl_sendsize = 64u << 10;
	reg.ctl_recvsize = 128u << 10;
	reg.ctl_connect = ctl_connect_cb;
	reg.ctl_disconnect = ctl_disconnect_cb;
	reg.ctl_send = ctl_send_cb;
	reg.ctl_setopt = ctl_setopt_cb;
	reg.ctl_getopt = ctl_getopt_cb;
	reg.ctl_rcvd = ctl_rcvd_cb;
	e = ctl_register(&reg, &t.ref);
	SAY("kernel control %s: %s (error %d)\n", H4_CTL_NAME, e ? "NOT registered" : "registered", e);

	t.major = cdevsw_add(-1, &h4_cdevsw);
	if (t.major < 0) {
		SAY("cdevsw_add failed: no /dev nodes\n");
		return;
	}
	void *a = devfs_make_node(makedev(t.major, MINOR_UART), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "uart.bluetooth");
	void *b = devfs_make_node(makedev(t.major, MINOR_CU), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "cu.bluetooth");
	void *c = devfs_make_node(makedev(t.major, MINOR_WAKE), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "btwake");
	SAY("/dev/uart.bluetooth %s, /dev/cu.bluetooth %s, /dev/btwake %s (major %d); sends wait up to %u ms for the chip\n",
	    a ? "ok" : "FAILED", b ? "ok" : "FAILED", c ? "ok" : "FAILED", t.major, t.wait_ms);
}

/* the RX thread also delivers local answers to the kernel control client (deliver_ctl), so a dead chip needs it too */
static bool
rx_thread_start(void)
{
	thread_t thread;

	if (t.rx_running) {
		return true;
	}
	if (kernel_thread_start(h4_rx_thread, NULL, &thread) != KERN_SUCCESS) {
		return false;
	}
	t.rx_running = true;
	thread_deallocate(thread);
	return true;
}

void
hakios_qcabt_h4_start(void)
{
	thread_t thread;

	if (!t.attached) {
		hakios_qcabt_h4_attach();
	}
	lck_mtx_lock(&h4_lock);
	tio_default();
	lck_mtx_unlock(&h4_lock);
	t.last_act = now_us();
	if (!rx_thread_start()) {
		hakios_qcabt_h4_fail("no RX thread");
		return;
	}
	lck_mtx_lock(&h4_lock);
	t.ready = true;
	lck_mtx_unlock(&h4_lock);
	wakeup(&t.ready);
	SAY("transport ready at %u baud: the UART belongs to the transport now\n", hakios_qcabt_uart_baud());
	if (!t.worker && kernel_thread_start(h4_power_thread, NULL, &thread) == KERN_SUCCESS) {
		t.worker = true;
		thread_deallocate(thread);
	}
	lck_mtx_lock(&h4_lock);
	if (t.owner == OWNER_NONE) {
		t.gone_at = now_us();   /* no client yet: the off timer starts now */
	}
	lck_mtx_unlock(&h4_lock);
}

void
hakios_qcabt_h4_fail(const char *why)
{
	lck_mtx_lock(&h4_lock);
	t.failed = true;
	strlcpy(t.why, why, sizeof(t.why));
	lck_mtx_unlock(&h4_lock);
	wakeup(&t.ready);
	if (t.attached && t.dead_mode == 1 && !rx_thread_start()) {
		SAY("transport not usable: no RX thread for the local answers\n");
	}
	SAY("transport not usable: %s (%s)\n", why, t.dead_mode == 1 ? "commands get a local status 0x03 answer" :
	    t.dead_mode == 2 ? "clients are refused" : "sends fail with EIO");
}
#endif /* ARM64_BOARD_CONFIG_QCABT */
