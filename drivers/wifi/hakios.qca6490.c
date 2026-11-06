/*
 * The talk with the Wi-Fi firmware's service ("WLFW") on the Xiaomi 12S Pro's QCA6490, over QMI and QRTR on MHI (the transport
 * is in hakios.adreno7.c): in the order of ath11k's qmi.c for a chip of this family, and of what Android's cnss2 logs on
 * this phone. Which indications we want, what the host can do; the firmware asks for memory and is given it; what the
 * chip is; the regulatory database and the board's file in pieces; where m3 is; "ready", then the configuration and the
 * mode "mission" as far as drivers/wifi/hakios.wlfw.c knows them for its own chip.
 *
 * The messages' structures and tables are ath11k's, taken out of its sources when the kernel is built (tools/mk-wlfw.sh)
 * and not kept anywhere, as for the redwood's hakios.wlfw.c. Without them this says so.
 */
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/hakios.qmi.h>
#include <sys/systm.h>
#include <vm/vm_kern.h>
#include <mach/vm_param.h>

extern bool hakios_unicorn_wifi_memory(uint64_t address, vm_offset_t memory, uint64_t size);
extern const uint8_t *hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk);
extern void IOSleep(unsigned int milliseconds);
extern vm_offset_t kvtophys(vm_offset_t va);

void hakios_unicorn_wlfw(void);

#if !__has_include("hakios.wlfw_ath11k.h")

void
hakios_unicorn_wlfw(void)
{
	printf("wifi: wlfw: FAILED: ath11k's message descriptions were not there when this kernel was built (tools/mk-wlfw.sh)\n");
}

#else

#define HAKIOS_QMI_NO_KERNEL_CONNECT
#include "hakios.wlfw_ath11k.h"

#define WLFW_MEMORY_AT          0xa1000000ULL   /* behind the chip's addresses, after MHI's pool */
#define WLFW_MEMORY_STEP        0x1000000ULL

static struct qmi_handle handle;
static volatile bool server, gone, memory_asked, memory_ready, ready, init_done, cal_done;
static struct {
	uint32_t type, size;
	uint64_t address;
} memory[ATH11K_QMI_WLANFW_MAX_NUM_MEM_SEG_V01];
static uint32_t memory_count;

static int
wlfw_new_server(struct qmi_handle *qmi, struct qmi_service *service)
{
	int error = hakios_qmi_connect(qmi->sock, service->node, service->port);

	printf("wifi: wlfw: the service at node %u port %u: %d\n", service->node, service->port, error);
	server = error == 0;
	return error;
}

static void
wlfw_del_server(__unused struct qmi_handle *qmi, struct qmi_service *service)
{
	printf("wifi: wlfw: THE SERVICE AT NODE %u PORT %u HAS GONE\n", service->node, service->port);
	gone = true;
}

static void
wlfw_memory_request(__unused struct qmi_handle *qmi, __unused struct sockaddr_qrtr *sq, __unused struct qmi_txn *txn, const void *decoded)
{
	const struct qmi_wlanfw_request_mem_ind_msg_v01 *message = decoded;

	memory_count = message->mem_seg_len;
	for (uint32_t i = 0; i < memory_count && i < ATH11K_QMI_WLANFW_MAX_NUM_MEM_SEG_V01; i++) {
		memory[i].type = message->mem_seg[i].type;
		memory[i].size = message->mem_seg[i].size;
		printf("wifi: wlfw: the firmware asks for memory: type %u, %#x bytes\n", memory[i].type, memory[i].size);
	}
	memory_asked = true;
}

static void
wlfw_memory_ready(__unused struct qmi_handle *qmi, __unused struct sockaddr_qrtr *sq, __unused struct qmi_txn *txn, __unused const void *decoded)
{
	printf("wifi: wlfw: the firmware says its memory is ready\n");
	memory_ready = true;
}

static void
wlfw_ready(__unused struct qmi_handle *qmi, __unused struct sockaddr_qrtr *sq, __unused struct qmi_txn *txn, __unused const void *decoded)
{
	printf("wifi: wlfw: THE FIRMWARE IS READY\n");
	ready = true;
}

static void
wlfw_init_done(__unused struct qmi_handle *qmi, __unused struct sockaddr_qrtr *sq, __unused struct qmi_txn *txn, __unused const void *decoded)
{
	printf("wifi: wlfw: the firmware says init done\n");
	init_done = true;
}

static void
wlfw_cal_done(__unused struct qmi_handle *qmi, __unused struct sockaddr_qrtr *sq, __unused struct qmi_txn *txn, __unused const void *decoded)
{
	printf("wifi: wlfw: the firmware says calibration done\n");
	cal_done = true;
}

static const struct qmi_ops wlfw_ops = { .new_server = wlfw_new_server, .del_server = wlfw_del_server };
static const struct qmi_msg_handler wlfw_handlers[] = {
	{ QMI_INDICATION, QMI_WLFW_REQUEST_MEM_IND_V01, qmi_wlanfw_request_mem_ind_msg_v01_ei,
	  sizeof(struct qmi_wlanfw_request_mem_ind_msg_v01), wlfw_memory_request },
	{ QMI_INDICATION, QMI_WLFW_FW_MEM_READY_IND_V01, qmi_wlanfw_mem_ready_ind_msg_v01_ei,
	  sizeof(struct qmi_wlanfw_fw_mem_ready_ind_msg_v01), wlfw_memory_ready },
	{ QMI_INDICATION, QMI_WLFW_FW_READY_IND_V01, qmi_wlanfw_fw_ready_ind_msg_v01_ei,
	  sizeof(struct qmi_wlanfw_fw_ready_ind_msg_v01), wlfw_ready },
	{ QMI_INDICATION, QMI_WLFW_COLD_BOOT_CAL_DONE_IND_V01, qmi_wlanfw_cold_boot_cal_done_ind_msg_v01_ei,
	  sizeof(struct qmi_wlanfw_fw_cold_cal_done_ind_msg_v01), wlfw_cal_done },
	{ QMI_INDICATION, QMI_WLFW_FW_INIT_DONE_IND_V01, qmi_wlfw_fw_init_done_ind_msg_v01_ei,
	  sizeof(struct qmi_wlfw_fw_init_done_ind_msg_v01), wlfw_init_done },
	{ 0 },
};

static bool
ask(const char *what, int message, size_t largest, const struct qmi_elem_info *request_ei, const void *request,
    const struct qmi_elem_info *reply_ei, void *reply)
{
	const struct qmi_response_type_v01 *result = reply;
	struct qmi_txn txn;
	int ret = qmi_txn_init(&handle, &txn, reply_ei, reply);

	if (ret >= 0) {
		ret = qmi_send_request(&handle, NULL, &txn, message, largest, request_ei, request);
		if (ret < 0) {
			qmi_txn_cancel(&txn);
		} else {
			ret = hakios_qmi_txn_wait_ms(&txn, ATH11K_QMI_WLANFW_TIMEOUT_MS);
		}
	}
	if (ret >= 0 && result->result == QMI_RESULT_SUCCESS_V01) {
		return true;
	}
	printf("wifi: wlfw: FAILED: %s (message %#x): %d, result %u, error %u\n", what, message, ret, result->result, result->error);
	return false;
}

static bool
wait_for(volatile bool *flag, uint32_t ms)
{
	for (uint32_t i = 0; i < ms && !*flag && !gone; i += 5) {
		IOSleep(5);
	}
	return *flag;
}

static const uint8_t *
file_of(const char *name, uint64_t *size)
{
	int disk = -1;
	const uint8_t *data = hakios_image_file("fw", name, size, &disk);

	if (data == NULL) {
		printf("wifi: wlfw: FAILED: no fw/%s\n", name);
	}
	return data;
}

/* As ath11k_qmi_load_file_target_mem(). */
static bool
send_file(const char *name, uint8_t type, uint32_t board_id)
{
	static struct qmi_wlanfw_bdf_download_req_msg_v01 request;
	struct qmi_wlanfw_bdf_download_resp_msg_v01 reply;
	uint64_t size = 0;
	const uint8_t *data = file_of(name, &size);
	uint32_t left, pieces = 0;

	if (data == NULL) {
		return false;
	}
	bzero(&request, sizeof(request));
	for (left = (uint32_t)size; left > 0; pieces++) {
		request.valid = 1;
		request.file_id_valid = 1;
		request.file_id = board_id;
		request.total_size_valid = 1;
		request.total_size = left;
		request.seg_id_valid = 1;
		request.data_valid = 1;
		request.bdf_type = type;
		request.bdf_type_valid = 1;
		request.end_valid = 1;
		request.end = left <= QMI_WLANFW_MAX_DATA_SIZE_V01;
		request.data_len = request.end ? left : QMI_WLANFW_MAX_DATA_SIZE_V01;
		memcpy(request.data, data, request.data_len);
		bzero(&reply, sizeof(reply));
		if (!ask(name, QMI_WLANFW_BDF_DOWNLOAD_REQ_V01, QMI_WLANFW_BDF_DOWNLOAD_REQ_MSG_V01_MAX_LEN,
		    qmi_wlanfw_bdf_download_req_msg_v01_ei, &request, qmi_wlanfw_bdf_download_resp_msg_v01_ei, &reply)) {
			return false;
		}
		left -= request.data_len;
		data += request.data_len;
		request.seg_id++;
	}
	printf("wifi: wlfw: %s: %llu bytes taken, in %u pieces\n", name, size, pieces);
	return true;
}

static bool
give_memory(void)
{
	static struct qmi_wlanfw_respond_mem_req_msg_v01 request;
	struct qmi_wlanfw_respond_mem_resp_msg_v01 reply;

	bzero(&request, sizeof(request));
	bzero(&reply, sizeof(reply));
	request.mem_seg_len = memory_count;
	for (uint32_t i = 0; i < memory_count; i++) {
		vm_offset_t piece = 0;

		if (kmem_alloc(kernel_map, &piece, round_page(memory[i].size), KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
			printf("wifi: wlfw: FAILED: no memory for %#x bytes\n", memory[i].size);
			return false;
		}
		memory[i].address = WLFW_MEMORY_AT + i * WLFW_MEMORY_STEP;
		for (vm_offset_t line = piece; line < piece + memory[i].size; line += 64) {
			__asm__ volatile ("dc civac, %0" : : "r" (line));
		}
		__asm__ volatile ("dsb sy");
		if (!hakios_unicorn_wifi_memory(memory[i].address, piece, memory[i].size)) {
			printf("wifi: wlfw: FAILED: no page tables for %#x bytes\n", memory[i].size);
			return false;
		}
		request.mem_seg[i].addr = memory[i].address;
		request.mem_seg[i].size = memory[i].size;
		request.mem_seg[i].type = memory[i].type;
	}
	return ask("the memory it asked for", QMI_WLANFW_RESPOND_MEM_REQ_V01, QMI_WLANFW_RESPOND_MEM_REQ_MSG_V01_MAX_LEN,
	           qmi_wlanfw_respond_mem_req_msg_v01_ei, &request, qmi_wlanfw_respond_mem_resp_msg_v01_ei, &reply);
}

/*
 * Step 12: ath11k_qmi_firmware_start(), mode "mission": the firmware's logging, the configuration (the copy engines'
 * pipes and the map of services: ath11k's tables for this family), the mode. The list of shadow registers ath11k sends is
 * left out for now: the driver works it out from its rings, and it has none yet.
 */
static void
mission(void)
{
	static struct qmi_wlanfw_wlan_cfg_req_msg_v01 cfg;
	struct qmi_wlanfw_wlan_cfg_resp_msg_v01 cfg_reply;
	struct qmi_wlanfw_wlan_ini_req_msg_v01 ini;
	struct qmi_wlanfw_wlan_ini_resp_msg_v01 ini_reply;
	struct qmi_wlanfw_wlan_mode_req_msg_v01 mode;
	struct qmi_wlanfw_wlan_mode_resp_msg_v01 mode_reply;

	bzero(&ini, sizeof(ini));
	bzero(&ini_reply, sizeof(ini_reply));
	ini.enablefwlog_valid = 1;
	ini.enablefwlog = 1;
	if (!ask("the firmware's logging", QMI_WLANFW_WLAN_INI_REQ_V01, QMI_WLANFW_WLAN_INI_REQ_MSG_V01_MAX_LEN,
	    qmi_wlanfw_wlan_ini_req_msg_v01_ei, &ini, qmi_wlanfw_wlan_ini_resp_msg_v01_ei, &ini_reply)) {
		return;
	}
	bzero(&cfg, sizeof(cfg));
	bzero(&cfg_reply, sizeof(cfg_reply));
	cfg.host_version_valid = 1;
	strlcpy(cfg.host_version, ATH11K_HOST_VERSION_STRING, sizeof(cfg.host_version));
	cfg.tgt_cfg_valid = 1;
	cfg.tgt_cfg_len = WLFW_PIPE_COUNT;
	for (uint32_t i = 0; i < cfg.tgt_cfg_len; i++) {
		cfg.tgt_cfg[i].pipe_num = WLFW_PIPES[i].pipenum;
		cfg.tgt_cfg[i].pipe_dir = WLFW_PIPES[i].pipedir;
		cfg.tgt_cfg[i].nentries = WLFW_PIPES[i].nentries;
		cfg.tgt_cfg[i].nbytes_max = WLFW_PIPES[i].nbytes_max;
		cfg.tgt_cfg[i].flags = WLFW_PIPES[i].flags;
	}
	cfg.svc_cfg_valid = 1;
	cfg.svc_cfg_len = WLFW_SERVICE_COUNT;
	for (uint32_t i = 0; i < cfg.svc_cfg_len; i++) {
		cfg.svc_cfg[i].service_id = WLFW_SERVICES[i].service_id;
		cfg.svc_cfg[i].pipe_dir = WLFW_SERVICES[i].pipedir;
		cfg.svc_cfg[i].pipe_num = WLFW_SERVICES[i].pipenum;
	}
	if (!ask("the configuration", QMI_WLANFW_WLAN_CFG_REQ_V01, QMI_WLANFW_WLAN_CFG_REQ_MSG_V01_MAX_LEN,
	    qmi_wlanfw_wlan_cfg_req_msg_v01_ei, &cfg, qmi_wlanfw_wlan_cfg_resp_msg_v01_ei, &cfg_reply)) {
		return;
	}
	printf("wifi: wlfw: the configuration taken: %u pipes, %u services\n", cfg.tgt_cfg_len, cfg.svc_cfg_len);
	bzero(&mode, sizeof(mode));
	bzero(&mode_reply, sizeof(mode_reply));
	mode.mode = 0;                  /* core.h: ATH11K_FIRMWARE_MODE_NORMAL, "mission" */
	mode.hw_debug_valid = 1;
	mode.hw_debug = 0;
	if (ask("the mode: mission", QMI_WLANFW_WLAN_MODE_REQ_V01, QMI_WLANFW_WLAN_MODE_REQ_MSG_V01_MAX_LEN,
	    qmi_wlanfw_wlan_mode_req_msg_v01_ei, &mode, qmi_wlanfw_wlan_mode_resp_msg_v01_ei, &mode_reply)) {
		printf("wifi: wlfw: THE RADIO'S FIRMWARE TOOK THE MODE \"MISSION\"\n");
	}
}

void
hakios_unicorn_wlfw(void)
{
	static struct qmi_wlanfw_cap_resp_msg_v01 target;
	struct qmi_wlanfw_ind_register_req_msg_v01 indications;
	struct qmi_wlanfw_ind_register_resp_msg_v01 indications_reply;
	struct qmi_wlanfw_host_cap_req_msg_v01 host;
	struct qmi_wlanfw_host_cap_resp_msg_v01 host_reply;
	struct qmi_wlanfw_cap_req_msg_v01 what = { 0 };
	struct qmi_wlanfw_m3_info_req_msg_v01 m3;
	struct qmi_wlanfw_m3_info_resp_msg_v01 m3_reply;
	const uint8_t *m3_file;
	vm_offset_t m3_memory = 0;
	uint64_t m3_size = 0;
	uint32_t board_id, sleep_clock = 0;
	int ret = qmi_handle_init(&handle, ATH11K_QMI_RESP_LEN_MAX, &wlfw_ops, wlfw_handlers);

	if (ret < 0) {
		printf("wifi: wlfw: FAILED: no handle: %d\n", ret);
		return;
	}
	ret = qmi_add_lookup(&handle, ATH11K_QMI_WLFW_SERVICE_ID_V01, ATH11K_QMI_WLFW_SERVICE_VERS_V01, ATH11K_QMI_WLFW_SERVICE_INS_ID_V01_QCA6390);
	if (ret < 0 || !wait_for(&server, 5000)) {
		printf("wifi: wlfw: FAILED: the service does not appear (%d)\n", ret);
		return;
	}
	bzero(&indications, sizeof(indications));
	bzero(&indications_reply, sizeof(indications_reply));
	indications.client_id_valid = 1;
	indications.client_id = QMI_WLANFW_CLIENT_ID;
	indications.fw_ready_enable_valid = 1;
	indications.fw_ready_enable = 1;
	indications.cal_done_enable_valid = 1;
	indications.cal_done_enable = 1;
	indications.fw_init_done_enable_valid = 1;
	indications.fw_init_done_enable = 1;
	indications.request_mem_enable_valid = 1;
	indications.request_mem_enable = 1;
	indications.fw_mem_ready_enable_valid = 1;
	indications.fw_mem_ready_enable = 1;
	if (!ask("indications", QMI_WLANFW_IND_REGISTER_REQ_V01, QMI_WLANFW_IND_REGISTER_REQ_MSG_V01_MAX_LEN,
	    qmi_wlanfw_ind_register_req_msg_v01_ei, &indications, qmi_wlanfw_ind_register_resp_msg_v01_ei, &indications_reply)) {
		return;
	}
	bzero(&host, sizeof(host));
	bzero(&host_reply, sizeof(host_reply));
	host.num_clients_valid = 1;
	host.num_clients = 1;
	host.mem_cfg_mode_valid = 1;
	host.mem_cfg_mode = 0;
	host.bdf_support_valid = 1;
	host.bdf_support = 1;
	host.m3_support_valid = 1;
	host.m3_support = 1;
	host.m3_cache_support_valid = 1;
	host.m3_cache_support = 1;
	host.cal_done_valid = 1;
	host.cal_done = 0;
	/* boot-arg wifi_sleep_clock=1: ath11k's for a machine that is no Qualcomm platform; this phone is one */
	PE_parse_boot_argn("wifi_sleep_clock", &sleep_clock, sizeof(sleep_clock));
	if (sleep_clock != 0) {
		host.nm_modem_valid = 1;
		host.nm_modem = 0x04 | 0x02;            /* qmi.c: HOST_CSTATE_BIT, SLEEP_CLOCK_SELECT_INTERNAL_BIT */
	}
	if (!ask("what the host can do", QMI_WLANFW_HOST_CAP_REQ_V01, QMI_WLANFW_HOST_CAP_REQ_MSG_V01_MAX_LEN,
	    qmi_wlanfw_host_cap_req_msg_v01_ei, &host, qmi_wlanfw_host_cap_resp_msg_v01_ei, &host_reply)) {
		return;
	}
	printf("wifi: wlfw: indications and capabilities taken; waiting for its request for memory\n");
	if (!wait_for(&memory_asked, 10000)) {
		printf("wifi: wlfw: FAILED: no request for memory\n");
		return;
	}
	if (!give_memory() || !wait_for(&memory_ready, 10000)) {
		printf("wifi: wlfw: FAILED: its memory did not become ready\n");
		return;
	}
	bzero(&target, sizeof(target));
	if (!ask("what the chip is", QMI_WLANFW_CAP_REQ_V01, QMI_WLANFW_CAP_REQ_MSG_V01_MAX_LEN,
	    qmi_wlanfw_cap_req_msg_v01_ei, &what, qmi_wlanfw_cap_resp_msg_v01_ei, &target)) {
		return;
	}
	board_id = target.board_info_valid ? target.board_info.board_id : 0xff;
	printf("wifi: wlfw: chip id %#x, family %#x, board %#x, soc %#x, firmware %#x \"%s\"\n", target.chip_info.chip_id,
	    target.chip_info.chip_family, board_id, target.soc_info.soc_id, target.fw_version_info.fw_version, target.fw_build_id);
	if (!send_file("regdb_xiaomi.bin", ATH11K_QMI_BDF_TYPE_REGDB, board_id) || !send_file("bd_l3s.elf", ATH11K_QMI_BDF_TYPE_ELF, board_id)) {
		return;
	}
	m3_file = file_of("m3.bin", &m3_size);
	if (m3_file == NULL || kmem_alloc(kernel_map, &m3_memory, round_page(m3_size), KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("wifi: wlfw: FAILED: m3\n");
		return;
	}
	memcpy((void *)m3_memory, m3_file, m3_size);
	for (vm_offset_t line = m3_memory; line < m3_memory + m3_size; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	if (!hakios_unicorn_wifi_memory(WLFW_MEMORY_AT - 0x100000, m3_memory, m3_size)) {
		printf("wifi: wlfw: FAILED: no page tables for m3\n");
		return;
	}
	bzero(&m3, sizeof(m3));
	bzero(&m3_reply, sizeof(m3_reply));
	m3.addr = WLFW_MEMORY_AT - 0x100000;
	m3.size = (uint32_t)m3_size;
	if (!ask("where m3 is", QMI_WLANFW_M3_INFO_REQ_V01, QMI_WLANFW_M3_INFO_REQ_MSG_V01_MAX_MSG_LEN,
	    qmi_wlanfw_m3_info_req_msg_v01_ei, &m3, qmi_wlanfw_m3_info_resp_msg_v01_ei, &m3_reply)) {
		return;
	}
	printf("wifi: wlfw: m3 taken: %llu bytes\n", m3_size);
	wait_for(&init_done, 5000);
	{
		uint32_t step = 11;

		PE_parse_boot_argn("wifi_pcie", &step, sizeof(step));
		if (step < 12) {
			wait_for(&ready, 20000);
			printf("wifi: wlfw: end of step 11: ready %d, init done %d, calibration done %d, the service gone %d\n", ready, init_done, cal_done, gone);
			return;
		}
	}
	mission();
	wait_for(&ready, 20000);
	printf("wifi: wlfw: end of step 12: ready %d, init done %d, calibration done %d, the service gone %d\n", ready, init_done, cal_done, gone);
}

#endif

/*
 * ---- the radio for Linux's ath11k (boot-arg wifi_lx=2) ----------------------------------------------------------------
 *
 * What drivers/wifi/hakios.lx.c asks the phone's Wi-Fi driver (the redwood's answers are in hakios.wifi.c): files by the
 * names Linux has for them, memory behind the chip's addresses, whether the firmware can be spoken to. The PCIe link,
 * MHI and QRTR are hakios.adreno7.c's (wifi_pcie=12); ath11k is started on the thread that would run the stand-in
 * above. The chip's registers ath11k maps itself (wifi/lx/src/lx_bus.c, LX_QCA6490).
 */
#include <stdarg.h>
#include <sys/hakios.input.h>

extern bool hakios_unicorn_wifi_map_physical(uint64_t address, uint64_t physical, uint64_t size);
extern bool hakios_unicorn_wifi_up(void);
extern void loader_log_putc(char c);

#define RADIO_POOL_FIRST        0xa4000000ULL   /* ath11k's memory, behind the chip's addresses: after MHI's and m3's */
#define RADIO_POOL_END          0xb0000000ULL   /* dt: qcom,iommu-dma-addr-pool 0xa0000000 + 0x10000000 */

static uint64_t radio_next = RADIO_POOL_FIRST;

static void
wifi_line(const char *format, va_list list, bool live)
{
	char line[400];

	vsnprintf(line, sizeof(line), format, list);
	if (live) {
		for (const char *c = line; *c != 0; c++) {
			loader_log_putc(*c);
		}
	}
	printf("%s", line);
}

int
hakios_wifi_say(const char *format, ...)
{
	va_list list;

	va_start(list, format);
	wifi_line(format, list, false);
	va_end(list);
	return 0;
}

int
hakios_wifi_tell(const char *format, ...)
{
	va_list list;

	va_start(list, format);
	wifi_line(format, list, true);
	va_end(list);
	return 0;
}

uint32_t
hakios_wifi_option(const char *name)
{
	uint32_t value = 0;

	PE_parse_boot_argn(name, &value, sizeof(value));
	return value;
}

const uint8_t *
hakios_wifi_driver_file(const char *name, uint64_t *size)
{
	static const char *const files[][2] = { { "board.bin", "bd_l3s.elf" }, { "regdb.bin", "regdb_xiaomi.bin" }, { "m3.bin", "m3.bin" } };
	const char *last = strrchr(name, '/');
	int disk = -1;

	last = last == NULL ? name : last + 1;
	for (unsigned i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
		if (strcmp(last, files[i][0]) == 0) {
			const uint8_t *data = hakios_image_file("fw", files[i][1], size, &disk);

			printf("wifi: lx: the driver asks for %s: fw/%s, %s\n", name, files[i][1], data != NULL ? "there" : "NOT THERE");
			return data;
		}
	}
	printf("wifi: lx: the driver asks for %s: not one of ours\n", name);
	return NULL;
}

bool
hakios_wifi_dsp_is_up(void)
{
	bool up = hakios_unicorn_wifi_up();

	printf("wifi: lx: the driver asks whether the chip is up: %s\n", up ? "MHI in mission mode, QRTR's channels open" : "IT IS NOT");
	return up;
}

bool
hakios_wifi_radio_map(uint64_t physical, uint64_t size, unsigned long long *address)
{
	uint64_t at = radio_next;

	size = (size + 4095) & ~4095ULL;
	if (at + size > RADIO_POOL_END || !hakios_unicorn_wifi_map_physical(at, physical, size)) {
		printf("wifi: lx: FAILED: %llu bytes cannot be shown to the chip (%#llx used of the pool)\n", size, radio_next - RADIO_POOL_FIRST);
		return false;
	}
	radio_next = at + size;
	*address = at;
	printf("wifi: lx: %llu KB shown to the chip at %#llx\n", size >> 10, at);
	return true;
}

/* Kernel memory, page by page (hakios.lx.c, lx_os_radio_memory()). */
bool
hakios_wifi_radio_map_memory(vm_offset_t memory, uint64_t size, unsigned long long *address)
{
	uint64_t at = radio_next;

	size = (size + 4095) & ~4095ULL;
	if (at + size > RADIO_POOL_END || !hakios_unicorn_wifi_memory(at, memory, size)) {
		printf("wifi: lx: FAILED: %llu bytes of memory cannot be shown to the chip\n", size);
		return false;
	}
	radio_next = at + size;
	*address = at;
	printf("wifi: lx: %llu KB shown to the chip at %#llx\n", size >> 10, at);
	return true;
}

/*
 * MSI as the phone's own kernel has it (dt: pcie0_msi, qcom,pci-msi at 0x17110040, SPIs 0x300 to 0x31f): the radio writes
 * the interrupt's number to the GIC's SETSPI register, through its SMMU context, where that register is shown to it as a
 * device's page. As the redwood's hakios_wifi_radio_msi().
 */
#define RADIO_MSI_REGISTER      0x17110040ULL
#define RADIO_FIRST_SPI         0x300U

bool
hakios_wifi_radio_msi(unsigned long long *address, uint32_t *data, uint32_t *first_spi)
{
	extern bool hakios_unicorn_wifi_map_device(uint64_t address, uint64_t physical, uint64_t size);
	static uint64_t page;

	if (page == 0) {
		uint64_t at = (radio_next + 0xffff) & ~0xffffULL;

		if (at + 0x10000 > RADIO_POOL_END || !hakios_unicorn_wifi_map_device(at, RADIO_MSI_REGISTER & ~4095ULL, 4096)) {
			hakios_wifi_tell("wifi: lx: FAILED: the GIC's SETSPI cannot be shown to the chip\n");
			return false;
		}
		radio_next = at + 0x10000;
		page = at;
		hakios_wifi_tell("wifi: lx: the GIC's SETSPI (%#llx) is shown to the chip at %#llx\n", RADIO_MSI_REGISTER, page);
	}
	*address = page + (RADIO_MSI_REGISTER & 4095);
	*data = RADIO_FIRST_SPI + 32;
	*first_spi = RADIO_FIRST_SPI + 32;
	return true;
}

bool
hakios_wifi_radio_address(uint8_t *address)
{
	char text[32] = "";
	unsigned count = 0, value = 0, digits = 0;

	if (!PE_parse_boot_arg_str("wifi_mac", text, sizeof(text))) {
		return false;
	}
	for (const char *c = text;; c++) {
		int digit = *c >= '0' && *c <= '9' ? *c - '0' : *c >= 'a' && *c <= 'f' ? *c - 'a' + 10 : *c >= 'A' && *c <= 'F' ? *c - 'A' + 10 : -1;

		if (digit >= 0 && digits < 2) {
			value = value << 4 | (unsigned)digit;
			digits++;
		} else if ((*c == ':' || *c == 0) && digits != 0 && count < 6) {
			address[count++] = (uint8_t)value;
			value = digits = 0;
		} else {
			return false;
		}
		if (*c == 0) {
			break;
		}
	}
	return count == 6 && !(address[0] & 1);
}

bool
hakios_wifi_network(uint8_t *name, unsigned *name_length, uint8_t *passphrase, unsigned *passphrase_length)
{
	uint64_t size = 0;
	int disk = -1;
	const uint8_t *file = hakios_image_file("fw", "network", &size, &disk), *end, *line;
	unsigned lengths[2] = { 0, 0 }, sizes[2] = { *name_length, *passphrase_length };
	uint8_t *where[2] = { name, passphrase };

	if (file == NULL) {
		printf("wifi: lx: which network to join: none was given (fw/network)\n");
		return false;
	}
	end = file + size;
	line = file;
	for (unsigned n = 0; n < 2 && line < end; n++) {
		const uint8_t *stop = line;

		while (stop < end && *stop != '\n') {
			stop++;
		}
		lengths[n] = (unsigned)(stop - line) - (stop > line && stop[-1] == '\r' ? 1 : 0);
		if (lengths[n] > sizes[n]) {
			lengths[n] = 0;
		} else {
			memcpy(where[n], line, lengths[n]);
		}
		line = stop + 1;
	}
	if (lengths[0] == 0 || lengths[1] < 8 || lengths[1] > 63) {
		printf("wifi: lx: fw/network is not a name and a passphrase of 8 to 63\n");
		return false;
	}
	*name_length = lengths[0];
	*passphrase_length = lengths[1];
	return true;
}

/* (the radio is started by hakios.adreno7.c, and taken down with the run for now) */
void
hakios_wifi_init(void)
{
}

void
hakios_wifi_stop(void)
{
}

void
hakios_wifi_quiesce(void)
{
}
