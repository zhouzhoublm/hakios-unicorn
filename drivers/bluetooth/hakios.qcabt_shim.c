/*
 * Adapted for the Xiaomi 12S Pro from iOSPlus10 (github.com/akshaynexus/iOSPlus10, patches/xnu/0332-0338), which is
 * Copyright (c) 2026 iOSPlus10 contributors, MIT License: see THIRD-PARTY-NOTICE in this directory.
 */
/*
 * QCABT (OnePlus 10 Pro 5G, SM8450): QCA6490 Bluetooth, stage B part 2 (patch 0333): the HCI shim between iOS
 * (bluetoothd / BlueTool, which expect a Broadcom controller) and the Qualcomm controller. Called by the H4
 * transport (hakios_qcabt_h4.c, patch 0332) for every complete packet. qcabt=6 only.
 *
 * Why [V, 20H392 usr/sbin/bluetoothd and usr/sbin/BlueTool, iPhone10,3]:
 *   - bluetoothd does not boot the chip: it asks BlueTool over XPC (RunBuiltinScript "boot"/"init", reply handler
 *     0x10027249c reads "result" 0x100272580, "firmware" 0x10027261c, "manufacturer" 0x100272630; result 10 or 13 =
 *     "Couldn't find a platform / chipset / firmware combination" 0x10062ccb8 -> power state 2 = "Unknown
 *     platform/chipset/manufacturer combination, restarting" 0x1003a619c). BlueTool's built-in scripts are
 *     Broadcom only (.hcd patchram: 0xfc2e download minidriver, 0xfc4c write RAM, 0xfc4e launch RAM; 0xfc18 baud,
 *     0xfc01 BD address, 0xfc27 sleep mode; opcodes as in Linux btbcm.c:155-379, hci_bcm.c:195-399). The
 *     /etc/bluetool override scripts are read only when MobileGestalt ReleaseType is Vendor or Internal
 *     (BlueTool 0x100016054-0x100016280). Userland answer: a BlueTool stand-in (userland/bluetooth).
 *   - bluetoothd's own stack init (initCmd*CB) is standard HCI. Read Local Version (initCmdReadLocalVersionCB
 *     0x10010bc6c) keeps HCI version, HCI revision, LMP version and LMP subversion (0x10010bcd4-0x10010bd10) and
 *     NOT the manufacturer: no manufacturer check there, so 0x001d (Qualcomm) passes unchanged by default.
 *   - bluetoothd has 220 Broadcom/Apple vendor commands (name table 0x100804a20: BD_VSC_*, OLYMPIC, LE_META ...).
 *     On the QCA6490 the OGF 0x3f space is Qualcomm's: 0xfc00 EDL patch, 0xfc0b, 0xfc14 BD address, 0xfc17 logging,
 *     0xfc48 baud rate (btqca.h, hci_qca.c). A Broadcom opcode that is a different QCA command can change the
 *     controller state (the baud rate!). So by default NO host vendor command reaches the controller: each one is
 *     answered here with a Command Complete. Status 0 (success, no return parameters) for the Broadcom "write"
 *     commands that return only a status (table below), status 0x01 (Unknown HCI Command) for all others: a
 *     controller without the command answers the same way, and a status != 0 makes bluetoothd skip the return
 *     parameter parser ("Unsupported command %!, ignoring error").
 *   - Controller vendor events (code 0xff, QCA debug/log) are not delivered: bluetoothd would read them as Broadcom
 *     or Apple vendor events.
 * Policy boot-arg qcabt_shim: 1 (default) as above; 0 = everything passes (raw controller); 2 = every vendor
 * command gets status 0 (experiment: may make bluetoothd parse missing return parameters).
 * qcabt_mfr=N: rewrite the manufacturer of the Read Local Version Command Complete to N (e.g. 15 = Broadcom);
 * default 0 = unchanged (see above).
 * Log ("qcabt: shim ..."): every intercepted opcode (first 4 times per opcode, then every 64th), every rewrite,
 * the controller's Read Local Version once per client, dropped vendor events (first 16).
 */
#if 1
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/hakios.qcabt.h>

#define SAY(...)        printf("qcabt: shim " __VA_ARGS__)

#define H4_CMD          0x01u
#define H4_EVT          0x04u
#define EV_CMD_COMPLETE 0x0eu
#define EV_VENDOR       0xffu
#define OGF_VENDOR      0x3fu
#define OP_READ_LOCAL_VERSION 0x1001u
#define EV_CMD_STATUS   0x0fu
/*
 * 0336 (iOSPlus10): standard commands of bluetoothd's HCI init that the QCA6490 does not take [V, phone 2026-10-27,
 * logs/usblog-t2.txt and phone-log-20261027-025222.txt]:
 *   0x0c6d Write_LE_Host_Support: bluetoothd sends 01 6d 0c 02 01 01; the chip lists the command (mask octet 24
 *          bit 6) but answers 0x11 (Unsupported Feature or Parameter Value) because Simultaneous_LE_Host must be 0x00
 *          since Core 4.1. 0337: the parameter is rewritten to 0x00 and the command goes to the chip; only if the
 *          chip still rejects it, its status is rewritten to 0 (0336 answered it locally).
 *   0x0c5b Write_Default_Erroneous_Data_Reporting / 0x0c5a Read_...: bluetoothd never sends them; its stack checks the
 *          Read_Local_Supported_Commands mask (octet 18 bit 3 / bit 2, as Linux hci_sync.c commands[18] & 0x08 / 0x04),
 *          fails locally ("OI_HCICMD_WriteDefaultErroneousDataReporting (status=628)", "HCI initialization failed",
 *          restart reason 108). So the 0x1002 answer gets both bits, and when the chip did not have them the commands
 *          are answered here (write: status 0; read: status 0, reporting 0x00 disabled).
 * Every other non-zero status from the chip is logged ("controller rejected ...") for the next round.
 * qcabt_shimfix=0 turns 0336/0337 off (no effect with qcabt_shim=0: then every packet passes unchanged).
 *
 * 0337 (iOSPlus10): Apple/Broadcom vendor commands [V phone 2026-10-27 03:17, logs/phone-log-20261027-031731.txt and
 * the kernel h4 log]: bluetoothd's BCM VSC completion parser _BCM_ByteStreamEventParser (bluetoothd 0x100126134,
 * bcm_vsc.c) calls the waiting command's callback only when at least 2 bytes follow the status; with fewer it logs
 * "Invalid data size %d" and returns, so the command never completes: "BD_VSC_OLYMPIC_ANTENNA_PREF_B timedout waiting
 * for controller" after 15 s, "Notifying BT Firmware Crashed", restart reason 3602. _BCM_SubOpcodeChecker
 * (0x100126390) matches 0xfce9 answers by the first return byte = the command's sub-opcode. So every vendor command
 * answered here carries return data: byte 0 = the first parameter byte (sub-opcode echo), then zeros
 * (qcabt_vscdata, default 8 bytes, 2..32). The opcodes bluetoothd sent on the QCA6490 get status 0 (feature
 * configuration; names from the error order in the log [I]). 0x2007 LE_Read_Advertising_Physical_Channel_Tx_Power:
 * the chip answers 0x0c (Command Disallowed); rewritten to status 0, 0 dBm.
 * 0338 review of 0337 [V h4 logs usblog-t6 and usblog-20261026-233816 04:06]: 0xfce2 is a multiplex opcode (byte 0 =
 * sub-opcode: 0x0b, 0x1c, 0x15, 0x26, 0x1e, 0x36 seen; the error order names 0x0b CHANGE_LE_SETTINGS, 0x1c
 * SET_MIN_AFH_LE_CHANNELS, 0x15 ANTENNA_PREF_B [I]). 0xfd48 carries 01 + a 6-byte address (the same address goes in
 * the next 0x2005 LE_Set_Random_Address): an address list VSC [I], not SET_MIN_AFH_LE_CHANNELS. With 0337 bluetoothd
 * passed all VSCs (no "timedout waiting for controller") and stopped at 0x200b (below).
 *
 * 0338 (iOSPlus10): the QCA6490 firmware (BTFW.HSP.2.1.0-00394) rejects legacy LE advertising/scanning commands with
 * 0x0c Command Disallowed although the host sent no extended LE command [V usblog-20261026-233816 04:06:33: 0x2007
 * and 0x200b LE_Set_Scan_Parameters 00 e0 01 30 00 01 00 -> status 0x0c; bluetoothd exits with status 1 and launchd
 * restarts it every ~10 s]. bluetoothd uses only legacy LE commands on this chip ("support for Extended Scanning is
 * disabled": its chipset table, not the chip's LE features). So the shim translates (qcabt_lext=1, default; 0 =
 * pass the legacy commands unchanged):
 *   0x200b Set_Scan_Parameters  -> 0x2041 Set_Extended_Scan_Parameters (1M PHY), answer op rewritten
 *   0x200c Set_Scan_Enable      -> 0x2042 Set_Extended_Scan_Enable (duration 0, period 0), answer op rewritten
 *   0x200d Create_Connection    -> 0x2043 Extended_Create_Connection (1M PHY), Command Status op rewritten
 *   0x2006 Set_Advertising_Parameters -> 0x2036 (set 0, legacy PDU properties) [+ 0x2035 set random address]
 *   0x2008 / 0x2009 Set_Advertising_Data / Scan_Response_Data -> 0x2037 / 0x2038 (set 0, complete data)
 *   0x200a Set_Advertising_Enable -> 0x2039 (set 0)
 *   0x2005 Set_Random_Address: sent as is; when set 0 exists, 0x2035 for set 0 follows (status only logged)
 *   events: LE Extended Advertising Report (0x3e sub 0x0d) -> one LE Advertising Report (sub 0x02) per legacy-PDU
 *   report (extended PDUs dropped: a legacy scan does not see them); LE Advertising Set Terminated (sub 0x12) and
 *   LE Scan Timeout (sub 0x11) dropped; LE event mask (0x2001) gets bit 12 (Extended Advertising Report).
 * The advertising commands run as a chain: one command at a time, the next after the controller's answer (RX
 * thread), then one legacy Command Complete with the first failing status (0x2035 failures are only logged).
 */
#define OP_READ_SUPPORTED_COMMANDS 0x1002u
#define OP_READ_ERR_DATA  0x0c5au
#define OP_WRITE_ERR_DATA 0x0c5bu
#define OP_WRITE_LE_HOST  0x0c6du
#define OP_LE_READ_ADV_TX_POWER 0x2007u
#define OP_RESET          0x0c03u
#define OP_LE_EVENT_MASK  0x2001u
#define OP_LE_RAND_ADDR   0x2005u
#define OP_LE_ADV_PARAM   0x2006u
#define OP_LE_ADV_DATA    0x2008u
#define OP_LE_SCAN_RSP    0x2009u
#define OP_LE_ADV_ENABLE  0x200au
#define OP_LE_SCAN_PARAM  0x200bu
#define OP_LE_SCAN_ENABLE 0x200cu
#define OP_LE_CREATE_CONN 0x200du
#define OP_LE_SET_RAND    0x2035u
#define OP_LE_EXT_ADV_PARAM 0x2036u
#define OP_LE_EXT_ADV_DATA  0x2037u
#define OP_LE_EXT_SCAN_RSP  0x2038u
#define OP_LE_EXT_ADV_EN    0x2039u
#define OP_LE_EXT_SCAN_PARAM 0x2041u
#define OP_LE_EXT_SCAN_EN   0x2042u
#define OP_LE_EXT_CONN      0x2043u
#define EV_LE_META        0x3eu
#define VSC_DATA_DEF      8u
#define CMDS_OCTET_ERR    18u
#define CMDS_ERR_BITS     0x0cu         /* bit 2 read, bit 3 write */

struct shim_op {
	uint16_t op;
	uint8_t status;         /* Command Complete status */
	const char *name;
};

/*
 * Broadcom vendor commands with a status-only Command Complete, answered "success" (so BlueTool.orig scripts and
 * bluetoothd's sleep/PCM setup go on). Everything else in OGF 0x3f: status 0x01.
 */
static const struct shim_op shim_ops[] = {
	{ 0xfc01, 0x00, "BCM Write_BD_ADDR (btbcm.c:155; the kernel set the ADT address, step 20)" },
	{ 0xfc18, 0x00, "BCM Update_UART_Baud_Rate (hci_bcm.c:214; the line stays, see h4 termios)" },
	{ 0xfc1c, 0x00, "BCM Write_SCO_PCM_Int_Param (btbcm.c:201)" },
	{ 0xfc1e, 0x00, "BCM Write_PCM_Data_Format_Param" },
	{ 0xfc27, 0x00, "BCM Write_Sleep_Mode_Param (hci_bcm.c:399; BD_VSC_WRITE_SLEEPMODE_PARAM)" },
	{ 0xfc2e, 0x00, "BCM Download_Minidriver (btbcm.c:223)" },
	{ 0xfc45, 0x00, "BCM Write_UART_Clock_Setting (hci_bcm.c:195)" },
	{ 0xfc4c, 0x00, "BCM Write_RAM (.hcd patchram record)" },
	{ 0xfc4e, 0x00, "BCM Launch_RAM (.hcd patchram end)" },
	{ 0xfc6d, 0x00, "BCM Write_I2SPCM_Interface_Param" },
	{ 0xfc1d, 0x01, "BCM Read_SCO_PCM_Int_Param (btbcm.c:173)" },
	{ 0xfc5a, 0x01, "BCM Read_USB_Product (btbcm.c:379)" },
	{ 0xfc6e, 0x01, "BCM Read_Controller_Features (btbcm.c:359)" },
	{ 0xfc79, 0x01, "BCM Read_Verbose_Config_Version_Info (btbcm.c:339)" },
	/* 0337: Apple VSCs bluetoothd sends on this chip (opcode [V] h4 log; name [I] from the error order) */
	{ 0xfd3c, 0x00, "Apple VSC 0xfd3c (after \"BCM_4357 supports the following VSCs\")" },
	{ 0xfce9, 0x00, "Apple LE_META (BD_VSC_LE_META_ENA_FEATURES; sub-opcode in byte 0, _BCM_SubOpcodeChecker)" },
	{ 0xfce2, 0x00, "Apple OLYMPIC (sub 0x0b CHANGE_LE_SETTINGS, 0x1c SET_MIN_AFH_LE_CHANNELS, 0x15 ANTENNA_PREF_B; "
	  "sub-opcode in byte 0)" },
	{ 0xfd48, 0x00, "Apple VSC 0xfd48 (01 + a 6-byte address; an address list command [I])" },
	{ 0xfd47, 0x00, "Apple VSC 0xfd47 (BD_VSC_ENABLE_EXTENDED_DUPLICATE_FILTER)" },
};

#define SEEN_SLOTS      1024u           /* OCF 0..0x3ff */

static struct {
	uint32_t policy, mfr;
	bool version_logged;
	uint16_t seen[SEEN_SLOTS];
	uint32_t n_local, n_vse;
	uint32_t fix, vscdata;
	bool lehost_logged, advtx_logged, lehost_rej_logged;
	bool chip_err_rd, chip_err_wr, cmds_known;
	uint8_t rej[SEEN_SLOTS];
	/* 0338 legacy -> extended LE */
	uint32_t lext;
	bool adv_set, rnd_known;
	uint8_t rnd[6];
	uint32_t n_xlat, n_rep_in, n_rep_out, n_rep_drop, n_ev_drop, n_rep_logged, n_chain_stale;
	uint8_t adv_type;               /* legacy Advertising_Type of the last accepted 0x2006 (0 = ADV_IND default) */
	uint8_t pend_scan_p, pend_scan_e, pend_conn;
	struct {
		bool active;
		uint16_t legacy;
		uint8_t n, i, status, crit;     /* crit: bit i = a failure of step i is the legacy status */
		uint8_t want_type;              /* 0x2006 chain: its legacy type, kept when 0x2036 succeeds */
		uint64_t at_us;                 /* last step sent (stale chain check) */
		uint8_t len[3];
		uint8_t cmd[3][48];
	} ch;
} s;

static inline uint16_t
le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | p[1] << 8);
}

void
hakios_qcabt_shim_init(void)
{
	s.policy = hakios_qcabt_arg("shim", 1);
	s.mfr = hakios_qcabt_arg("mfr", 0);
	s.fix = hakios_qcabt_arg("shimfix", 1);
	s.vscdata = hakios_qcabt_arg("vscdata", VSC_DATA_DEF);
	if (s.vscdata < 2 || s.vscdata > 32) {
		s.vscdata = VSC_DATA_DEF;
	}
	SAY("0336/0337 init fixes %s: 0x0c6d Simultaneous_LE_Host -> 0x00; 0x0c5a/0x0c5b Erroneous Data Reporting bits "
	    "set in the 0x1002 mask and answered here when the chip lacks them; 0x2007 0x0c -> status 0, 0 dBm; vendor "
	    "answers carry %u data bytes (sub-opcode echo + zeros)\n", s.fix ? "on" : "off", s.vscdata);
	s.lext = hakios_qcabt_arg("lext", 1);
	SAY("0338 legacy LE -> extended LE translation %s (qcabt_lext): 0x200b/0x200c/0x200d/0x2006/0x2008/0x2009/"
	    "0x200a -> 0x2041/0x2042/0x2043/0x2036/0x2037/0x2038/0x2039; extended reports -> legacy reports\n",
	    s.lext ? "on" : "off");
	SAY("policy %u (%s), manufacturer rewrite %s%#x; %zu Broadcom opcodes answered with success, other vendor "
	    "commands with status 0x01\n", s.policy,
	    s.policy == 0 ? "pass everything" : s.policy == 2 ? "all vendor commands succeed" : "vendor commands local",
	    s.mfr ? "to " : "off ", s.mfr, sizeof(shim_ops) / sizeof(shim_ops[0]));
	for (size_t i = 0; i < sizeof(shim_ops) / sizeof(shim_ops[0]); i++) {
		SAY("table %#06x -> status %#x: %s\n", shim_ops[i].op, shim_ops[i].status, shim_ops[i].name);
	}
}

void
hakios_qcabt_shim_session(void)
{
	s.version_logged = false;
	bzero(s.seen, sizeof(s.seen));
	bzero(s.rej, sizeof(s.rej));
	s.cmds_known = s.chip_err_rd = s.chip_err_wr = false;
	s.lehost_logged = s.advtx_logged = s.lehost_rej_logged = false;
	s.adv_set = s.rnd_known = false;
	s.adv_type = 0;
	s.pend_scan_p = s.pend_scan_e = s.pend_conn = 0;
	bzero(&s.ch, sizeof(s.ch));
}

static void
answer(uint16_t op, uint8_t status)
{
	/* H4 event: 04 0e 04 <Num_HCI_Command_Packets 1> <op lo> <op hi> <status> */
	const uint8_t ev[7] = { H4_EVT, EV_CMD_COMPLETE, 4, 1, (uint8_t)op, (uint8_t)(op >> 8), status };
	hakios_qcabt_h4_inject(ev, sizeof(ev));
}

/* 0337: a vendor command answer with return data (BCM parser needs >= 2 bytes after the status) */
static void
answer_vsc(uint16_t op, uint8_t status, uint8_t sub)
{
	uint8_t ev[7 + 32];
	uint32_t n = s.vscdata;
	bzero(ev, sizeof(ev));
	ev[0] = H4_EVT;
	ev[1] = EV_CMD_COMPLETE;
	ev[2] = (uint8_t)(4u + n);
	ev[3] = 1;
	ev[4] = (uint8_t)op;
	ev[5] = (uint8_t)(op >> 8);
	ev[6] = status;
	ev[7] = sub;
	hakios_qcabt_h4_inject(ev, 7u + n);
}

/*
 * ---- 0338: legacy -> extended LE ----
 * Locking: the state below changes on the TX path (h4_tx held) and on the RX thread, without a lock. This is safe
 * because the controller gives one command credit and bluetoothd sends the next LE command only after the answer
 * to the previous one (review F8). Late answers after an HCI_Reset go to bluetoothd with the extended opcode.
 */

static uint8_t
mkcmd(uint8_t *b, uint16_t op, const uint8_t *par, uint8_t plen)
{
	b[0] = H4_CMD;
	b[1] = (uint8_t)op;
	b[2] = (uint8_t)(op >> 8);
	b[3] = plen;
	memcpy(b + 4, par, plen);
	return (uint8_t)(4u + plen);
}

/* 0x2036 for set 0 from the 15 legacy parameter bytes (NULL: legacy defaults, ADV_IND 1.28 s, public) */
static uint8_t
mk_ext_adv_param(uint8_t *b, const uint8_t *lp)
{
	static const uint8_t def[15] = { 0x00, 0x08, 0x00, 0x08, 0x00, 0x00, 0x00, 0, 0, 0, 0, 0, 0, 0x07, 0x00 };
	const uint8_t *l = lp ? lp : def;
	uint16_t props;
	switch (l[4]) {
	case 0x00: props = 0x0013; break;       /* ADV_IND: legacy, connectable, scannable */
	case 0x01: props = 0x001d; break;       /* ADV_DIRECT_IND high duty */
	case 0x02: props = 0x0012; break;       /* ADV_SCAN_IND */
	case 0x03: props = 0x0010; break;       /* ADV_NONCONN_IND */
	case 0x04: props = 0x0015; break;       /* ADV_DIRECT_IND low duty */
	default:   props = 0x0013; break;
	}
	uint8_t p[25] = {
		0x00, (uint8_t)props, (uint8_t)(props >> 8),
		l[0], l[1], 0x00,                       /* primary interval min (3 bytes) */
		l[2], l[3], 0x00,                       /* primary interval max */
		l[13], l[5], l[6],                      /* channel map, own address type, peer address type */
		l[7], l[8], l[9], l[10], l[11], l[12],  /* peer address */
		l[14], 0x7f, 0x01, 0x00, 0x01, 0x00, 0x00 /* filter, TX power no preference, 1M, skip 0, 1M, SID 0, no notify */
	};
	return mkcmd(b, OP_LE_EXT_ADV_PARAM, p, sizeof(p));
}

static void
chain_answer(uint8_t status)
{
	uint16_t op = s.ch.legacy;
	s.ch.active = false;
	if (status != 0 || s.n_xlat < 16) {
		SAY("lext: legacy %#06x answered with status %#x after %u extended command(s)\n", op, status, s.ch.i + 1u);
	}
	answer(op, status);
}

/* send step s.ch.i; false = the UART send failed (the legacy command is answered with 0x03 Hardware Failure) */
static bool
chain_send(bool from_rx)
{
	s.ch.at_us = hakios_qcabt_us();
	if (hakios_qcabt_h4_send_cmd(s.ch.cmd[s.ch.i], s.ch.len[s.ch.i], from_rx) != 0) {
		chain_answer(0x03);
		return false;
	}
	return true;
}

static int
chain_start(uint16_t legacy, uint8_t crit)
{
	s.ch.active = true;
	s.ch.legacy = legacy;
	s.ch.i = 0;
	s.ch.status = 0;
	s.ch.crit = crit;
	s.n_xlat++;
	(void)chain_send(false);
	return HAKIOS_QCABT_SHIM_DONE;
}

/* the controller answered a command; true = it was a chain step (the event is not delivered) */
static bool
chain_rx(uint16_t op, uint8_t status)
{
	if (!s.ch.active || s.ch.len[s.ch.i] < 4 || op != le16(s.ch.cmd[s.ch.i] + 1)) {
		return false;
	}
	if (op == OP_LE_EXT_ADV_PARAM && status == 0) {
		s.adv_set = true;               /* review F2: set 0 exists only after the controller took 0x2036 */
		s.adv_type = s.ch.want_type;
	}
	if (status != 0) {
		if (s.ch.crit & (1u << s.ch.i)) {
			chain_answer(status);
			return true;
		}
		SAY("lext: %#06x (step %u of legacy %#06x) status %#x: only logged\n", op, s.ch.i + 1u, s.ch.legacy, status);
	}
	if (s.ch.i + 1u < s.ch.n) {
		s.ch.i++;
		(void)chain_send(true);
		return true;
	}
	chain_answer(0);
	return true;
}

/* host command -> extended form; HAKIOS_QCABT_SHIM_PASS when the command is not translated */
static int
lext_tx(uint8_t *pkt, uint32_t len)
{
	uint16_t op = le16(pkt + 1);
	uint8_t plen = pkt[3];
	const uint8_t *l = pkt + 4;
	if (len < 4u + plen) {
		return HAKIOS_QCABT_SHIM_PASS;
	}
	if (op == OP_RESET) {
		s.adv_set = s.rnd_known = false;
		s.adv_type = 0;
		s.pend_scan_p = s.pend_scan_e = s.pend_conn = 0;
		bzero(&s.ch, sizeof(s.ch));
		return HAKIOS_QCABT_SHIM_PASS;
	}
	if (op == OP_LE_EVENT_MASK && plen >= 8 && (pkt[5] & 0x10u) == 0) {
		SAY("lext: LE event mask byte 1 %#x -> %#x (Extended Advertising Report on)\n", pkt[5], pkt[5] | 0x10u);
		pkt[5] |= 0x10u;
		return HAKIOS_QCABT_SHIM_PASS;
	}
	bool adv = op == OP_LE_ADV_PARAM || op == OP_LE_ADV_DATA || op == OP_LE_SCAN_RSP || op == OP_LE_ADV_ENABLE ||
	    (op == OP_LE_RAND_ADDR && s.adv_set);
	if (adv && s.ch.active && hakios_qcabt_us() - s.ch.at_us > 2000000u) {
		/* review F6: a step answer never came (lost command); bluetoothd has its own timeout for the old one */
		s.n_chain_stale++;
		SAY("lext: chain of legacy %#06x had no answer for step %u in 2 s: dropped (#%u)\n", s.ch.legacy,
		    s.ch.i + 1u, s.n_chain_stale);
		s.ch.active = false;
	}
	if (adv && s.ch.active) {
		SAY("lext: %#06x while legacy %#06x is still running: answered with 0x0c Command Disallowed\n", op,
		    s.ch.legacy);
		answer(op, 0x0c);
		return HAKIOS_QCABT_SHIM_DONE;
	}
	uint8_t b[48];
	switch (op) {
	case OP_LE_SCAN_PARAM:
		if (plen < 7) {
			break;
		} else {
			/* own type, filter, PHYs 1M, [type, interval, window] */
			const uint8_t p[8] = { l[5], l[6], 0x01, l[0], l[1], l[2], l[3], l[4] };
			s.pend_scan_p++;
			s.n_xlat++;
			if (hakios_qcabt_h4_send_cmd(b, mkcmd(b, OP_LE_EXT_SCAN_PARAM, p, sizeof(p)), false) != 0) {
				s.pend_scan_p--;
				answer(op, 0x03);
			}
			return HAKIOS_QCABT_SHIM_DONE;
		}
	case OP_LE_SCAN_ENABLE:
		if (plen < 2) {
			break;
		} else {
			const uint8_t p[6] = { l[0], l[1], 0, 0, 0, 0 };
			s.pend_scan_e++;
			s.n_xlat++;
			if (hakios_qcabt_h4_send_cmd(b, mkcmd(b, OP_LE_EXT_SCAN_EN, p, sizeof(p)), false) != 0) {
				s.pend_scan_e--;
				answer(op, 0x03);
			}
			return HAKIOS_QCABT_SHIM_DONE;
		}
	case OP_LE_CREATE_CONN:
		if (plen < 25) {
			break;
		} else {
			/* filter, own type, peer type, peer address, PHYs 1M, scan interval/window, conn interval min/max,
			 * latency, supervision timeout, CE length min/max */
			const uint8_t p[26] = { l[4], l[12], l[5], l[6], l[7], l[8], l[9], l[10], l[11], 0x01,
				                l[0], l[1], l[2], l[3], l[13], l[14], l[15], l[16], l[17], l[18], l[19], l[20],
				                l[21], l[22], l[23], l[24] };
			s.pend_conn++;
			s.n_xlat++;
			if (hakios_qcabt_h4_send_cmd(b, mkcmd(b, OP_LE_EXT_CONN, p, sizeof(p)), false) != 0) {
				s.pend_conn--;
				const uint8_t st[7] = { H4_EVT, EV_CMD_STATUS, 4, 0x03, 1, (uint8_t)op, (uint8_t)(op >> 8) };
				hakios_qcabt_h4_inject(st, sizeof(st));
			}
			return HAKIOS_QCABT_SHIM_DONE;
		}
	case OP_LE_ADV_PARAM:
		if (plen < 15) {
			break;
		}
		s.ch.n = 1;
		s.ch.len[0] = mk_ext_adv_param(s.ch.cmd[0], l);
		s.ch.want_type = l[4];
		if ((l[5] == 0x01 || l[5] == 0x03) && s.rnd_known) {
			const uint8_t p[7] = { 0x00, s.rnd[0], s.rnd[1], s.rnd[2], s.rnd[3], s.rnd[4], s.rnd[5] };
			s.ch.len[1] = mkcmd(s.ch.cmd[1], OP_LE_SET_RAND, p, sizeof(p));
			s.ch.n = 2;
		}
		return chain_start(op, 0x01);
	case OP_LE_ADV_DATA:
	case OP_LE_SCAN_RSP:
		if (plen < 1) {
			break;
		} else if (s.adv_set && ((op == OP_LE_ADV_DATA && (s.adv_type == 0x01 || s.adv_type == 0x04)) ||
		    (op == OP_LE_SCAN_RSP && l[0] != 0 && s.adv_type != 0x00 && s.adv_type != 0x02))) {
			/* review F4: legacy takes and ignores data the PDU type cannot carry; 0x2037/0x2038 answer 0x12 */
			if (s.n_xlat < 64) {
				SAY("lext: %#06x for legacy type %u: the PDU carries no such data; local status 0\n", op,
				    s.adv_type);
			}
			answer(op, 0);
			return HAKIOS_QCABT_SHIM_DONE;
		} else {
			uint8_t dl = MIN(l[0], (uint8_t)31);
			dl = (uint8_t)MIN((uint32_t)dl, (uint32_t)plen - 1u);
			uint8_t p[4 + 31];
			p[0] = 0x00;                    /* set 0 */
			p[1] = 0x03;                    /* complete data */
			p[2] = 0x01;                    /* no fragmentation preference */
			p[3] = dl;
			memcpy(p + 4, l + 1, dl);
			uint8_t k = 0;
			if (!s.adv_set) {
				s.ch.len[k++] = mk_ext_adv_param(s.ch.cmd[0], NULL);
				s.ch.want_type = 0x00;
			}
			s.ch.len[k] = mkcmd(s.ch.cmd[k], op == OP_LE_ADV_DATA ? OP_LE_EXT_ADV_DATA : OP_LE_EXT_SCAN_RSP, p,
			    (uint8_t)(4u + dl));
			s.ch.n = (uint8_t)(k + 1u);
			return chain_start(op, 0x03);
		}
	case OP_LE_ADV_ENABLE:
		if (plen < 1) {
			break;
		} else {
			if (!s.adv_set && l[0] == 0) {
				answer(op, 0);          /* nothing advertises: a legacy disable succeeds */
				return HAKIOS_QCABT_SHIM_DONE;
			}
			/* review F3: high duty directed needs a duration (1.28 s = 0x0080 x 10 ms), as the legacy timeout */
			bool hd = s.adv_set && s.adv_type == 0x01 && l[0] != 0;
			const uint8_t p[6] = { l[0] ? 0x01 : 0x00, 0x01, 0x00, hd ? 0x80 : 0x00, 0x00, 0x00 };
			uint8_t k = 0;
			if (!s.adv_set) {
				s.ch.len[k++] = mk_ext_adv_param(s.ch.cmd[0], NULL);
				s.ch.want_type = 0x00;
			}
			s.ch.len[k] = mkcmd(s.ch.cmd[k], OP_LE_EXT_ADV_EN, p, sizeof(p));
			s.ch.n = (uint8_t)(k + 1u);
			return chain_start(op, 0x03);
		}
	case OP_LE_RAND_ADDR:
		if (plen < 6) {
			break;
		}
		memcpy(s.rnd, l, 6);
		s.rnd_known = true;
		if (!s.adv_set) {
			return HAKIOS_QCABT_SHIM_PASS;
		} else {
			const uint8_t p[7] = { 0x00, l[0], l[1], l[2], l[3], l[4], l[5] };
			s.ch.len[0] = mkcmd(s.ch.cmd[0], OP_LE_RAND_ADDR, l, 6);
			s.ch.len[1] = mkcmd(s.ch.cmd[1], OP_LE_SET_RAND, p, sizeof(p));
			s.ch.n = 2;
			return chain_start(op, 0x01);
		}
	default:
		break;
	}
	return HAKIOS_QCABT_SHIM_PASS;
}

/* LE Extended Advertising Report -> LE Advertising Report(s); always HAKIOS_QCABT_SHIM_DROP for the original */
static int
lext_report(const uint8_t *pkt, uint32_t len)
{
	uint32_t end = MIN(len, 3u + pkt[2]);
	uint32_t at = 5;                        /* 04 3e plen 0d num */
	uint8_t num = len > 4 ? pkt[4] : 0;
	for (uint8_t r = 0; r < num && at + 24u <= end; r++) {
		const uint8_t *e = pkt + at;
		uint16_t et = le16(e);
		uint8_t dl = e[23];
		if (at + 24u + dl > end) {
			break;
		}
		at += 24u + dl;
		s.n_rep_in++;
		if ((et & 0x10u) == 0 || dl > 31 || e[2] == 0xff) {
			s.n_rep_drop++;         /* extended PDU (a legacy scan does not see it) or anonymous */
			continue;
		}
		uint8_t lt;
		if (et & 0x08u) {
			lt = 0x04;              /* SCAN_RSP */
		} else if (et & 0x04u) {
			lt = 0x01;              /* ADV_DIRECT_IND */
		} else if ((et & 0x03u) == 0x03u) {
			lt = 0x00;              /* ADV_IND */
		} else if (et & 0x02u) {
			lt = 0x02;              /* ADV_SCAN_IND */
		} else {
			lt = 0x03;              /* ADV_NONCONN_IND */
		}
		uint8_t ev[3 + 12 + 31];        /* plen = sub, num, type, addr type, addr(6), len, data, RSSI = 12 + dl */
		ev[0] = H4_EVT;
		ev[1] = EV_LE_META;
		ev[2] = (uint8_t)(12u + dl);
		ev[3] = 0x02;
		ev[4] = 1;
		ev[5] = lt;
		ev[6] = e[2];
		memcpy(ev + 7, e + 3, 6);
		ev[13] = dl;
		memcpy(ev + 14, e + 24, dl);
		ev[14 + dl] = e[13];            /* RSSI */
		s.n_rep_out++;
		hakios_qcabt_h4_inject(ev, 14u + dl + 1u);
	}
	if (s.n_rep_in > 0 && s.n_rep_in != s.n_rep_logged && (s.n_rep_logged == 0 || s.n_rep_in - s.n_rep_logged >= 1024u)) {
		s.n_rep_logged = s.n_rep_in;
		SAY("lext: extended advertising reports %u in, %u legacy reports out, %u dropped (extended PDU)\n",
		    s.n_rep_in, s.n_rep_out, s.n_rep_drop);
	}
	return HAKIOS_QCABT_SHIM_DROP;
}

/* controller -> host in translation mode; HAKIOS_QCABT_SHIM_PASS = go on with the normal rx checks */
static int
lext_rx(uint8_t *pkt, uint32_t len)
{
	if (pkt[1] == EV_LE_META && len >= 4) {
		if (pkt[3] == 0x0d) {
			return lext_report(pkt, len);
		}
		if (pkt[3] == 0x12 || pkt[3] == 0x11) {
			if (s.n_ev_drop++ < 8) {
				SAY("lext: LE Meta sub %#x (%s) not delivered (bluetoothd runs legacy advertising/scanning)\n",
				    pkt[3], pkt[3] == 0x12 ? "Advertising Set Terminated" : "Scan Timeout");
			}
			return HAKIOS_QCABT_SHIM_DROP;
		}
		return HAKIOS_QCABT_SHIM_PASS;
	}
	bool cc = pkt[1] == EV_CMD_COMPLETE && pkt[2] >= 4 && len >= 7;
	bool cs = pkt[1] == EV_CMD_STATUS && pkt[2] >= 4 && len >= 7;
	if (!cc && !cs) {
		return HAKIOS_QCABT_SHIM_PASS;
	}
	uint8_t *opp = cc ? pkt + 4 : pkt + 5;
	uint16_t op = le16(opp);
	uint8_t st = cc ? pkt[6] : pkt[3];
	if (chain_rx(op, st)) {
		return HAKIOS_QCABT_SHIM_DROP;
	}
	uint16_t to = 0;
	if (op == OP_LE_EXT_SCAN_PARAM && s.pend_scan_p) {
		s.pend_scan_p--;
		to = OP_LE_SCAN_PARAM;
	} else if (op == OP_LE_EXT_SCAN_EN && s.pend_scan_e) {
		s.pend_scan_e--;
		to = OP_LE_SCAN_ENABLE;
	} else if (op == OP_LE_EXT_CONN && s.pend_conn) {
		s.pend_conn--;
		to = OP_LE_CREATE_CONN;
	}
	if (to) {
		opp[0] = (uint8_t)to;
		opp[1] = (uint8_t)(to >> 8);
		if (st != 0 || s.n_xlat < 16) {
			SAY("lext: %#06x answer (status %#x) delivered as legacy %#06x\n", op, st, to);
		}
	}
	return HAKIOS_QCABT_SHIM_PASS;
}

int
hakios_qcabt_shim_tx(uint8_t *pkt, uint32_t len)
{
	if (s.policy == 0 || len < 4 || pkt[0] != H4_CMD) {
		return HAKIOS_QCABT_SHIM_PASS;
	}
	uint16_t op = le16(pkt + 1);
	if (s.lext && (op >> 10) == 0x08u) {
		int r = lext_tx(pkt, len);
		if (r != HAKIOS_QCABT_SHIM_PASS) {
			return r;
		}
	} else if (s.lext && op == OP_RESET) {
		(void)lext_tx(pkt, len);
	}
	if (s.fix && op == OP_WRITE_LE_HOST && len >= 6 && pkt[3] >= 2 && pkt[5] != 0) {
		if (!s.lehost_logged) {
			s.lehost_logged = true;
			SAY("init fix: 0x0c6d Write_LE_Host_Support LE %#x Simultaneous_LE_Host %#x -> 0x00 (Core 4.1+), sent to "
			    "the chip\n", pkt[4], pkt[5]);
		}
		pkt[5] = 0;
		return HAKIOS_QCABT_SHIM_PASS;
	}
	if (s.fix && s.cmds_known && ((op == OP_WRITE_ERR_DATA && !s.chip_err_wr) ||
	    (op == OP_READ_ERR_DATA && !s.chip_err_rd))) {
		uint16_t k = s.seen[op & (SEEN_SLOTS - 1u)]++;
		s.n_local++;
		if (k < 4 || (k % 64u) == 0) {
			SAY("init fix: %#06x plen %u (#%u) answered locally with status 0 (not in the chip's supported-commands "
			    "mask)\n", op, pkt[3], k + 1u);
		}
		if (op == OP_READ_ERR_DATA) {
			const uint8_t ev[8] = { H4_EVT, EV_CMD_COMPLETE, 5, 1, (uint8_t)op, (uint8_t)(op >> 8), 0, 0 };
			hakios_qcabt_h4_inject(ev, sizeof(ev));
		} else {
			answer(op, 0);
		}
		return HAKIOS_QCABT_SHIM_DONE;
	}
	if ((op >> 10) != OGF_VENDOR) {
		return HAKIOS_QCABT_SHIM_PASS;
	}
	uint8_t status = 0x01;
	const char *name = "not in the table (Broadcom/Apple VSC; the QCA opcode space differs)";
	for (size_t i = 0; i < sizeof(shim_ops) / sizeof(shim_ops[0]); i++) {
		if (shim_ops[i].op == op) {
			status = shim_ops[i].status;
			name = shim_ops[i].name;
			break;
		}
	}
	if (s.policy == 2) {
		status = 0x00;
	}
	uint16_t k = s.seen[op & (SEEN_SLOTS - 1u)]++;
	s.n_local++;
	if (k < 4 || (k % 64u) == 0) {
		SAY("intercepted %#06x plen %u (#%u) -> local Command Complete status %#x + %u data bytes: %s\n", op, pkt[3],
		    k + 1u, status, s.vscdata, name);
	}
	answer_vsc(op, status, pkt[3] > 0 && len >= 5 ? pkt[4] : 0);
	return HAKIOS_QCABT_SHIM_DONE;
}

int
hakios_qcabt_shim_rx(uint8_t *pkt, uint32_t len)
{
	if (s.policy == 0 || len < 3 || pkt[0] != H4_EVT) {
		return HAKIOS_QCABT_SHIM_PASS;
	}
	if (pkt[1] == EV_VENDOR) {
		if (s.n_vse++ < 16) {
			SAY("controller vendor event (plen %u) not delivered (QCA debug/log; iOS reads 0xff as its own)\n",
			    pkt[2]);
		}
		return HAKIOS_QCABT_SHIM_DROP;
	}
	if (s.lext && lext_rx(pkt, len) == HAKIOS_QCABT_SHIM_DROP) {
		return HAKIOS_QCABT_SHIM_DROP;
	}
	/* 0336: the supported-commands mask (04 0e 44 01 02 10 00 <64 bytes>): log it, add the Erroneous Data bits */
	if (pkt[1] == EV_CMD_COMPLETE && pkt[2] >= 68 && len >= 71 && le16(pkt + 4) == OP_READ_SUPPORTED_COMMANDS &&
	    pkt[6] == 0) {
		uint8_t *m = pkt + 7;
		if (!s.cmds_known) {
			char hx[3 * 32 + 1];
			for (unsigned half = 0; half < 2; half++) {
				for (unsigned i = 0; i < 32; i++) {
					snprintf(hx + 3 * i, 4, " %02x", m[32 * half + i]);
				}
				SAY("supported commands octets %u-%u:%s\n", 32 * half, 32 * half + 31, hx);
			}
		}
		s.cmds_known = true;
		s.chip_err_rd = (m[CMDS_OCTET_ERR] & 0x04u) != 0;
		s.chip_err_wr = (m[CMDS_OCTET_ERR] & 0x08u) != 0;
		if (s.fix && (m[CMDS_OCTET_ERR] & CMDS_ERR_BITS) != CMDS_ERR_BITS) {
			SAY("supported commands: octet 18 %#x -> %#x (Read/Write_Default_Erroneous_Data_Reporting answered here)\n",
			    m[CMDS_OCTET_ERR], m[CMDS_OCTET_ERR] | CMDS_ERR_BITS);
			m[CMDS_OCTET_ERR] |= CMDS_ERR_BITS;
		}
	}
	/* 0337: chip answers that bluetoothd treats as fatal or that block its init */
	if (s.fix && pkt[1] == EV_CMD_COMPLETE && pkt[2] >= 4 && len >= 7 && pkt[6] != 0) {
		uint16_t op = le16(pkt + 4);
		if (op == OP_WRITE_LE_HOST) {
			if (!s.lehost_rej_logged) {
				s.lehost_rej_logged = true;
				SAY("init fix: the chip still rejects 0x0c6d (status %#x) -> status 0\n", pkt[6]);
			}
			pkt[6] = 0;
		} else if (op == OP_LE_READ_ADV_TX_POWER && pkt[6] == 0x0c && pkt[2] >= 5 && len >= 8) {
			if (!s.advtx_logged) {
				s.advtx_logged = true;
				SAY("init fix: 0x2007 LE_Read_Advertising_Physical_Channel_Tx_Power status 0xc (Command Disallowed) "
				    "-> status 0, 0 dBm\n");
			}
			pkt[6] = 0;
			pkt[7] = 0;
		}
	}
	/* 0336: log every standard command the chip rejects (Command Complete or Command Status with status != 0) */
	if ((pkt[1] == EV_CMD_COMPLETE && pkt[2] >= 4 && len >= 7 && pkt[6] != 0) ||
	    (pkt[1] == EV_CMD_STATUS && pkt[2] >= 4 && len >= 7 && pkt[3] != 0)) {
		bool cc = pkt[1] == EV_CMD_COMPLETE;
		uint16_t op = cc ? le16(pkt + 4) : le16(pkt + 5);
		uint8_t st = cc ? pkt[6] : pkt[3];
		if ((op >> 10) != OGF_VENDOR && s.rej[op & (SEEN_SLOTS - 1u)]++ < 4) {
			SAY("controller rejected %#06x: %s status %#x\n", op, cc ? "Command Complete" : "Command Status", st);
		}
	}
	/* 04 0e plen ncmd op(2) status hci_ver hci_rev(2) lmp_ver mfr(2) lmp_subver(2): plen 12, len 15 */
	if (pkt[1] == EV_CMD_COMPLETE && pkt[2] >= 12 && len >= 15 && le16(pkt + 4) == OP_READ_LOCAL_VERSION &&
	    pkt[6] == 0) {
		uint16_t mfr = le16(pkt + 11);
		if (!s.version_logged) {
			s.version_logged = true;
			SAY("Read_Local_Version: HCI %u rev %#06x, LMP %u subversion %#06x, manufacturer %#06x%s\n", pkt[7],
			    le16(pkt + 8), pkt[10], le16(pkt + 13), mfr, mfr == 0x1d ? " (Qualcomm)" : "");
		}
		if (s.mfr && s.mfr != mfr) {
			pkt[11] = (uint8_t)s.mfr;
			pkt[12] = (uint8_t)(s.mfr >> 8);
			SAY("Read_Local_Version: manufacturer %#06x rewritten to %#06x (qcabt_mfr)\n", mfr, s.mfr & 0xffffu);
		}
	}
	return HAKIOS_QCABT_SHIM_PASS;
}
#endif /* ARM64_BOARD_CONFIG_QCABT */
