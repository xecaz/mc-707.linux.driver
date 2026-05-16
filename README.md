# snd-roland-mc707

Out-of-tree Linux ALSA driver for the **Roland MC-707** groovebox in its native
vendor-specific USB mode (USB `0582:0229`).

## Goal

Expose, simultaneously and on a stock Linux box:

- **6** audio output channels (PC → MC-707), 24-bit / 44.1 kHz (isochronous EP `0x0D`).
- **20** audio input channels (MC-707 → PC), 24-bit / 44.1 kHz (isochronous EP `0x8E`, implicit-feedback).
- **MIDI** (bulk EP `0x03` OUT / `0x84` IN, Roland's vendor-flavoured USB-MIDI framing).

The in-tree `snd-usb-audio` driver does not bind because all four interfaces
report `bInterfaceClass = 0xFF` (vendor-specific). The MC-707's firmware ≥ 1.60
"GENERIC" class-compliant mode exists but collapses I/O to a single stereo pair,
defeating the point of a groovebox. This driver targets the full vendor mode.

## Status

Work in progress. See the milestone roadmap in
[`/home/xecaz/code/mc707/CLAUDE.md`](../mc707/CLAUDE.md) and the detailed plan
at `~/.claude/plans/in-this-folder-i-composed-blossom.md`.

- M0 — Environment / scaffolding: in progress.
- M1 — MIDI working end-to-end: not started.
- M2 — Audio init sequence (no streaming): not started.
- M3 — Multichannel audio streaming with implicit feedback: not started.

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
