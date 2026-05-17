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

/* Iso streaming parameters. 4 URBs × 8 packets/URB = 4 ms initial
 * latency at high-speed. Each microframe carries 5 or 6 sample-frames
 * per the 44100/8000 pacing residue counter. */
#define MC707_URB_COUNT          4
#define MC707_PACKETS_PER_URB    8

/* Per-substream state: which USB interface backs this PCM substream,
 * what alt we're currently on, and (once alt 1 is active) the iso
 * endpoint descriptor we feed to URBs in M3. */
struct mc707_substream {
	struct usb_interface         *intf;        /* iface 1 (out) or 2 (in) */
	struct usb_endpoint_descriptor *ep_desc;   /* set after alt switch */
	bool                          alt_active;  /* alt 1 vs alt 0 */

	/* PCM-thread / completion link */
	struct snd_pcm_substream     *substream;   /* set in pcm_open */
	bool                          running;     /* true between trigger START/STOP */

	/* URB ring for iso transfers. Allocated in hw_params, freed in hw_free.
	 * Each URB owns one usb_alloc_coherent'd buffer big enough to hold
	 * MC707_PACKETS_PER_URB worst-case-sized packets. buf_size is
	 * stashed here so urbs_free doesn't depend on ep_desc still being
	 * valid — disconnect can clear ep_desc before hw_free runs. */
	struct urb                   *urbs[MC707_URB_COUNT];
	size_t                        urb_buf_size;

	/* Pacing residue for 44100/8000: each packet, increment by 44100;
	 * if ≥ 8000 emit 6 sample-frames and subtract 8000, else emit 5.
	 * Across 8000 packets that sums to exactly 44100 sample-frames. */
	unsigned int                  pacing_residue;
};

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
	struct mc707_substream playback;     /* iface 1 */
	struct mc707_substream capture;      /* iface 2 */
};

/* Alt-setting we drive each audio interface to when streaming. Both
 * iface 1 and iface 2 declare alt 1 as the "full bandwidth" alt
 * (160 B / 524 B packets) and alt 2 as a 56-byte fallback we never use. */
#define MC707_AUDIO_ALT_ACTIVE  1
#define MC707_AUDIO_ALT_IDLE    0

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

static struct mc707_substream *mc707_sub_for(struct snd_pcm_substream *s)
{
	struct mc707_dev *d = s->pcm->private_data;

	return (s->stream == SNDRV_PCM_STREAM_PLAYBACK) ? &d->playback : &d->capture;
}

/* Activate alt 1 on this substream's USB interface and find the iso
 * endpoint descriptor (the one we'll drive in M3). */
static int mc707_activate_alt(struct mc707_substream *sub,
			      struct mc707_dev *d, bool playback)
{
	struct usb_host_interface *alt;
	int i, err;

	if (!sub->intf) {
		dev_err(&d->udev->dev,
			"MC-707: %s open with no matching interface bound\n",
			playback ? "playback" : "capture");
		return -ENODEV;
	}
	if (sub->alt_active)
		return 0;

	err = usb_set_interface(d->udev,
				sub->intf->cur_altsetting->desc.bInterfaceNumber,
				MC707_AUDIO_ALT_ACTIVE);
	if (err < 0) {
		dev_err(&sub->intf->dev,
			"MC-707: usb_set_interface(alt=%d) failed: %d\n",
			MC707_AUDIO_ALT_ACTIVE, err);
		return err;
	}

	/* Find the single iso endpoint exposed by alt 1. */
	alt = sub->intf->cur_altsetting;
	sub->ep_desc = NULL;
	for (i = 0; i < alt->desc.bNumEndpoints; i++) {
		struct usb_endpoint_descriptor *e = &alt->endpoint[i].desc;
		if (usb_endpoint_xfer_isoc(e)) {
			sub->ep_desc = e;
			break;
		}
	}
	if (!sub->ep_desc) {
		dev_err(&sub->intf->dev,
			"MC-707: no iso endpoint found on alt %d\n",
			MC707_AUDIO_ALT_ACTIVE);
		usb_set_interface(d->udev,
				  sub->intf->cur_altsetting->desc.bInterfaceNumber,
				  MC707_AUDIO_ALT_IDLE);
		return -ENODEV;
	}

	sub->alt_active = true;
	dev_info(&sub->intf->dev,
		 "MC-707: %s alt %d active — iso EP 0x%02x, mps %u, bInterval %u\n",
		 playback ? "playback" : "capture",
		 MC707_AUDIO_ALT_ACTIVE,
		 sub->ep_desc->bEndpointAddress,
		 usb_endpoint_maxp(sub->ep_desc),
		 sub->ep_desc->bInterval);
	return 0;
}

static void mc707_deactivate_alt(struct mc707_substream *sub,
				 struct mc707_dev *d)
{
	if (!sub->alt_active || !sub->intf)
		return;
	usb_set_interface(d->udev,
			  sub->intf->cur_altsetting->desc.bInterfaceNumber,
			  MC707_AUDIO_ALT_IDLE);
	sub->ep_desc = NULL;
	sub->alt_active = false;
}

/* ===== URB engine (M3 step 1: silence-only, no ALSA buffer touch) =========
 *
 * Pacing: 44100 sample-frames/sec spread across 8000 microframes/sec.
 * Residue counter: add 44100 per packet, emit 6 frames when ≥8000 then
 * subtract 8000, else emit 5. Averages exactly 5.5125 sample-frames per
 * microframe across each second.
 */

static unsigned int mc707_frames_for_packet(struct mc707_substream *sub)
{
	sub->pacing_residue += MC707_RATE;
	if (sub->pacing_residue >= 8000) {
		sub->pacing_residue -= 8000;
		return 6;
	}
	return 5;
}

static void mc707_urb_complete_out(struct urb *urb);

/* Free all URBs and their DMA buffers. Safe to call on partially-built
 * rings (entries may be NULL) and on URBs still queued at the HCD
 * (kill_urb waits for completion synchronously). */
static void mc707_urbs_free(struct mc707_substream *sub, struct mc707_dev *d)
{
	int i;

	for (i = 0; i < MC707_URB_COUNT; i++) {
		struct urb *urb = sub->urbs[i];
		if (!urb)
			continue;
		usb_kill_urb(urb);
		if (urb->transfer_buffer && sub->urb_buf_size)
			usb_free_coherent(d->udev, sub->urb_buf_size,
					  urb->transfer_buffer, urb->transfer_dma);
		usb_free_urb(urb);
		sub->urbs[i] = NULL;
	}
	sub->urb_buf_size = 0;
}

/* Allocate URBs + transfer buffers sized for the iso endpoint's max packet.
 * Pipe and per-packet descriptors are (re)set in mc707_pcm_prepare. */
static int mc707_urbs_alloc(struct mc707_substream *sub, struct mc707_dev *d)
{
	unsigned int mps;
	int i;

	if (!sub->ep_desc)
		return -ENODEV;

	mps = usb_endpoint_maxp(sub->ep_desc);
	sub->urb_buf_size = mps * MC707_PACKETS_PER_URB;

	for (i = 0; i < MC707_URB_COUNT; i++) {
		struct urb *urb;
		void *buf;

		urb = usb_alloc_urb(MC707_PACKETS_PER_URB, GFP_KERNEL);
		if (!urb)
			goto fail;

		buf = usb_alloc_coherent(d->udev, sub->urb_buf_size, GFP_KERNEL,
					 &urb->transfer_dma);
		if (!buf) {
			usb_free_urb(urb);
			goto fail;
		}
		urb->transfer_buffer = buf;
		urb->dev             = d->udev;
		urb->number_of_packets = MC707_PACKETS_PER_URB;
		urb->context         = sub;
		urb->complete        = mc707_urb_complete_out;
		urb->transfer_flags  = URB_ISO_ASAP | URB_NO_TRANSFER_DMA_MAP;
		sub->urbs[i] = urb;
	}
	return 0;

fail:
	mc707_urbs_free(sub, d);
	return -ENOMEM;
}

/* Fill one URB's iso packet descriptors + payload from the pacing counter.
 * Step 1: payload is silence (memset 0). */
static void mc707_urb_fill_silence(struct urb *urb, struct mc707_substream *sub,
				   bool is_playback)
{
	unsigned int offset = 0;
	unsigned int bytes_per_frame = (is_playback ? MC707_OUT_CHANNELS
						    : MC707_IN_CHANNELS)
				       * MC707_SUBFRAME_BYTES;
	int i;

	for (i = 0; i < urb->number_of_packets; i++) {
		unsigned int frames = mc707_frames_for_packet(sub);
		unsigned int bytes  = frames * bytes_per_frame;

		urb->iso_frame_desc[i].offset = offset;
		urb->iso_frame_desc[i].length = bytes;
		urb->iso_frame_desc[i].status = 0;
		urb->iso_frame_desc[i].actual_length = 0;
		offset += bytes;
	}
	urb->transfer_buffer_length = offset;
	memset(urb->transfer_buffer, 0, offset);
}

static void mc707_urb_complete_out(struct urb *urb)
{
	struct mc707_substream *sub = urb->context;
	int err;

	/* Shutdown / kill paths — just stop. */
	if (urb->status == -ESHUTDOWN || urb->status == -ENOENT ||
	    urb->status == -EINTR    || urb->status == -ECONNRESET)
		return;

	if (urb->status)
		dev_warn_ratelimited(&urb->dev->dev,
				     "MC-707: iso URB status %d\n", urb->status);

	if (!READ_ONCE(sub->running))
		return;

	mc707_urb_fill_silence(urb, sub, /*is_playback=*/true);

	err = usb_submit_urb(urb, GFP_ATOMIC);
	if (err && err != -ESHUTDOWN)
		dev_err_ratelimited(&urb->dev->dev,
				    "MC-707: iso URB resubmit failed: %d\n", err);
}

static int mc707_pcm_open(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct mc707_dev *d = substream->pcm->private_data;
	bool playback = (substream->stream == SNDRV_PCM_STREAM_PLAYBACK);
	struct mc707_substream *sub = mc707_sub_for(substream);
	int err;

	runtime->hw = playback ? mc707_pcm_hw_playback : mc707_pcm_hw_capture;

	/* M3 step 1: only playback. Capture stays as a stub. */
	if (!playback)
		return 0;

	err = mc707_activate_alt(sub, d, playback);
	if (err < 0)
		return err;

	sub->substream      = substream;
	sub->running        = false;
	sub->pacing_residue = 0;
	return 0;
}

static int mc707_pcm_close(struct snd_pcm_substream *substream)
{
	struct mc707_dev *d = substream->pcm->private_data;
	struct mc707_substream *sub = mc707_sub_for(substream);

	sub->substream = NULL;
	mc707_deactivate_alt(sub, d);
	return 0;
}

static int mc707_pcm_hw_params(struct snd_pcm_substream *substream,
			       struct snd_pcm_hw_params *params)
{
	struct mc707_dev *d = substream->pcm->private_data;
	struct mc707_substream *sub = mc707_sub_for(substream);

	if (substream->stream != SNDRV_PCM_STREAM_PLAYBACK)
		return 0;

	/* If a previous hw_params built a ring, drop it first. */
	mc707_urbs_free(sub, d);
	return mc707_urbs_alloc(sub, d);
}

static int mc707_pcm_hw_free(struct snd_pcm_substream *substream)
{
	struct mc707_dev *d = substream->pcm->private_data;
	struct mc707_substream *sub = mc707_sub_for(substream);

	if (substream->stream != SNDRV_PCM_STREAM_PLAYBACK)
		return 0;

	/* Make sure no URBs are still in flight from a previous run. */
	WRITE_ONCE(sub->running, false);
	mc707_urbs_free(sub, d);
	return 0;
}

static int mc707_pcm_prepare(struct snd_pcm_substream *substream)
{
	struct mc707_substream *sub = mc707_sub_for(substream);
	struct mc707_dev *d = substream->pcm->private_data;
	bool playback = (substream->stream == SNDRV_PCM_STREAM_PLAYBACK);
	int i;

	if (!playback)
		return 0;

	/* Re-derive pipe + per-packet descriptors from the active EP. */
	for (i = 0; i < MC707_URB_COUNT; i++) {
		struct urb *urb = sub->urbs[i];
		if (!urb)
			return -ENODEV;
		urb->pipe     = usb_sndisocpipe(d->udev,
					        usb_endpoint_num(sub->ep_desc));
		urb->interval = 1;   /* every microframe (high-speed bInterval=1) */
		mc707_urb_fill_silence(urb, sub, playback);
	}
	sub->pacing_residue = 0;
	return 0;
}

static int mc707_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct mc707_substream *sub = mc707_sub_for(substream);
	int i, err;

	if (substream->stream != SNDRV_PCM_STREAM_PLAYBACK)
		return -ENOSYS;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		WRITE_ONCE(sub->running, true);
		for (i = 0; i < MC707_URB_COUNT; i++) {
			err = usb_submit_urb(sub->urbs[i], GFP_ATOMIC);
			if (err) {
				dev_err(&sub->intf->dev,
					"MC-707: iso URB[%d] submit failed: %d\n",
					i, err);
				WRITE_ONCE(sub->running, false);
				/* Cancel any already-queued URBs. */
				while (--i >= 0)
					usb_kill_urb(sub->urbs[i]);
				return err;
			}
		}
		return 0;

	case SNDRV_PCM_TRIGGER_STOP:
		WRITE_ONCE(sub->running, false);
		for (i = 0; i < MC707_URB_COUNT; i++)
			usb_kill_urb(sub->urbs[i]);
		return 0;

	default:
		return -EINVAL;
	}
}

static snd_pcm_uframes_t mc707_pcm_pointer(struct snd_pcm_substream *substream)
{
	/* Step 1 returns 0 — no real ALSA-buffer tracking yet. ALSA will
	 * see no progress and aplay will eventually time out, but URBs
	 * are still going out on the wire and we can observe device
	 * behavior in dmesg / a USB analyzer. Step 2 will wire this up. */
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
	struct mc707_substream *sub = is_playback ? &d->playback : &d->capture;
	int err;

	err = mc707_create_pcm(d);
	if (err < 0) {
		dev_err(&intf->dev, "MC-707: snd_pcm_new failed: %d\n", err);
		return err;
	}
	sub->intf = intf;
	sub->alt_active = false;
	sub->ep_desc = NULL;

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

	/* Defensive: kill any in-flight iso URBs on BOTH audio substreams
	 * before anything else. Disconnect order isn't guaranteed (we may
	 * be called for iface 3 first while ifaces 1/2 still have URBs
	 * queued), and a torn-down audio path with live URBs racing the
	 * MIDI disconnect has been observed to wedge the device hard
	 * enough that `snd_usbmidi_disconnect → __timer_delete_sync`
	 * hangs forever and the box needs a reboot. Always kill our URBs
	 * first regardless of which interface we're being disconnected
	 * for. usb_kill_urb is safe on never-submitted or already-killed
	 * URBs. */
	{
		struct mc707_substream *subs[] = { &d->playback, &d->capture };
		int s, k;
		for (s = 0; s < ARRAY_SIZE(subs); s++) {
			WRITE_ONCE(subs[s]->running, false);
			for (k = 0; k < MC707_URB_COUNT; k++)
				if (subs[s]->urbs[k])
					usb_kill_urb(subs[s]->urbs[k]);
		}
	}

	switch (ifnum) {
	case MC707_MIDI_IFNUM:
		snd_usbmidi_disconnect(&d->midi_list);
		break;
	case MC707_AUDIO_OUT_IFNUM:
		/* Clear our pointer into this interface so PCM open can't
		 * touch it after disconnect. URBs were already killed above;
		 * the URB structures and DMA buffers stay until pcm_hw_free
		 * (or until the snd_card is freed below). */
		d->playback.intf = NULL;
		d->playback.ep_desc = NULL;
		d->playback.alt_active = false;
		break;
	case MC707_AUDIO_IN_IFNUM:
		d->capture.intf = NULL;
		d->capture.ep_desc = NULL;
		d->capture.alt_active = false;
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
