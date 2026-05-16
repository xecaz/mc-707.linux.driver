// SPDX-License-Identifier: GPL-2.0-or-later
//
// snd-roland-mc707: out-of-tree ALSA driver for the Roland MC-707 groovebox
// in its native vendor-specific USB mode (USB 0582:0229).
//
// See ../README.md and /home/xecaz/code/mc707/CLAUDE.md for project context.
//
// This file is the M0 skeleton — it registers a usb_driver, logs probe/disconnect,
// and does nothing else yet. M1 will hand interface 3 to snd_usbmidi_create().

#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/usb.h>

#define DRIVER_NAME "snd-roland-mc707"

#define USB_VID_ROLAND   0x0582
#define USB_PID_MC707    0x0229

static int mc707_probe(struct usb_interface *intf,
		       const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev(intf);
	u8 ifnum = intf->cur_altsetting->desc.bInterfaceNumber;

	dev_info(&intf->dev,
		 "MC-707 probe: bus %d dev %d, ifnum %u, bcdDevice 0x%04x\n",
		 udev->bus->busnum, udev->devnum, ifnum,
		 le16_to_cpu(udev->descriptor.bcdDevice));

	return 0;
}

static void mc707_disconnect(struct usb_interface *intf)
{
	dev_info(&intf->dev, "MC-707 disconnect: ifnum %u\n",
		 intf->cur_altsetting->desc.bInterfaceNumber);
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
