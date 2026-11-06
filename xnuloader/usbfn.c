/*
 * USB serial console for the XNU loader, over EDK2's EFI_USBFN_IO_PROTOCOL.
 *
 * Why this file exists: the loader's own `usb_start()` asks the firmware for Qualcomm's
 * EFI_USB_DEVICE_PROTOCOL, and a blob-free unicornPkg has no such provider, so until now the loader
 * could only talk to the screen. `UsbfnDwc3Dxe` (in the firmware, see docs/USB-SERIAL.md) now provides
 * the standard EFI_USBFN_IO_PROTOCOL -- the controller, the endpoints, the DMA, but no class. This file
 * is that class: a CDC ACM device, so the PC sees a serial port and every `print()` in the loader can
 * be read on it as text.
 *
 * It is written against the loader's own freestanding `efi.h` (no EDK2 headers are on this build's
 * include path), so the protocol structures are declared here, exactly as MdePkg's UsbFunctionIo.h
 * lays them out.
 *
 * What is deliberately simple in this first version, and why:
 *   - USB 2.0 only, one configuration, no isochronous/interrupt traffic of our own.
 *   - Transmit is synchronous and bounded: a ring is flushed with one bulk transfer, and the caller
 *     never waits longer than a few tens of milliseconds. A missing host can never block the loader.
 *   - Nothing is printed from inside the event pump (no allocation, no recursion).
 */

#include "efi.h"

/* ------------------------------------------------------------------------ */
/* EFI_USBFN_IO_PROTOCOL, as MdePkg/Include/Protocol/UsbFunctionIo.h defines it */

/* gEfiUsbFunctionIoProtocolGuid, verbatim from MdePkg/MdePkg.dec (checked 2026-11-06) */
#define USBFN_IO_GUID { 0x32d2963a, 0xfe5d, 0x4f30, { 0xb6, 0x33, 0x6e, 0x5d, 0xc5, 0x58, 0x03, 0xcc } }

typedef struct { uint8_t request_type; uint8_t request; uint16_t value; uint16_t index; uint16_t length; } usbfn_setup;

typedef enum {
	USBFN_MSG_NONE = 0,
	USBFN_MSG_SETUP,
	USBFN_MSG_RX,
	USBFN_MSG_TX,
	USBFN_MSG_DETACH,
	USBFN_MSG_ATTACH,
	USBFN_MSG_RESET,
	USBFN_MSG_SUSPEND,
	USBFN_MSG_RESUME,
	USBFN_MSG_SPEED
} usbfn_message;

typedef enum {
	USBFN_XFER_UNKNOWN = 0,
	USBFN_XFER_COMPLETE,
	USBFN_XFER_ABORTED,
	USBFN_XFER_ACTIVE,
	USBFN_XFER_NONE
} usbfn_xfer_status;

/* UINTN, enum, UINT8, enum, pointer: padded exactly like the firmware's struct */
typedef struct {
	uint64_t bytes;
	uint32_t status;
	uint8_t  ep_index;
	uint8_t  pad0[3];
	uint32_t direction;
	uint32_t pad1;
	void    *buffer;
} usbfn_xfer_result;

typedef union { usbfn_setup udr; usbfn_xfer_result utr; uint32_t ubs; } usbfn_payload;

typedef struct { uint8_t length; uint8_t type; uint16_t bcd_usb; uint8_t device_class; uint8_t subclass;
                 uint8_t protocol; uint8_t max_packet0; uint16_t vid; uint16_t pid; uint16_t bcd_device;
                 uint8_t str_manu; uint8_t str_product; uint8_t str_serial; uint8_t configs; } usbfn_device_desc;
typedef struct { uint8_t length; uint8_t type; uint16_t total; uint8_t interfaces; uint8_t value;
                 uint8_t str_index; uint8_t attributes; uint8_t max_power; } usbfn_config_desc;
typedef struct { uint8_t length; uint8_t type; uint8_t number; uint8_t alternate; uint8_t endpoints;
                 uint8_t class_code; uint8_t subclass; uint8_t protocol; uint8_t str_index; } usbfn_interface_desc;
typedef struct { uint8_t length; uint8_t type; uint8_t address; uint8_t attributes; uint16_t max_packet;
                 uint8_t interval; } usbfn_endpoint_desc;

typedef struct {
	usbfn_interface_desc  *interface_desc;
	usbfn_endpoint_desc  **endpoints;   /* NULL terminated */
} usbfn_interface_info;

typedef struct {
	usbfn_config_desc      *config_desc; /* the whole configuration, wTotalLength bytes */
	usbfn_interface_info  **interfaces;  /* NULL terminated */
} usbfn_config_info;

typedef struct {
	usbfn_device_desc   *device_desc;
	usbfn_config_info  **configs;        /* NULL terminated */
} usbfn_device_info;

#define USBFN_DIR_TX 1u   /* device -> host (IN) */
#define USBFN_DIR_RX 0u   /* host -> device (OUT) */

typedef struct {
	uint32_t revision;
	efi_status (EFIAPI *detect_port)(void *self, uint32_t *port_type);
	efi_status (EFIAPI *configure_enable_endpoints)(void *self, usbfn_device_info *info);
	efi_status (EFIAPI *get_endpoint_max_packet_size)(void *self, uint32_t type, uint32_t speed, uint16_t *size);
	efi_status (EFIAPI *get_device_info)(void *self, uint32_t id, uint16_t *size, void *buffer);
	efi_status (EFIAPI *get_vid_pid)(void *self, uint16_t *vid, uint16_t *pid);
	efi_status (EFIAPI *abort_transfer)(void *self, uint8_t ep, uint32_t dir);
	efi_status (EFIAPI *get_endpoint_stall_state)(void *self, uint8_t ep, uint32_t dir, uint8_t *state);
	efi_status (EFIAPI *set_endpoint_stall_state)(void *self, uint8_t ep, uint32_t dir, uint8_t state);
	efi_status (EFIAPI *handle_event)(void *self, uint32_t *message, uint64_t *size, usbfn_payload *payload);
	efi_status (EFIAPI *transfer)(void *self, uint8_t ep_index, uint32_t dir, uint64_t *size, void *buffer);
	efi_status (EFIAPI *get_max_transfer_size)(void *self, uint8_t ep, uint64_t *size);
	efi_status (EFIAPI *allocate_transfer_buffer)(void *self, uint64_t size, void **buffer);
	efi_status (EFIAPI *free_transfer_buffer)(void *self, void *buffer);
	efi_status (EFIAPI *start_controller)(void *self);
	efi_status (EFIAPI *stop_controller)(void *self);
	efi_status (EFIAPI *set_endpoint_policy)(void *self, uint8_t ep, uint32_t dir, uint32_t type, uint64_t size, void *value);
	efi_status (EFIAPI *get_endpoint_policy)(void *self, uint8_t ep, uint32_t dir, uint32_t type, uint64_t *size, void *value);
} usbfn_io;

/* ------------------------------------------------------------------------ */
/* The device we present: a CDC ACM serial port (vid:pid 18d1:d00d, "xnuloader") */

#define EP_NOTIFY 1u   /* interrupt IN,  ep 0x81 */
#define EP_DATA   2u   /* bulk IN/OUT,   ep 0x82 / 0x02 */

static const uint8_t desc_device[18] = {
	18, 1, 0x00, 0x02, 0x02, 0x00, 0x00, 64,
	0xd1, 0x18, 0x0d, 0xd0, 0x00, 0x01,
	1, 2, 3, 1
};

/*
 * One configuration, two interfaces (CDC communication + CDC data). Offsets matter: the interface and
 * endpoint descriptors handed to the firmware point into this array.
 */
static const uint8_t desc_config[67] = {
	/*  0: configuration */            9, 2, 67, 0, 2, 1, 0, 0x80, 50,
	/*  9: interface 0, CDC control */ 9, 4, 0, 0, 1, 0x02, 0x02, 0x01, 0,
	/* 18: header functional */        5, 0x24, 0x00, 0x10, 0x01,
	/* 23: call management */          5, 0x24, 0x01, 0x00, 1,
	/* 28: ACM */                      4, 0x24, 0x02, 0x02,
	/* 32: union */                    5, 0x24, 0x06, 0, 1,
	/* 37: notification, ep 0x81 */    7, 5, 0x81, 0x03, 0x10, 0x00, 0x10,
	/* 44: interface 1, CDC data */    9, 4, 1, 0, 2, 0x0a, 0x00, 0x00, 0,
	/* 53: bulk out, ep 0x02 */        7, 5, 0x02, 0x02, 0x00, 0x02, 0x00,
	/* 60: bulk in, ep 0x82 */         7, 5, 0x82, 0x02, 0x00, 0x02, 0x00
};

#define CFG_OFF_IF0       9
#define CFG_OFF_EP_NOTIFY 37
#define CFG_OFF_IF1       44
#define CFG_OFF_EP_OUT    53
#define CFG_OFF_EP_IN     60

static const uint8_t desc_string0[4]  = { 4, 3, 0x09, 0x04 };
static const uint8_t desc_string1[14] = { 14, 3, 'H',0, 'a',0, 'k',0, 'i',0, 'O',0, 'S',0 };
static const uint8_t desc_string2[34] = { 34, 3,
	'u',0, 'n',0, 'i',0, 'c',0, 'o',0, 'r',0, 'n',0, ' ',0, 'l',0, 'o',0, 'a',0, 'd',0, 'e',0, 'r',0 };
static const uint8_t desc_string3[20] = { 20, 3,
	'x',0, 'n',0, 'u',0, 'l',0, 'o',0, 'a',0, 'd',0, 'e',0, 'r',0 };

static usbfn_endpoint_desc *eps_if0[2];
static usbfn_endpoint_desc *eps_if1[3];
static usbfn_interface_info iface0 = { (usbfn_interface_desc *)(desc_config + CFG_OFF_IF0), eps_if0 };
static usbfn_interface_info iface1 = { (usbfn_interface_desc *)(desc_config + CFG_OFF_IF1), eps_if1 };
static usbfn_interface_info *ifaces[3];
static usbfn_config_info config0 = { (usbfn_config_desc *)desc_config, ifaces };
static usbfn_config_info *configs[2];

static usbfn_io *usb;
static int usb_started;      /* controller running */
static int usb_configured;   /* host chose our configuration */
static int host_open;        /* CDC DTR: the host has the port open */
static int tx_busy;
static void *rx_buffer, *tx_buffer;
static uint8_t tx_ring[1024];
static unsigned tx_ring_len;
static uint8_t line_coding[7] = { 0x00, 0xc2, 0x01, 0x00, 0x00, 0x00, 0x08 }; /* 115200 8N1 */

/* declared before use: usbfn_put_char() flushes, usb_control_send() pumps */
void usbfn_flush(void);
static void usb_pump(unsigned rounds);

/* ------------------------------------------------------------------------ */

static void
usb_pump(unsigned rounds)
{
	unsigned i;

	for (i = 0; i < rounds; i++) {
		uint32_t msg = USBFN_MSG_NONE;
		uint64_t size = sizeof(usbfn_payload);
		usbfn_payload payload;
		efi_status s;

		payload.utr.bytes = 0;
		payload.utr.status = 0;
		payload.utr.ep_index = 0;
		payload.utr.direction = 0;
		payload.utr.buffer = 0;

		s = usb->handle_event(usb, &msg, &size, &payload);
		if (EFI_ERROR(s)) {
			return;
		}
		if (msg == USBFN_MSG_NONE) {
			return;   /* nothing queued: the firmware drains its own event ring here */
		}

		switch (msg) {
		case USBFN_MSG_TX:
			if (payload.utr.ep_index == EP_DATA) {
				tx_busy = 0;
			}
			break;
		case USBFN_MSG_RX:
			if (payload.utr.ep_index == EP_DATA) {
				/* the host may send anything; a serial console needs none of it */
				if (usb_configured) {
					uint64_t len = 512;
					usb->transfer(usb, EP_DATA, USBFN_DIR_RX, &len, rx_buffer);
				}
			}
			break;
		case USBFN_MSG_RESET:
		case USBFN_MSG_DETACH:
			usb_configured = 0;
			host_open = 0;
			tx_busy = 0;
			break;
		default:
			break;
		}
	}
}

static void
usb_control_send(const void *data, unsigned len)
{
	uint64_t size = len;

	if (usb->transfer(usb, 0, USBFN_DIR_TX, &size, (void *)data) == 0) {
		usb_pump(64);
	}
}

static void
usb_control_recv(void *data, unsigned len)
{
	uint64_t size = len;

	if (usb->transfer(usb, 0, USBFN_DIR_RX, &size, data) == 0) {
		usb_pump(64);
	}
}

static void
usb_get_descriptor(const usbfn_setup *req)
{
	const uint8_t *d = 0;
	unsigned len = 0;
	unsigned want = req->length;

	switch (req->value >> 8) {
	case 1: d = desc_device; len = sizeof(desc_device); break;
	case 2: d = desc_config; len = sizeof(desc_config); break;
	case 3:
		switch (req->value & 0xff) {
		case 0: d = desc_string0; len = sizeof(desc_string0); break;
		case 1: d = desc_string1; len = sizeof(desc_string1); break;
		case 2: d = desc_string2; len = sizeof(desc_string2); break;
		case 3: d = desc_string3; len = sizeof(desc_string3); break;
		default: break;
		}
		break;
	default:
		break;
	}

	if (d == 0) {
		/* e.g. the device_qualifier of a high speed only device: answer with a stall */
		usb->set_endpoint_stall_state(usb, 0, USBFN_DIR_TX, 1);
		return;
	}
	if (want != 0 && want < len) {
		len = want;
	}
	usb_control_send(d, len);
}

static void
usb_handle_setup(const usbfn_setup *req)
{
	uint8_t type = (uint8_t)(req->request_type & 0x60);
	uint8_t in = (uint8_t)(req->request_type & 0x80);

	if (type == 0x00 && in) {
		switch (req->request) {
		case 0x06: usb_get_descriptor(req); break;                     /* GET_DESCRIPTOR */
		case 0x08: { uint8_t one = 1; usb_control_send(&one, 1); } break; /* GET_CONFIGURATION */
		case 0x00: { uint8_t zero[2] = { 0, 0 }; usb_control_send(zero, 2); } break; /* GET_STATUS */
		case 0x0a: { uint8_t zero = 0; usb_control_send(&zero, 1); } break;          /* GET_INTERFACE */
		default: break;
		}
		return;
	}

	if (type == 0x00 && !in) {
		switch (req->request) {
		case 0x09: {                                                   /* SET_CONFIGURATION */
			usb_configured = 1;
			uint64_t len = 512;
			usb->transfer(usb, EP_DATA, USBFN_DIR_RX, &len, rx_buffer);
			break;
		}
		case 0x01:                                                     /* CLEAR_FEATURE */
		case 0x03:                                                     /* SET_FEATURE */
		case 0x0b:                                                     /* SET_INTERFACE */
		default:
			break;
		}
		return;
	}

	if (type == 0x20) {   /* CDC class, interface requests */
		switch (req->request) {
		case 0x20: usb_control_recv(line_coding, sizeof(line_coding)); break; /* SET_LINE_CODING */
		case 0x21: usb_control_send(line_coding, sizeof(line_coding)); break; /* GET_LINE_CODING */
		case 0x22: host_open = (req->value & 1) != 0; break;                  /* SET_CONTROL_LINE_STATE */
		default: break;
		}
	}
}

/* ------------------------------------------------------------------------ */
/* Public interface used by loader.c                                        */

int
usbfn_serial_start(void)
{
	static const efi_guid guid = USBFN_IO_GUID;
	usbfn_device_info info;
	efi_status s;

	if (usb_started) {
		return 1;
	}

	if (EFI_ERROR(BS->locate_protocol(&guid, 0, (void **)&usb)) || usb == 0) {
		return 0;
	}

	/* the descriptor tables the firmware needs to bind our endpoints */
	eps_if0[0] = (usbfn_endpoint_desc *)(desc_config + CFG_OFF_EP_NOTIFY);
	eps_if0[1] = 0;
	eps_if1[0] = (usbfn_endpoint_desc *)(desc_config + CFG_OFF_EP_OUT);
	eps_if1[1] = (usbfn_endpoint_desc *)(desc_config + CFG_OFF_EP_IN);
	eps_if1[2] = 0;
	ifaces[0] = &iface0;
	ifaces[1] = &iface1;
	ifaces[2] = 0;
	configs[0] = &config0;
	configs[1] = 0;
	info.device_desc = (usbfn_device_desc *)desc_device;
	info.configs = configs;

	s = usb->start_controller(usb);
	if (EFI_ERROR(s)) {
		return 0;
	}

	s = usb->configure_enable_endpoints(usb, &info);
	if (EFI_ERROR(s)) {
		return 0;
	}

	if (EFI_ERROR(usb->allocate_transfer_buffer(usb, 512, &rx_buffer)) ||
	    EFI_ERROR(usb->allocate_transfer_buffer(usb, 512, &tx_buffer))) {
		return 0;
	}

	usb_started = 1;
	tx_ring_len = 0;
	tx_busy = 0;
	return 1;
}

void
usbfn_put_char(char c)
{
	if (!usb_started || !usb_configured || !host_open) {
		return;   /* nobody is reading: the loader keeps running, the ring stays empty */
	}

	if (tx_ring_len < sizeof(tx_ring)) {
		tx_ring[tx_ring_len++] = (uint8_t)c;
	}
	if (c != '\n' && tx_ring_len < sizeof(tx_ring)) {
		return;
	}

	usbfn_flush();
}

void
usbfn_flush(void)
{
	unsigned i;
	unsigned n = tx_ring_len < 512 ? tx_ring_len : 512;
	uint64_t size;

	if (!tx_busy) {
		uint8_t *dst = (uint8_t *)tx_buffer;

		for (i = 0; i < n; i++) {
			dst[i] = tx_ring[i];
		}
		for (i = n; i < tx_ring_len; i++) {
			tx_ring[i - n] = tx_ring[i];
		}
		tx_ring_len -= n;

		size = n;
		tx_busy = 1;
		if (EFI_ERROR(usb->transfer(usb, EP_DATA, USBFN_DIR_TX, &size, tx_buffer))) {
			tx_busy = 0;
			return;
		}
	}

	/* bounded wait: a host that stopped reading must not stall the loader */
	for (i = 0; i < 40 && tx_busy; i++) {
		usb_pump(64);
		if (tx_busy) {
			BS->stall(1000);
		}
	}
}

void
usbfn_write(const char *s, unsigned len)
{
	while (len--) {
		usbfn_put_char(*s++);
	}
}

void
usbfn_poll(void)
{
	if (!usb_started && tx_ring_len == 0) {
		return;
	}
	if (usb_started) {
		usb_pump(64);
	}
	if (tx_ring_len != 0 && usb_started && usb_configured) {
		usbfn_flush();
	}
}
