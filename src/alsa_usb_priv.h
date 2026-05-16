// SPDX-License-Identifier: GPL-2.0-or-later
//
// Private re-declarations of internal ALSA USB structures and the
// __snd_usbmidi_create() prototype, copied verbatim from Linux 6.12's
// `sound/usb/midi.h` and `sound/usb/usbaudio.h`.
//
// These headers are NOT part of the public kernel ABI exposed by
// linux-headers-$(uname -r). They are internal to the in-tree
// snd-usb-audio driver. We copy them here so an out-of-tree module can
// call __snd_usbmidi_create(), which IS exported via EXPORT_SYMBOL from
// snd-usbmidi-lib (confirmed in /usr/src/linux-headers-.../Module.symvers).
//
// **Stability warning:** if these struct layouts change in a future
// kernel, this module will silently corrupt memory on load. Keep this
// file synced to the running kernel's source whenever upgrading.

#ifndef MC707_ALSA_USB_PRIV_H
#define MC707_ALSA_USB_PRIV_H

#include <linux/types.h>
#include <linux/list.h>

struct snd_card;
struct usb_interface;

/* From sound/usb/usbaudio.h ---------------------------------------- */

struct snd_usb_audio_quirk {
	const char *vendor_name;
	const char *product_name;
	int16_t ifnum;
	uint16_t type;
	bool shares_media_device;
	const void *data;
};

#define QUIRK_NO_INTERFACE	-2
#define QUIRK_ANY_INTERFACE	-1

enum {
	QUIRK_IGNORE_INTERFACE,
	QUIRK_COMPOSITE,
	QUIRK_AUTODETECT,
	QUIRK_MIDI_STANDARD_INTERFACE,
	QUIRK_MIDI_FIXED_ENDPOINT,
	QUIRK_MIDI_YAMAHA,
	QUIRK_MIDI_ROLAND,
	QUIRK_MIDI_MIDIMAN,
	QUIRK_MIDI_NOVATION,
	QUIRK_MIDI_RAW_BYTES,
	QUIRK_MIDI_EMAGIC,
	QUIRK_MIDI_CME,
	QUIRK_MIDI_AKAI,
	QUIRK_MIDI_US122L,
	QUIRK_MIDI_FTDI,
	QUIRK_MIDI_CH345,
	QUIRK_AUDIO_STANDARD_INTERFACE,
	QUIRK_AUDIO_FIXED_ENDPOINT,
	QUIRK_AUDIO_EDIROL_UAXX,
	QUIRK_AUDIO_STANDARD_MIXER,
	QUIRK_SETUP_FMT_AFTER_RESUME,
	QUIRK_SETUP_DISABLE_AUTOSUSPEND,
	QUIRK_TYPE_COUNT
};

#define USB_ID(vendor, product) (((unsigned int)(vendor) << 16) | (product))

/* From sound/usb/midi.h -------------------------------------------- */

struct snd_usb_midi_endpoint_info {
	int8_t  out_ep;       /* ep number, 0 = autodetect */
	uint8_t out_interval; /* interval for interrupt endpoints */
	int8_t  in_ep;
	uint8_t in_interval;
	uint16_t out_cables;  /* bitmask */
	uint16_t in_cables;   /* bitmask */
};

int __snd_usbmidi_create(struct snd_card *card,
			 struct usb_interface *iface,
			 struct list_head *midi_list,
			 const struct snd_usb_audio_quirk *quirk,
			 unsigned int usb_id);

void snd_usbmidi_input_start(struct list_head *p);
void snd_usbmidi_input_stop(struct list_head *p);
void snd_usbmidi_disconnect(struct list_head *p);

#endif /* MC707_ALSA_USB_PRIV_H */
