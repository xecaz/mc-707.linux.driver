// SPDX-License-Identifier: GPL-2.0-or-later
//
// snd-roland-mc707: out-of-tree ALSA driver for the Roland MC-707 groovebox
// in its native vendor-specific USB mode (USB 0582:0229).
//
// M1: MIDI working (interface 3, interrupt EPs on alt 1).
// M2 (this file): also claim audio interfaces 1 and 2; create a single
//   snd_card shared across all interfaces; expose a snd_pcm device with
//   playback (6 ch) and capture (20 ch), both 24-bit/44.1 kHz. No streaming
//   yet — that's M3. `aplay -l` and `arecord -l` should show the MC-707.
//
// Audio interfaces are UAC1 Type I Format under a vendor-class wrapper —
// see notes/audio-init.md for the RE rationale. No Roland-specific vendor
// control protocol is needed for audio init; rates are baked into the
// descriptor (only 44.1 kHz advertised).

#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/usb.h>
#include <linux/list.h>
#include <linux/mutex.h>

#include <sound/core.h>
#include <sound/initval.h>
#include <sound/pcm.h>

#include "alsa_usb_priv.h"

#define DRIVER_NAME      "snd-roland-mc707"
#define DRIVER_LONGNAME  "Roland MC-707 (vendor mode)"

#define USB_VID_ROLAND   0x0582
#define USB_PID_MC707    0x0229

#define MC707_AUDIO_OUT_IFNUM   1   /* playback to device, 6 ch  */
#define MC707_AUDIO_IN_IFNUM    2   /* capture from device, 20 ch */
#define MC707_MIDI_IFNUM        3

/* Interrupt MIDI endpoints live on iface 3 alt 1. We use the interrupt
 * transport (NOT the bulk transport on alt 0) because upstream's
 * snd_usbmidi_switch_roland_altsetting() — triggered for any Roland
 * vendor (0x0582) with 2 altsettings — forcibly switches to alt 1 when
 * alt 1 has interrupt endpoints. bInterval=4 at high-speed → 1ms polling.
 *
 * out_ep/in_ep must be endpoint NUMBERS (low 4 bits), not addresses.
 * struct snd_usb_midi_endpoint_info uses int8_t; bit-7-set values
 * sign-extend on int promotion and corrupt URB pipe-type bits. For
 * PIPE_INTERRUPT that breaks usb_urb_ep_type_check. (See notes.) */
#define MC707_MIDI_OUT_EP    0x03  /* iface 3 alt 1, EP 3 OUT */
#define MC707_MIDI_IN_EP     0x05  /* iface 3 alt 1, EP 5 IN  */
#define MC707_MIDI_INTERVAL  4

/* Audio format constants — from iface 1/2 alt 1 UAC1 Type I Format
 * descriptors (`0b 24 02 01 NN 04 18 01 44 ac 00`). */
#define MC707_RATE             44100
#define MC707_BITS_PER_SAMPLE  24
#define MC707_SUBFRAME_BYTES   4     /* 24-bit in 32-bit container */
#define MC707_OUT_CHANNELS     6
#define MC707_IN_CHANNELS      20

/* Per-USB-device state. One mc707_dev exists per plugged MC-707,
 * shared by all its bound interfaces. Lives in card->private_data; freed
 * automatically when the snd_card is freed.
 *
 * USB interfaces probe one at a time, in numerical order. We use a
 * global list of mc707_devs keyed by usb_device * so each iface's probe
 * handler can find or create the shared state. Lock with
 * mc707_devices_lock. */
struct mc707_dev {
	struct list_head    list;            /* in mc707_devices_list */
	struct usb_device  *udev;
	struct snd_card    *card;
	bool                registered;      /* snd_card_register done */
	int                 intf_refcount;   /* bound interfaces */

	/* MIDI (iface 3) */
	struct list_head    midi_list;       /* snd-usbmidi-lib's list */

	/* PCM (iface 1 = playback, iface 2 = capture) */
	struct snd_pcm     *pcm;
};

static DEFINE_MUTEX(mc707_devices_lock);
static LIST_HEAD(mc707_devices_list);

/* ===== MIDI ============================================================== */

static const struct snd_usb_midi_endpoint_info mc707_midi_ep_info = {
	.out_ep       = MC707_MIDI_OUT_EP,
	.out_interval = MC707_MIDI_INTERVAL,
	.in_ep        = MC707_MIDI_IN_EP,
	.in_interval  = MC707_MIDI_INTERVAL,
	.out_cables   = 0x0001,
	.in_cables    = 0x0001,
};

static const struct snd_usb_audio_quirk mc707_midi_quirk = {
	.vendor_name  = "Roland",
	.product_name = "MC-707",
	.ifnum        = MC707_MIDI_IFNUM,
	.type         = QUIRK_MIDI_FIXED_ENDPOINT,
	.data         = &mc707_midi_ep_info,
};

/* ===== PCM stubs =========================================================
 *
 * M2 only exposes the device structure. Open/close/hw_params are accepted
 * so userspace can probe the format. trigger() refuses START — actual
 * streaming is M3.
 */

static const struct snd_pcm_hardware mc707_pcm_hw_playback = {
	.info             = SNDRV_PCM_INFO_INTERLEAVED |
			    SNDRV_PCM_INFO_BLOCK_TRANSFER,
	.formats          = SNDRV_PCM_FMTBIT_S24_LE,
	.rates            = SNDRV_PCM_RATE_44100,
	.rate_min         = MC707_RATE,
	.rate_max         = MC707_RATE,
	.channels_min     = MC707_OUT_CHANNELS,
	.channels_max     = MC707_OUT_CHANNELS,
	.buffer_bytes_max = 64 * 1024,
	.period_bytes_min = 64,
	.period_bytes_max = 16 * 1024,
	.periods_min      = 2,
	.periods_max      = 64,
};

static const struct snd_pcm_hardware mc707_pcm_hw_capture = {
	.info             = SNDRV_PCM_INFO_INTERLEAVED |
			    SNDRV_PCM_INFO_BLOCK_TRANSFER,
	.formats          = SNDRV_PCM_FMTBIT_S24_LE,
	.rates            = SNDRV_PCM_RATE_44100,
	.rate_min         = MC707_RATE,
	.rate_max         = MC707_RATE,
	.channels_min     = MC707_IN_CHANNELS,
	.channels_max     = MC707_IN_CHANNELS,
	.buffer_bytes_max = 256 * 1024,
	.period_bytes_min = 64,
	.period_bytes_max = 64 * 1024,
	.periods_min      = 2,
	.periods_max      = 64,
};

static int mc707_pcm_open(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;

	runtime->hw = (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		      ? mc707_pcm_hw_playback : mc707_pcm_hw_capture;
	return 0;
}

static int mc707_pcm_close(struct snd_pcm_substream *substream)
{
	return 0;
}

static int mc707_pcm_hw_params(struct snd_pcm_substream *substream,
			       struct snd_pcm_hw_params *params)
{
	return 0;
}

static int mc707_pcm_hw_free(struct snd_pcm_substream *substream)
{
	return 0;
}

static int mc707_pcm_prepare(struct snd_pcm_substream *substream)
{
	return 0;
}

static int mc707_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	/* M2: no streaming yet; M3 will wire up iso URBs here. */
	dev_warn_once(substream->pcm->card->dev,
		      "MC-707: PCM trigger requested but streaming is not implemented (M3)\n");
	return -ENOSYS;
}

static snd_pcm_uframes_t mc707_pcm_pointer(struct snd_pcm_substream *substream)
{
	return 0;
}

static const struct snd_pcm_ops mc707_pcm_ops = {
	.open      = mc707_pcm_open,
	.close     = mc707_pcm_close,
	.hw_params = mc707_pcm_hw_params,
	.hw_free   = mc707_pcm_hw_free,
	.prepare   = mc707_pcm_prepare,
	.trigger   = mc707_pcm_trigger,
	.pointer   = mc707_pcm_pointer,
};

static int mc707_create_pcm(struct mc707_dev *d)
{
	struct snd_pcm *pcm;
	int err;

	if (d->pcm)
		return 0;

	err = snd_pcm_new(d->card, "MC-707", 0, 1, 1, &pcm);
	if (err < 0)
		return err;

	pcm->private_data = d;
	strscpy(pcm->name, "MC-707", sizeof(pcm->name));
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &mc707_pcm_ops);
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE,  &mc707_pcm_ops);

	d->pcm = pcm;
	return 0;
}

/* ===== Per-interface probe handlers ====================================== */

static int mc707_attach_midi(struct usb_interface *intf, struct mc707_dev *d)
{
	int err;

	err = __snd_usbmidi_create(d->card, intf, &d->midi_list,
				   &mc707_midi_quirk,
				   USB_ID(USB_VID_ROLAND, USB_PID_MC707),
				   NULL);
	if (err < 0) {
		dev_err(&intf->dev,
			"MC-707: __snd_usbmidi_create() failed: %d\n", err);
		return err;
	}

	snd_usbmidi_input_start(&d->midi_list);
	dev_info(&intf->dev, "MC-707: MIDI online (iface %d)\n",
		 MC707_MIDI_IFNUM);
	return 0;
}

static int mc707_attach_audio(struct usb_interface *intf, struct mc707_dev *d,
			      bool is_playback)
{
	int err;

	err = mc707_create_pcm(d);
	if (err < 0) {
		dev_err(&intf->dev, "MC-707: snd_pcm_new failed: %d\n", err);
		return err;
	}

	dev_info(&intf->dev,
		 "MC-707: audio %s claimed (iface %u) — %u ch × 24-bit × %u Hz (streaming pending M3)\n",
		 is_playback ? "OUT" : "IN",
		 intf->cur_altsetting->desc.bInterfaceNumber,
		 is_playback ? MC707_OUT_CHANNELS : MC707_IN_CHANNELS,
		 MC707_RATE);
	return 0;
}

/* ===== Card lifecycle ==================================================== */

/* Caller must hold mc707_devices_lock. */
static struct mc707_dev *mc707_find_locked(struct usb_device *udev)
{
	struct mc707_dev *d;

	list_for_each_entry(d, &mc707_devices_list, list)
		if (d->udev == udev)
			return d;
	return NULL;
}

/* Caller must hold mc707_devices_lock. */
static struct mc707_dev *mc707_alloc_card(struct usb_interface *intf,
					  struct usb_device *udev)
{
	struct mc707_dev *d;
	struct snd_card *card;
	int err;

	err = snd_card_new(&intf->dev, SNDRV_DEFAULT_IDX1, "MC707",
			   THIS_MODULE, sizeof(*d), &card);
	if (err < 0)
		return ERR_PTR(err);

	d = card->private_data;
	d->card = card;
	d->udev = udev;
	INIT_LIST_HEAD(&d->midi_list);

	strscpy(card->driver,    DRIVER_NAME,     sizeof(card->driver));
	strscpy(card->shortname, "Roland MC-707", sizeof(card->shortname));
	scnprintf(card->longname, sizeof(card->longname),
		  "%s at %s", DRIVER_LONGNAME, dev_name(&udev->dev));

	list_add(&d->list, &mc707_devices_list);
	return d;
}

static int mc707_probe(struct usb_interface *intf,
		       const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev(intf);
	u8 ifnum = intf->cur_altsetting->desc.bInterfaceNumber;
	struct mc707_dev *d;
	bool created = false;
	int err;

	dev_info(&intf->dev,
		 "MC-707 probe: bus %d dev %d, ifnum %u, bcdDevice 0x%04x\n",
		 udev->bus->busnum, udev->devnum, ifnum,
		 le16_to_cpu(udev->descriptor.bcdDevice));

	/* Iface 0 is the control-only anchor (no endpoints, no work for us). */
	if (ifnum != MC707_AUDIO_OUT_IFNUM &&
	    ifnum != MC707_AUDIO_IN_IFNUM  &&
	    ifnum != MC707_MIDI_IFNUM) {
		dev_info(&intf->dev,
			 "MC-707: skipping iface %u (no handler)\n", ifnum);
		return -ENODEV;
	}

	mutex_lock(&mc707_devices_lock);

	d = mc707_find_locked(udev);
	if (!d) {
		d = mc707_alloc_card(intf, udev);
		if (IS_ERR(d)) {
			err = PTR_ERR(d);
			goto out_unlock;
		}
		created = true;
	}

	switch (ifnum) {
	case MC707_AUDIO_OUT_IFNUM:
		err = mc707_attach_audio(intf, d, /*is_playback=*/true);
		break;
	case MC707_AUDIO_IN_IFNUM:
		err = mc707_attach_audio(intf, d, /*is_playback=*/false);
		break;
	case MC707_MIDI_IFNUM:
		err = mc707_attach_midi(intf, d);
		break;
	default:
		err = -ENODEV;
		break;
	}

	if (err < 0) {
		/* If we allocated the card for this probe and the attach
		 * failed, tear it down so we don't leave a zombie. */
		if (created) {
			list_del(&d->list);
			snd_card_free(d->card);
		}
		goto out_unlock;
	}

	/* snd_card_register is idempotent — first call does the full
	 * registration, subsequent calls register newly-added devices.
	 * We call it on every successful interface attach so the card
	 * appears as soon as at least one component is ready. */
	err = snd_card_register(d->card);
	if (err < 0) {
		dev_err(&intf->dev,
			"MC-707: snd_card_register failed: %d\n", err);
		/* Best-effort: we leave the partially-built card in place
		 * and rely on the disconnect path to tear down. */
		goto out_unlock;
	}
	d->registered = true;
	d->intf_refcount++;
	usb_set_intfdata(intf, d);

out_unlock:
	mutex_unlock(&mc707_devices_lock);
	return err;
}

static void mc707_disconnect(struct usb_interface *intf)
{
	struct mc707_dev *d = usb_get_intfdata(intf);
	u8 ifnum;

	if (!d)
		return;

	ifnum = intf->cur_altsetting->desc.bInterfaceNumber;

	mutex_lock(&mc707_devices_lock);

	dev_info(&intf->dev,
		 "MC-707 disconnect: ifnum %u (refcount %d → %d)\n",
		 ifnum, d->intf_refcount, d->intf_refcount - 1);

	switch (ifnum) {
	case MC707_MIDI_IFNUM:
		snd_usbmidi_disconnect(&d->midi_list);
		break;
	case MC707_AUDIO_OUT_IFNUM:
	case MC707_AUDIO_IN_IFNUM:
		/* M2: nothing to tear down per-iface — the PCM device
		 * lives with the snd_card and is freed below when the
		 * last interface disconnects. */
		break;
	}

	usb_set_intfdata(intf, NULL);
	d->intf_refcount--;

	if (d->intf_refcount == 0) {
		list_del(&d->list);
		snd_card_disconnect(d->card);
		snd_card_free_when_closed(d->card);
	}

	mutex_unlock(&mc707_devices_lock);
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
MODULE_VERSION("0.2");
