/*
 * Copyright (c) 2019, Emil Renner Berthing
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its contributors
 *    may be used to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
 * OF SUCH DAMAGE.
 */
#include <stdint.h>
#include <stddef.h>

#include "gd32vf103/rcu.h"
#include "gd32vf103/dbg.h"

#include "lib/mtimer.h"
#include "lib/gpio.h"
#include "lib/eclic.h"

#include "usbfs.h"
#include "dfu.h"

#ifdef NDEBUG
#define debug(...)
#else
#include <stdio.h>
#define debug(...) printf(__VA_ARGS__)
#endif

#define USBFS_FIFO_RXSIZE  512
#define USBFS_FIFO_TX0SIZE 128
#define USBFS_FIFO_TX1SIZE 0
#define USBFS_FIFO_TX2SIZE 0
#define USBFS_FIFO_TX3SIZE 0

static const struct usb_descriptor_device usbfs_descriptor_device = {
	.bLength            = 18,
	.bDescriptorType    = 0x01, /* Device */
	.bcdUSB             = 0x0200,
	.bDeviceClass       = 0x00, /* 0x00 = per interface */
	.bDeviceSubClass    = 0x00,
	.bDeviceProtocol    = 0x00,
	.bMaxPacketSize0    = 64,
	.idVendor           = 0x1d50, /* OpenMoko vendor id */
	.idProduct          = 0x613e, /* GeckoBoot product id */
	.bcdDevice          = 0x0200,
	.iManufacturer      = 1,
	.iProduct           = 2,
	.iSerialNumber      = 3,
	.bNumConfigurations = 1,
};

static const struct usb_descriptor_configuration usbfs_descriptor_configuration1 = {
	.bLength              = 9,
	.bDescriptorType      = 0x02, /* Configuration */
	.wTotalLength         = 9 + 9 + 9,
	.bNumInterfaces       = 1,
	.bConfigurationValue  = 1,
	.iConfiguration       = 0,
	.bmAttributes         = 0x80,
	.bMaxPower            = 250,
	.rest = {
	/* Interface */
	/* .bLength            */ 9,
	/* .bDescriptorType    */ 0x04, /* Interface */
	/* .bInterfaceNumber   */ DFU_INTERFACE,
	/* .bAlternateSetting  */ 0,
	/* .bNumEndpoints      */ 0,    /* only the control pipe is used */
	/* .bInterfaceClass    */ 0xFE, /* application specific */
	/* .bInterfaceSubClass */ 0x01, /* device firmware upgrade */
	/* .bInterfaceProtocol */ 0x02, /* DFU mode protocol */
	/* .iInterface         */ 4,
	/* DFU Interface */
	/* .bLength            */ 9,
	/* .bDescriptorType    */ 0x21, /* DFU Interface */
	/* .bmAttributes       */ 0x0f, /* download, upload, detach and manifest tolerant */
	/* .wDetachTimeOut     */ USB_WORD(500), /* 500ms */
	/* .wTransferSize      */ USB_WORD(DFU_TRANSFERSIZE),
	/* .bcdDFUVersion      */ USB_WORD(0x0101), /* DFU v1.1 */
	}
};

static const struct usb_descriptor_string usbfs_descriptor_string0 = {
	.bLength         = 4,
	.bDescriptorType = 0x03, /* String */
	.wCodepoint = {
		0x0409, /* English (US) */
	},
};

static const struct usb_descriptor_string usbfs_descriptor_manufacturer = {
	.bLength         = 16,
	.bDescriptorType = 0x03, /* String */
	.wCodepoint = {
		'L','a','b','i','t','a','t',
	},
};

static const struct usb_descriptor_string usbfs_descriptor_product = {
	.bLength         = 20,
	.bDescriptorType = 0x03, /* String */
	.wCodepoint = {
		'G','D','3','2','V','F','1','0','3',
	},
};

/* must be at least 12 characters long and consist of only '0'-'9','A'-'B'
 * at least according to the mass-storage bulk-only document */
static const struct usb_descriptor_string usbfs_descriptor_serial = {
	.bLength         = 26,
	.bDescriptorType = 0x03, /* String */
	.wCodepoint = {
		'0','0','0','0','0','0','0','0','0','0','0','1',
	},
};

static const struct usb_descriptor_string usbfs_descriptor_dfu = {
	.bLength         = 20,
	.bDescriptorType = 0x03, /* String */
	.wCodepoint = {
		'G','e','c','k','o','B','o','o','t',
	},
};

static const struct usb_descriptor_string *const usbfs_descriptor_string[] = {
	&usbfs_descriptor_string0,
	&usbfs_descriptor_manufacturer,
	&usbfs_descriptor_product,
	&usbfs_descriptor_serial,
	&usbfs_descriptor_dfu,
};

static struct {
	uint32_t *ep0out;
	const unsigned char *ep0in;
	uint32_t bytes;
} usbfs_state;

static struct {
	union {
		struct usb_setup_packet setup;
		uint32_t v[2];
	};
	uint8_t data[DFU_TRANSFERSIZE];
} usbfs_outbuf;

static uint16_t usbfs_status;

bool usbfs_reboot_on_ack;

static void
dumpsetup(const struct usb_setup_packet *p)
{
	debug("{\n"
	      "  bmRequestType 0x%02x\n"
	      "  bRequest      0x%02x\n"
	      "  wValue        0x%04x\n"
	      "  wIndex        0x%04x\n"
	      "  wLength       0x%04x\n"
	      "}\n",
		p->bmRequestType,
		p->bRequest,
		p->wValue,
		p->wIndex,
		p->wLength);
}

static void
usbfs_ep0in_transfer(void)
{
	uint32_t len = usbfs_state.bytes;
	const unsigned char *p = usbfs_state.ep0in;
	const unsigned char *end;

	if (len > 64)
		len = 64;

	end = p + len;

	USBFS->DIEP[0].LEN = USBFS_DIEPLEN_PCNT(1U) | len;
	USBFS->DIEP[0].CTL |= USBFS_DIEPCTL_EPEN | USBFS_DIEPCTL_CNAK;
	while (p < end) {
		uint32_t v = *p++;
		if (p < end)
			v |= (*p++) << 8;
		if (p < end)
			v |= (*p++) << 16;
		if (p < end)
			v |= (*p++) << 24;
		USBFS->DFIFO[0][0] = v;
	}
}

#if 0
static void
usbfs_ep0in_transfer_empty(void)
{
	USBFS->DIEP[0].LEN = USBFS_DIEPLEN_PCNT(1U);
	USBFS->DIEP[0].CTL |= USBFS_DIEPCTL_EPEN | USBFS_DIEPCTL_CNAK;
}
#else
static inline void
usbfs_ep0in_transfer_empty(void) { usbfs_ep0in_transfer(); }
#endif

static inline void
usbfs_ep0in_stall(void)
{
	USBFS->DIEP[0].CTL |= USBFS_DIEPCTL_STALL;
}

static void
usbfs_ep0out_prepare_setup(void)
{
	USBFS->DOEP[0].LEN =
		USBFS_DOEPLEN_STPCNT(3U) |
		USBFS_DOEPLEN_PCNT(0U) |
		USBFS_DOEPLEN_TLEN(0U);
	USBFS->DOEP[0].CTL |= USBFS_DOEPCTL_EPEN | USBFS_DOEPCTL_STALL;
}

static void
usbfs_ep0out_prepare_out(void)
{
	USBFS->DOEP[0].LEN =
		USBFS_DOEPLEN_STPCNT(3U) |
		USBFS_DOEPLEN_PCNT(1U) |
		USBFS_DOEPLEN_TLEN(64);
	USBFS->DOEP[0].CTL |= USBFS_DOEPCTL_EPEN | USBFS_DOEPCTL_CNAK;
}

static void
usbfs_suspend(void)
{
	/*
	usbfs_phy_stop();
	USBFS->GINTEN =
		USBFS_GINTEN_WUIM |
		USBFS_GINTEN_USBRST;
	*/
}

static void
usbfs_wakeup(void)
{
	/*
	usbfs_phy_start();
	USBFS->GINTEN =
		USBFS_GINTEN_ENUMDNEM |
		USBFS_GINTEN_USBRST |
		USBFS_GINTEN_USBSUSPM |
		USBFS_GINTEN_OEPINT |
		USBFS_GINTEN_IEPINT;
	*/
}

static void
usbfs_txfifos_flush(void)
{
	/* flush all tx fifos */
	USBFS->GRSTCTL |= USBFS_GRSTCTL_TXFNUM(0x10U) | USBFS_GRSTCTL_TXFF;
	while ((USBFS->GRSTCTL & USBFS_GRSTCTL_TXFF))
		/* wait */;
	/* wait 3 more phy clocks */
	mtimer_udelay(3);
}

static void
usbfs_ep_reset(void)
{
	unsigned int i;

	USBFS->DIEP[0].CTL =
		USBFS_DIEPCTL_STALL |
		USBFS_DIEPCTL_EPTYPE_CONTROL |
		USBFS_DIEP0CTL_MPL_64B;
	USBFS->DIEP[0].INTF =
		USBFS_DIEPINTF_IEPNE |
		USBFS_DIEPINTF_EPTXFUD |
		USBFS_DIEPINTF_CITO |
		USBFS_DIEPINTF_EPDIS |
		USBFS_DIEPINTF_TF;
	for (i = 1; i < 4; i++) {
		/*
		if (USBFS->DIEP[i].CTL & USBFS_DIEPCTL_EPEN)
			USBFS->DIEP[i].CTL = USBFS_DIEPCTL_EPD | USBFS_DIEPCTL_SNAK;
		else
		*/
			USBFS->DIEP[i].CTL = USBFS_DIEPCTL_SNAK;
		USBFS->DIEP[i].INTF =
			USBFS_DIEPINTF_IEPNE |
			USBFS_DIEPINTF_EPTXFUD |
			USBFS_DIEPINTF_CITO |
			USBFS_DIEPINTF_EPDIS |
			USBFS_DIEPINTF_TF;
		USBFS->DIEP[i].LEN = 0;
	}

	USBFS->DOEP[0].CTL =
		USBFS_DOEPCTL_STALL |
		USBFS_DOEPCTL_EPTYPE_CONTROL |
		USBFS_DOEP0CTL_MPL_64B;
	USBFS->DOEP[0].INTF =
		USBFS_DOEPINTF_BTBSTP |
		USBFS_DOEPINTF_EPRXFOVR |
		USBFS_DOEPINTF_STPF |
		USBFS_DOEPINTF_EPDIS |
		USBFS_DOEPINTF_TF;
	for (i = 1; i < 4; i++) {
		/*
		if (USBFS->DOEP[i].CTL & USBFS_DOEPCTL_EPEN)
			USBFS->DOEP[i].CTL = USBFS_DOEPCTL_EPD | USBFS_DOEPCTL_SNAK;
		else
		*/
			USBFS->DOEP[i].CTL = USBFS_DOEPCTL_SNAK;
		USBFS->DOEP[i].INTF =
			USBFS_DOEPINTF_BTBSTP |
			USBFS_DOEPINTF_EPRXFOVR |
			USBFS_DOEPINTF_STPF |
			USBFS_DOEPINTF_EPDIS |
			USBFS_DOEPINTF_TF;
		USBFS->DOEP[i].LEN = 0;
	}
}

static void
usbfs_reset(void)
{
	/* flush all tx fifos */
	usbfs_txfifos_flush();

	/* reset endpoint registers */
	usbfs_ep_reset();

	/* reset address */
	USBFS->DCFG &= ~USBFS_DCFG_DAR_Msk;

	/* enable interrupts for endpoint 0 only */
	USBFS->DAEPINTEN =
		USBFS_DAEPINTEN_OEPIE(1U) |
		USBFS_DAEPINTEN_IEPIE(1U);
	USBFS->DOEPINTEN =
		USBFS_DOEPINTEN_STPFEN |
		/* USBFS_DOEPINTEN_EPDISEN | */
		USBFS_DOEPINTEN_TFEN;
	USBFS->DIEPINTEN = 
		/* USBFS_DIEPINTEN_CITOEN | */
		/* USBFS_DIEPINTEN_EPDISEN | */
		USBFS_DIEPINTEN_TFEN;

	/* reset internal state */
	usbfs_state.bytes = 0;
}

static void
usbfs_enumdone(void)
{
	USBFS->DCTL |= USBFS_DCTL_CGINAK;

	if ((USBFS->DSTAT & USBFS_DSTAT_ES_Msk) == USBFS_DSTAT_ES_FULL) {
		/* prepare to receive setup package */
		usbfs_ep0out_prepare_setup();

		debug("full speed.. ");
	} else {
		debug("low speed.. ");
	}
}

static int
usbfs_handle_get_status_device(const struct usb_setup_packet *p, const void **data)
{
	debug("GET_STATUS: device\n");
	*data = &usbfs_status;
	return 2;
}

static int
usbfs_handle_set_address(const struct usb_setup_packet *p, const void **data)
{
	debug("SET_ADDRESS: wValue = %hu\n", p->wValue);
	USBFS->DCFG = (USBFS->DCFG & ~USBFS_DCFG_DAR_Msk) |
		USBFS_DCFG_DAR((uint32_t)p->wValue);
	return 0;
}

static int
usbfs_handle_get_descriptor_device(const void **data, uint8_t index)
{
	if (index != 0) {
		debug("GET_DESCRIPTOR: type = 0x01, but index = 0x%02x\n", index);
		return -1;
	}
	*data = &usbfs_descriptor_device;
	return sizeof(usbfs_descriptor_device);
}

static int
usbfs_handle_get_descriptor_configuration(const void **data, uint8_t index)
{
	if (index != 0) {
		debug("GET_DESCRIPTOR: unknown configuration %hu\n", index);
		return -1;
	}
	*data = &usbfs_descriptor_configuration1;
	return usbfs_descriptor_configuration1.wTotalLength;
}

static int
usbfs_handle_get_descriptor_string(const void **data, uint8_t index)
{
	const struct usb_descriptor_string *desc;

	if (index >= ARRAY_SIZE(usbfs_descriptor_string)) {
		debug("GET_DESCRIPTOR: unknown string %hu\n", index);
		return -1;
	}
	desc = usbfs_descriptor_string[index];
	*data = desc;
	return desc->bLength;
}

static int
usbfs_handle_get_descriptor(const struct usb_setup_packet *p, const void **data)
{
	uint8_t type = p->wValue >> 8;
	uint8_t index = p->wValue & 0xFFU;

	switch (type) {
	case 0x01:
		debug("GET_DESCRIPTOR: device, %u bytes\n", p->wLength);
		return usbfs_handle_get_descriptor_device(data, index);
	case 0x02:
		debug("GET_DESCRIPTOR: configuration %u, %u bytes\n",
				index, p->wLength);
		return usbfs_handle_get_descriptor_configuration(data, index);
	case 0x03:
		debug("GET_DESCRIPTOR: string %u, %u bytes\n",
				index, p->wLength);
		return usbfs_handle_get_descriptor_string(data, index);
#ifndef NDEBUG
	case 0x06: /* DEVICE QUALIFIER (for high-speed) */
		debug("DEVICE_QUALIFIER\n");
		break;
	default:
		debug("GET_DESCRIPTOR: unknown type 0x%02x\n", type);
		dumpsetup(p);
		break;
#endif
	}
	return -1;
}

static int
usbfs_handle_get_configuration(const struct usb_setup_packet *p, const void **data)
{
	debug("GET_CONFIGURATION\n");
	*data = &usbfs_descriptor_configuration1.bConfigurationValue;
	return 1;
}

static int
usbfs_handle_set_configuration(const struct usb_setup_packet *p, const void **data)
{
	debug("SET_CONFIGURATION: wValue = %hu\n", p->wValue);

	if (p->wValue != usbfs_descriptor_configuration1.bConfigurationValue)
		return -1;

	return 0;
}

static int
usbfs_handle_set_interface0(const struct usb_setup_packet *p, const void **data)
{
	debug("SET_INTERFACE: wIndex = %hu, wValue = %hu\n", p->wIndex, p->wValue);

	if (p->wValue != 0)
		return -1;

	return 0;
}

static int
usbfs_handle_clear_feature_endpoint(const struct usb_setup_packet *p, const void **data)
{
	debug("CLEAR_FEATURE endpoint %hu\n", p->wIndex);
	return -1;
}

static const struct usb_setup_handler usbfs_setup_handlers[] = {
	{ .req = 0x0080, .idx =  0, .len = -1, .fn = usbfs_handle_get_status_device },
	{ .req = 0x0500, .idx =  0, .len =  0, .fn = usbfs_handle_set_address },
	{ .req = 0x0680, .idx = -1, .len = -1, .fn = usbfs_handle_get_descriptor },
	{ .req = 0x0880, .idx =  0, .len = -1, .fn = usbfs_handle_get_configuration },
	{ .req = 0x0900, .idx =  0, .len =  0, .fn = usbfs_handle_set_configuration },
	{ .req = 0x0102, .idx =  0, .len =  0, .fn = usbfs_handle_clear_feature_endpoint },
	{ .req = 0x0b01, .idx = DFU_INTERFACE, .len =  0, .fn = usbfs_handle_set_interface0 },
	{ .req = 0x0021, .idx = DFU_INTERFACE, .len =  0, .fn = dfu_detach },
	{ .req = 0x0121, .idx = DFU_INTERFACE, .len = -1, .fn = dfu_dnload },
	{ .req = 0x02a1, .idx = DFU_INTERFACE, .len = -1, .fn = dfu_upload },
	{ .req = 0x03a1, .idx = DFU_INTERFACE, .len = -1, .fn = dfu_getstatus },
	{ .req = 0x0421, .idx = DFU_INTERFACE, .len =  0, .fn = dfu_clrstatus },
	{ .req = 0x05a1, .idx = DFU_INTERFACE, .len = -1, .fn = dfu_getstate },
	{ .req = 0x0621, .idx = DFU_INTERFACE, .len =  0, .fn = dfu_abort },
};

static int
usbfs_setup_handler_run(const struct usb_setup_packet *p, const void **data)
{
	uint8_t idx = p->wIndex;
	const struct usb_setup_handler *h;

	for (h = usbfs_setup_handlers; h < ARRAY_END(usbfs_setup_handlers); h++) {
		if (h->req == p->request && (h->idx == 0xFFU || h->idx == idx)) {
			if (h->len != 0xFFU && h->len != p->wLength)
				break;
			return h->fn(p, data);
		}
	}

	debug("unknown request 0x%04x\n", p->request);
	dumpsetup(p);
	return -1;
}

static void
usbfs_handle_setup(void)
{
	const struct usb_setup_packet *p = &usbfs_outbuf.setup;

	usbfs_state.bytes = 0;

	if (p->bmRequestType & 0x80U) {
		const void *data;
		int ret = usbfs_setup_handler_run(p, &data);

		if (ret >= 0) {
			/* send IN data */
			if (ret > p->wLength)
				ret = p->wLength;
			usbfs_state.ep0in = data;
			usbfs_state.bytes = ret;
			usbfs_ep0in_transfer();
			/* prepare for IN ack */
			usbfs_ep0out_prepare_out();
			return;
		}
	} else if (p->wLength == 0) {
		const void *data;

		if (!usbfs_setup_handler_run(p, &data)) {
			/* send empty ack package */
			usbfs_ep0in_transfer_empty();
			/* prepare for next SETUP package */
			usbfs_ep0out_prepare_setup();
			return;
		}
	} else if (p->wLength <= ARRAY_SIZE(usbfs_outbuf.data)) {
		/* receive OUT data */
		usbfs_ep0out_prepare_out();
		usbfs_state.bytes = p->wLength;
		return;
	}

	/* stall IN endpoint */
	usbfs_ep0in_stall();
	/* prepare for next SETUP package */
	usbfs_ep0out_prepare_setup();
}

static void
usbfs_handle_ep0(void)
{
	uint32_t oflags = USBFS->DOEP[0].INTF;
	uint32_t iflags = USBFS->DIEP[0].INTF;
	uint32_t bytes;

	USBFS->DOEP[0].INTF = oflags;
	USBFS->DIEP[0].INTF = iflags;

	//debug("EP0 %04lx %04lx %lu\n", oflags, iflags, usbfs_state.bytes);

	if (oflags & USBFS_DOEPINTF_STPF) {
		usbfs_handle_setup();
		return;
	}

	bytes = usbfs_state.bytes;
	if (bytes == 0) {
		if (usbfs_reboot_on_ack) {
			/* do a software reset */
			DBG->KEY = DBG_KEY_UNLOCK;
			DBG->CMD = DBG_CMD_RESET;
			__builtin_unreachable();
		}
		return;
	}

	if (iflags & USBFS_DIEPINTF_TF) {
		/* data IN */
		if (bytes > 64) {
			/* send next package */
			usbfs_state.ep0in += 64;
			usbfs_state.bytes = bytes - 64;
			usbfs_ep0in_transfer();
		} else
			usbfs_state.bytes = 0;
	} else if (oflags & USBFS_DOEPINTF_TF) {
		/* data OUT */
		bytes = 64 - (USBFS->DOEP[0].LEN & USBFS_DOEPLEN_TLEN_Msk);
		if (usbfs_state.bytes > bytes) {
			usbfs_state.bytes -= bytes;
			/* prepare for more OUT data */
			usbfs_ep0out_prepare_out();
		} else {
			const void *data = usbfs_outbuf.data;

			usbfs_state.bytes = 0;
			if (!usbfs_setup_handler_run(&usbfs_outbuf.setup, &data)) {
				/* send empty ack package */
				usbfs_ep0in_transfer_empty();
			} else
				usbfs_ep0in_stall();
			usbfs_ep0out_prepare_setup();
		}
	}
}

static void
usbfs_handle_endpoints(void)
{
	uint32_t flags = USBFS->DAEPINT;

	if (flags & 0x10001U)
		usbfs_handle_ep0();
}

static void
usbfs_handle_rxdata(void)
{
	uint32_t grstat = USBFS->GRSTATP;
	unsigned int ep = grstat & USBFS_GRSTAT_EPNUM_Msk;
	unsigned int len;

	if (ep != 0) {
		debug("RXDATA: received data for endpoint %u\n", ep);
		return;
	}

	len = (grstat & USBFS_GRSTAT_BCOUNT_Msk) >> USBFS_GRSTAT_BCOUNT_Pos;
	if (len == 0)
		return;

	if ((grstat & USBFS_GRSTAT_RPCKST_Msk) == USBFS_GRSTAT_RPCKST_STP) {
		for (; len > 8; len -= 4)
			(void)USBFS->DFIFO[0][0];
		usbfs_state.ep0out = usbfs_outbuf.v;
		*usbfs_state.ep0out++ = USBFS->DFIFO[0][0];
		*usbfs_state.ep0out++ = USBFS->DFIFO[0][0];
	} else {
		while (1) {
			*usbfs_state.ep0out++ = USBFS->DFIFO[0][0];
			if (len <= 4)
				break;
			len -= 4;
		}
	}
}

void
USBFS_IRQHandler(void)
{
	uint32_t flags = USBFS->GINTF;

	//debug("flags = %08lx\n", flags);

	/* read all incoming packets */
	while ((flags & USBFS_GINTF_RXFNEIF)) {
		usbfs_handle_rxdata();
		flags = USBFS->GINTF;
	}

	if (flags & (USBFS_GINTF_OEPIF | USBFS_GINTF_IEPIF))
		usbfs_handle_endpoints();

	/*
	if (!(flags & (
			USBFS_GINTF_SP |
			USBFS_GINTF_WKUPIF |
			USBFS_GINTF_RST |
			USBFS_GINTF_ENUMF)))
		return;
	*/

	if (flags & USBFS_GINTF_SP) {
		debug("SUSPEND.. ");
		usbfs_suspend();
		USBFS->GINTF = USBFS_GINTF_SP;
		debug("done\n");
		return;
	}
	if (flags & USBFS_GINTF_WKUPIF) {
		debug("WAKEUP.. ");
		usbfs_wakeup();
		USBFS->GINTF = USBFS_GINTF_WKUPIF;
		debug("done\n");
	}
	if (flags & USBFS_GINTF_RST) {
		debug("RESET.. ");
		usbfs_reset();
		USBFS->GINTF = USBFS_GINTF_RST;
		debug("done\n");
	}
	if (flags & USBFS_GINTF_ENUMF) {
		debug("ENUMDONE.. ");
		usbfs_enumdone();
		USBFS->GINTF = USBFS_GINTF_ENUMF;
		debug("done\n");
	}
}

static void
usbfs_allocate_buffers(uint32_t rx,
		uint32_t tx0, uint32_t tx1, uint32_t tx2, uint32_t tx3)
{
	/* round up to number of 32bit words */
	rx  = (rx  + 3) >> 2;
	tx0 = (tx0 + 3) >> 2;
	tx1 = (tx1 + 3) >> 2;
	tx2 = (tx2 + 3) >> 2;
	tx3 = (tx3 + 3) >> 2;
	USBFS->GRFLEN = rx;
	USBFS->DIEP0TFLEN = (tx0 << 16) | rx;
	USBFS->DIEP1TFLEN = (tx1 << 16) | (rx + tx0);
	USBFS->DIEP2TFLEN = (tx2 << 16) | (rx + tx0 + tx1);
	USBFS->DIEP3TFLEN = (tx3 << 16) | (rx + tx0 + tx1 + tx2);
}

void
usbfs_init(void)
{
	/* turn on USBFS clock */
	RCU->AHBEN |= RCU_AHBEN_USBFSEN;

	/* turn on GPIOA and AFIO */
	RCU->APB2EN |= RCU_APB2EN_PAEN | RCU_APB2EN_AFEN;

	/* reset USBFS */
	RCU->AHBRST |= RCU_AHBRST_USBFSRST;
	RCU->AHBRST &= ~RCU_AHBRST_USBFSRST;

	/* disable global interrupt flag */
	USBFS->GAHBCS = 0U;

	/* disable Vbus sensing */
	USBFS->GCCFG = USBFS_GCCFG_VBUSIG;

	debug("core reset");
	USBFS->GRSTCTL = USBFS_GRSTCTL_CSRST;
	while ((USBFS->GRSTCTL & USBFS_GRSTCTL_CSRST))
		debug(".");
	debug(" done\n");
	mtimer_udelay(3);

	/* force device mode */
	debug("switching to device mode");
	USBFS->GUSBCS |= USBFS_GUSBCS_FDM;
	while ((USBFS->GINTF & USBFS_GINTF_COPM))
		debug(".");
	debug(" done\n");

	/* manual says: "the application must wait at
	 * least 25ms for [FDM to] take effect" */
	mtimer_udelay(25000);

	/* initialize device */
	USBFS->DCFG =
		USBFS_DCFG_EOPFT_80PCT |
		USBFS_DCFG_DS_FULL;

	/* disconnect */
	USBFS->DCTL = USBFS_DCTL_SD;

	/* now that we're disconnected, power on phy */
	USBFS->GCCFG =
		USBFS_GCCFG_VBUSIG |
		/* USBFS_GCCFG_VBUSACEN | */
		USBFS_GCCFG_VBUSBCEN |
		USBFS_GCCFG_PWRON;

	/* setup fifo allocation */
	usbfs_allocate_buffers(USBFS_FIFO_RXSIZE,
			USBFS_FIFO_TX0SIZE,
			USBFS_FIFO_TX1SIZE,
			USBFS_FIFO_TX2SIZE,
			USBFS_FIFO_TX3SIZE);

	/* flush all tx fifos */
	usbfs_txfifos_flush();

	/* flush rx fifo */
	USBFS->GRSTCTL |= USBFS_GRSTCTL_RXFF;
	while ((USBFS->GRSTCTL & USBFS_GRSTCTL_RXFF))
		/* wait */;
	/* wait 3 more phy clocks */
	mtimer_udelay(3);

	USBFS->DIEPINTEN = 0U;
	USBFS->DOEPINTEN = 0U;
	USBFS->DAEPINTEN = 0U;

	/* reset endpoint registers */
	usbfs_ep_reset();

	/* clear all sticky interrupts */
	USBFS->GINTF =
		USBFS_GINTF_WKUPIF |
		USBFS_GINTF_SESIF |
		USBFS_GINTF_DISCIF |
		USBFS_GINTF_IDPSC |
		USBFS_GINTF_ISOONCIF |
		USBFS_GINTF_ISOINCIF |
		USBFS_GINTF_EOPFIF |
		USBFS_GINTF_ISOOPDIF |
		USBFS_GINTF_ENUMF |
		USBFS_GINTF_RST |
		USBFS_GINTF_SP |
		USBFS_GINTF_ESP |
		USBFS_GINTF_SOF |
		USBFS_GINTF_MFIF;

	/* enable interrupts */
	USBFS->GINTEN =
		USBFS_GINTEN_WKUPIE |
		USBFS_GINTEN_OEPIE |
		USBFS_GINTEN_IEPIE |
		USBFS_GINTEN_ENUMFIE |
		USBFS_GINTEN_RSTIE |
		USBFS_GINTEN_RXFNEIE |
		USBFS_GINTEN_SPIE;

	/* enable eclic interrupt */
	eclic_config(USBFS_IRQn, ECLIC_ATTR_TRIG_LEVEL, 4);
	eclic_enable(USBFS_IRQn);

	/* set usb global interrupt flag */
	USBFS->GAHBCS |= USBFS_GAHBCS_GINTEN;

	/* connect */
	USBFS->DCTL &= ~USBFS_DCTL_SD;
}
