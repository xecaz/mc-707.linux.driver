# snd-roland-mc707

Out-of-tree Linux ALSA driver for the **Roland MC-707** groovebox in its native
vendor-specific USB mode (USB `0582:0229`).

## Goal

Expose, simultaneously and on a stock Linux box:

- **6** audio output channels (PC → MC-707), 24-bit / 44.1 kHz (isochronous EP `0x0D`).
- **20** audio input channels (MC-707 → PC), 24-bit / 44.1 kHz (isochronous EP `0x8E`, implicit-feedback).
- **MIDI** in/out via the device's native Roland-vendor USB-MIDI framing
  (interrupt EPs `0x03` OUT / `0x85` IN on iface 3 alt 1, bInterval=4 → 1 ms
  poll at high-speed).

The in-tree `snd-usb-audio` driver does not bind because all four interfaces
report `bInterfaceClass = 0xFF` (vendor-specific). The MC-707's firmware ≥ 1.60
"GENERIC" class-compliant mode exists but collapses I/O to a single stereo pair,
defeating the point of a groovebox. This driver targets the full vendor mode.

No public Linux driver exists for the MC-707 — nor for any Roland AIRA/MC-series
device in vendor mode with multichannel audio. The closest prior community
attempt (AIRA TR-8, Gearspace 2015) stalled at isochronous implicit feedback,
which we expect to be the hardest piece (M3 below).

## Status

- **M0 — Environment / scaffolding**: ✅ done.
  Module builds out-of-tree against the running kernel; all four USB
  interfaces probe; the MIDI interface gets a `dev_info` log; udev rule
  grants `plugdev` users RW on the device.
- **M1 — MIDI working end-to-end**: ✅ done.
  `amidi -l` shows `hw:1,0,0  Roland MC-707 MIDI 1`. Sending a Note-On
  (`amidi -p hw:1,0,0 -S "90 24 7F"`) audibly triggers the device's
  internal tone engine (verified with a drum kit on track 1, MIDI Rx
  channel 1). Uses upstream `snd-usbmidi-lib` via the exported
  `__snd_usbmidi_create()` with `QUIRK_MIDI_FIXED_ENDPOINT` over the
  interrupt-EP alt-setting (alt 1 of interface 3).
- **M2 — Audio init sequence (no streaming)**: not started.
  Setting sample rate / clock source via Roland's vendor control
  transfers, alt-setting selection for the iso interfaces, descriptor
  handshake. The Windows ASIO DLL (`RDAS1207.DLL`) exposes
  `setClockSource(int)` even though the USB descriptors only advertise
  44.1 kHz alt-settings — strongly suggesting vendor control transfers
  carry that negotiation. RE target.
- **M3 — Multichannel audio streaming with implicit feedback**: not started.
  6-channel iso OUT + 20-channel iso IN, with the IN endpoint also
  carrying implicit feedback timing for the OUT stream. The hard piece.

See the detailed roadmap in
[`/home/xecaz/code/mc707/CLAUDE.md`](../mc707/CLAUDE.md), the full plan
at `~/.claude/plans/in-this-folder-i-composed-blossom.md`, and the
"Hard-won facts" section in CLAUDE.md for the upstream-quirk subtleties
that cost the most time (Roland alt-setting override in
`snd_usbmidi_switch_roland_altsetting`, `int8_t` sign-extension in
`snd_usb_midi_endpoint_info`, MC-707 booting on alt 1 not alt 0, etc.).

## Building

Requires `linux-headers-$(uname -r)`, `build-essential`, and `dkms`.

```
make            # build against the running kernel
make modules    # explicit alias
sudo make load  # insmod the freshly built module
sudo make unload
```

DKMS install (system-wide, autoloads on hot-plug):

```
sudo dkms add .
sudo dkms build snd-roland-mc707/0.1
sudo dkms install snd-roland-mc707/0.1
```

## Verifying MIDI works

With the MC-707 plugged in and the module loaded:

```
$ amidi -l
Dir Device    Name
IO  hw:1,0,0  Roland MC-707 MIDI 1
```

To trigger an audible note, send a Note-On / Note-Off pair. The MC-707
must be listening on the matching MIDI channel — by default a drum kit
on track 1 responds to channel 1, drum hits live around note `0x24`
(kick) / `0x26` (snare):

```
amidi -p hw:1,0,0 -S "90 24 7F"   # Note On  ch1, note 36, vel 127 → kick
sleep 0.3
amidi -p hw:1,0,0 -S "80 24 00"   # Note Off
```

If silent, check the device: `MENU → SYSTEM → MIDI → Rx Channel` and the
per-track MIDI receive page.

## Layout

- `src/` — kernel module source.
- `udev/70-roland-mc707.rules` — gives `plugdev` users RW on the device.
- `notes/` — reverse-engineering notes against the Windows driver binaries in
  `../mc707/mc707_w1011d_v101DL/`.

## Reverse-engineering reference material

We do not redistribute or modify Roland's signed binaries. They live alongside
this repo at `/home/xecaz/code/mc707/mc707_w1011d_v101DL/` and are read-only
input to Ghidra. The PDB path leaked in `RDWM1207.SYS`
(`...usbdrv8oq\Sys\sysw10\x64\207\RDWM1207Full.pdb`) identifies Roland's 8th-gen
shared USB driver framework — protocol details discovered here are expected to
generalize to other AIRA/Boutique/Studio-Capture devices using `usbdrv8*`.
