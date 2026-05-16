// SPDX-License-Identifier: GPL-2.0-or-later
//
// snd-roland-mc707: out-of-tree ALSA driver for the Roland MC-707 groovebox
// in its native vendor-specific USB mode (USB 0582:0229).
//
// M1 scope: only interface 3 (MIDI) is handled. Interfaces 0/1/2 are
// rejected (`-ENODEV`) so they remain unbound — M2/M3 will fill them in.
//
// MIDI strategy: hand interface 3 to ALSA's `__snd_usbmidi_create()` with
// `QUIRK_MIDI_FIXED_ENDPOINT` describing the two bulk endpoints. Every
// modern Roland device in mainline `sound/usb/quirks-table.h` uses this
// quirk (NOT `QUIRK_MIDI_ROLAND`, which is for ancient SC-88-era framing).
// If SysEx round-trip ends up malformed, fall back to QUIRK_MIDI_ROLAND.

#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/usb.h>

#include <sound/core.h>
#include <sound/initval.h>

#include "alsa_usb_priv.h"

#define DRIVER_NAME      "snd-roland-mc707"
#define DRIVER_LONGNAME  "Roland MC-707 (vendor mode)"

#define USB_VID_ROLAND   0x0582
#define USB_PID_MC707    0x0229

#define MC707_MIDI_IFNUM   3
/* Interrupt MIDI endpoints live on iface 3 alt 1. We use the interrupt
 * transport (NOT the bulk transport on alt 0) because upstream's
 * snd_usbmidi_switch_roland_altsetting() — triggered for any Roland
 * vendor (0x0582) with 2 altsettings — forcibly switches to alt 1 when
 * alt 1 has interrupt endpoints. Fighting that switch would mean lying
 * about our vendor ID. bInterval=4 at high-speed → 1ms polling.
 *
 * IMPORTANT: out_ep/in_ep must be endpoint NUMBERS (low 4 bits), not
 * addresses. struct snd_usb_midi_endpoint_info uses int8_t for these
 * fields; if bit 7 is set (e.g. 0x85 for IN-EP-5), sign extension on
 * int promotion sets bits 16-31, including bits 30-31 which encode
 * pipe type. For PIPE_INTERRUPT (= 1) that corruption flips the
 * encoded type to PIPE_BULK (= 3), and usb_urb_ep_type_check rejects
 * the URB. (For PIPE_BULK the corruption is a no-op — which is why
 * mainline quirks like `.in_ep = 0x82` happen to work for bulk MIDI.) */
#define MC707_MIDI_OUT_EP    0x03  /* iface 3 alt 1, EP 3 OUT */
#define MC707_MIDI_IN_EP     0x05  /* iface 3 alt 1, EP 5 IN  */
#define MC707_MIDI_INTERVAL  4

/* Per-device state. Anchored on interface 3 since that's currently the
 * only interface we attach to. M2/M3 will move this to a device-wide
 * structure shared across interfaces. */
struct mc707_card {
	struct snd_card    *card;
	struct usb_device  *udev;
	struct list_head    midi_list;
};

static const struct snd_usb_midi_endpoint_info mc707_midi_ep_info = {
	.out_ep       = MC707_MIDI_OUT_EP,
	.out_interval = MC707_MIDI_INTERVAL,
	.in_ep        = MC707_MIDI_IN_EP,
	.in_interval  = MC707_MIDI_INTERVAL,
	.out_cables   = 0x0001,  /* 1 OUT cable — class descriptor
				  * `06 24 f1 02 01 01` (first hypothesis). */
	.in_cables    = 0x0001,  /* 1 IN cable, same source. */
};

static const struct snd_usb_audio_quirk mc707_midi_quirk = {
	.vendor_name  = "Roland",
	.product_name = "MC-707",
	.ifnum        = MC707_MIDI_IFNUM,
	.type         = QUIRK_MIDI_FIXED_ENDPOINT,
	.data         = &mc707_midi_ep_info,
};

static int mc707_probe_midi(struct usb_interface *intf,
			    const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev(intf);
	struct mc707_card *mc;
	struct snd_card *card;
	int err;

	/* Don't force alt 0. snd_usbmidi_switch_roland_altsetting() in
	 * __snd_usbmidi_create() will switch us to alt 1 (interrupt EPs)
	 * for any Roland device whose alt 1 advertises interrupt
	 * endpoints — which is exactly our case. */

	err = snd_card_new(&intf->dev, SNDRV_DEFAULT_IDX1, "MC707",
			   THIS_MODULE, sizeof(*mc), &card);
	if (err < 0)
		return err;

	mc = card->private_data;
	mc->card = card;
	mc->udev = udev;
	INIT_LIST_HEAD(&mc->midi_list);

	strscpy(card->driver,    DRIVER_NAME,     sizeof(card->driver));
	strscpy(card->shortname, "Roland MC-707", sizeof(card->shortname));
	scnprintf(card->longname, sizeof(card->longname),
		  "%s at %s", DRIVER_LONGNAME, dev_name(&udev->dev));

	err = __snd_usbmidi_create(card, intf, &mc->midi_list,
				   &mc707_midi_quirk,
				   USB_ID(USB_VID_ROLAND, USB_PID_MC707),
				   NULL /* num_rawmidis out-param; unused */);
	if (err < 0) {
		dev_err(&intf->dev,
			"MC-707: __snd_usbmidi_create() failed: %d\n", err);
		goto err_card;
	}

	err = snd_card_register(card);
	if (err < 0) {
		dev_err(&intf->dev,
			"MC-707: snd_card_register() failed: %d\n", err);
		goto err_midi;
	}

	usb_set_intfdata(intf, mc);
	snd_usbmidi_input_start(&mc->midi_list);

	dev_info(&intf->dev,
		 "MC-707: MIDI online (card %d, %s)\n",
		 card->number, card->shortname);
	return 0;

err_midi:
	snd_usbmidi_disconnect(&mc->midi_list);
err_card:
	snd_card_free(card);
	return err;
}

static int mc707_probe(struct usb_interface *intf,
		       const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev(intf);
	u8 ifnum = intf->cur_altsetting->desc.bInterfaceNumber;

	dev_info(&intf->dev,
		 "MC-707 probe: bus %d dev %d, ifnum %u, bcdDevice 0x%04x\n",
		 udev->bus->busnum, udev->devnum, ifnum,
		 le16_to_cpu(udev->descriptor.bcdDevice));

	if (ifnum != MC707_MIDI_IFNUM) {
		dev_info(&intf->dev,
			 "MC-707: skipping non-MIDI interface %u (M1 scope; "
			 "audio comes in M2/M3)\n", ifnum);
		return -ENODEV;
	}

	return mc707_probe_midi(intf, id);
}

static void mc707_disconnect(struct usb_interface *intf)
{
	struct mc707_card *mc = usb_get_intfdata(intf);
	u8 ifnum = intf->cur_altsetting->desc.bInterfaceNumber;

	if (!mc) {
		/* Non-MIDI interface (or never attached) — nothing to undo. */
		dev_info(&intf->dev,
			 "MC-707 disconnect: ifnum %u (unmanaged)\n", ifnum);
		return;
	}

	dev_info(&intf->dev,
		 "MC-707 disconnect: ifnum %u, tearing down card %d\n",
		 ifnum, mc->card->number);

	snd_usbmidi_disconnect(&mc->midi_list);
	snd_card_disconnect(mc->card);
	snd_card_free_when_closed(mc->card);
	usb_set_intfdata(intf, NULL);
}

static const struct usb_device_id mc707_id_table[] = {
	{ USB_DEVICE(USB_VID_ROLAND, USB_PID_MC707) },
	{ }
};
MODULE_DEVICE_TABLE(usb, mc707_id_table);

static struct usb_driver mc707_driver = {
	.name		= DRIVER_NAME,
	.probe		= mc707_probe,
	.disconnect	= mc707_disconnect,
	.id_table	= mc707_id_table,
};

module_usb_driver(mc707_driver);

MODULE_AUTHOR("xecaz");
MODULE_DESCRIPTION("ALSA driver for Roland MC-707 in vendor USB mode");
MODULE_LICENSE("GPL");
MODULE_VERSION("0.1");
