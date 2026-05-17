# snd-roland-mc707

> **Project archived 2026-05-17.** Mainline `snd-usb-audio` already handles
> the Roland MC-707 in vendor USB mode end-to-end on any Linux kernel
> ≥ 3.10 — there was never a need for this driver. The premise that
> motivated the project ("no Linux support exists for the MC-707 in vendor
> mode") was wrong. See **[Conclusion](#conclusion)** below for the
> evidence and the upstream commit responsible.

## Conclusion

Verified 2026-05-17 on bare metal, Debian 13.5, kernel `6.12.88+deb13-amd64`,
with this driver **not** loaded:

- `lsusb -t` shows `snd-usb-audio` bound to all three working interfaces of
  the MC-707 (ifaces 1, 2, 3 — vendor-class, `bInterfaceClass = 0xFF`).
- `/proc/asound/card1/stream0` reports 6-channel playback @ 44.1 kHz on
  EP `0x0D` with implicit feedback from EP `0x8E`, and 20-channel capture
  @ 44.1 kHz on EP `0x8E` — exactly the vendor-mode interface the device
  advertises.
- `arecord -D hw:MC707 -c 20 -r 44100 -f FLOAT_LE -d 10 …` captures real
  signal from the device's 8 stereo per-track outputs while a pattern
  plays. Channel map (verified via `sox -n stats` per channel):
  - **ch 1–2** — EXT IN L/R
  - **ch 3–18** — 8 stereo per-track captures (Track 1 L/R through Track 8 L/R)
  - **ch 19–20** — silent in every condition we tested, function unknown
    (the QEMU-session hypothesis that these are a USB monitor return is
    *wrong* — they stayed at the noise floor with `speaker-test`
    actively pushing 6 channels of USB OUT).
- `speaker-test -D hw:MC707 -c 6 -r 44100 -F FLOAT_LE` is audible through
  MAIN OUT — the isochronous OUT path works, which is the precise spot
  where the AIRA TR-8 community attempt died in 2015.
- `amidi -p hw:1,0,0 -S "90 24 7F"` triggers an audible kick drum.
- Full round-trip — record the device's internal pattern at 20 ch, downmix
  to stereo, expand to 6 ch, replay through MAIN OUT via
  `aplay -D hw:MC707` — works end-to-end with no glitches.

One minor sub-optimality: `hw_params` advertises only `FLOAT_LE`, not
native `S24_3LE`. The wire is still 24-bit; the kernel transparently
converts FLOAT_LE samples from userspace into 24-bit on the iso endpoint.
This costs CPU and rounds-tripping precision but does not affect channel
count, sample rate, or latency for normal use. It is the only thing a
future upstream patch could plausibly improve for this specific device.

### Who actually did the work

```
commit aafe77cc45a595ca1d4536f2412ddf671ea9108c
Author: Clemens Ladisch <clemens@ladisch.de>
Date:   Sun Mar 31 23:43:12 2013 +0200

    ALSA: usb-audio: add support for many Roland/Yamaha devices
```

Clemens Ladisch added a catch-all entry to `sound/usb/quirks-table.h`
matching **any** Roland device (vendor `0x0582`) with **any**
vendor-specific (`0xFF`) interface, with `QUIRK_AUTODETECT` so
`snd-usb-audio` parses the underlying UAC1 descriptors that Roland wraps
inside the vendor-class envelope:

```c
/* this catches most recent vendor-specific Roland devices */
{
    .match_flags = USB_DEVICE_ID_MATCH_VENDOR |
                   USB_DEVICE_ID_MATCH_INT_CLASS,
    .idVendor = 0x0582,
    .bInterfaceClass = USB_CLASS_VENDOR_SPEC,
    QUIRK_DRIVER_INFO {
        .ifnum = QUIRK_ANY_INTERFACE,
        .type = QUIRK_AUTODETECT
    }
},
```

That single entry is what binds `snd-usb-audio` to the MC-707 and to
every other Roland/AIRA/Boutique/Studio-Capture device using the
`usbdrv8*` family. It has been in mainline since **Linux v3.10 (June
2013)** — 13 years before this project was started, and 6 years before
the MC-707 was released. No PID-specific patch was ever required.

### Why this project still got started

A lesson in cheap verification:

1. The user's first attempt to use the MC-707 on Linux was on an older
   kernel where, for unrelated reasons, the device wasn't enumerating
   as a useful card. Rather than diagnosing the actual blocker, the
   project concluded "no Linux support exists" and went straight to
   writing a driver.
2. The custom `snd-roland-mc707` claimed iface 0 (which has no
   endpoints) and could later have been confused for the reason
   `snd-usb-audio` "wasn't binding" — but `snd-usb-audio` was binding
   ifaces 1/2/3 the whole time. The custom driver simply never blocked
   it.
3. The fix that ultimately surfaced this — upgrading from
   `6.12.74+deb13+1` to `6.12.88+deb13` — most likely fixed an
   unrelated bug (Debian config change, an intermediate stable point
   release with a UAC1 parsing fix, or just the user finally testing
   with the custom driver unloaded). The *binding* quirk has been in
   mainline the entire time.

**The single cheapest test that would have killed this project on day
one:** `rmmod snd-roland-mc707 2>/dev/null; lsusb -t | grep -A1 0582:0229`.
If that had shown `snd-usb-audio` bound to the interfaces — which it
would have on any kernel ≥ 3.10 — none of the rest of this repo would
have been written.

### What's still useful here

The reverse-engineering work in `notes/` retains some educational value:

- [`notes/usb-descriptors.md`](notes/usb-descriptors.md) — full
  descriptor breakdown of the MC-707 in vendor mode (live `lsusb -v`).
- [`notes/audio-init.md`](notes/audio-init.md) — Ghidra-assisted RE of
  Roland's Windows driver (`RDWM1207.SYS`, `RDAS1207.DLL`). The
  noteworthy finding — that there is no proprietary audio-init protocol
  at the URB layer, the SYS driver issues exactly *one* vendor control
  transfer in the whole binary and it's daemon-only — is what's
  actually load-bearing for understanding the device. It's also what
  predicted (correctly) that the vendor-class interfaces would parse
  cleanly as UAC1 if anyone tried.
- [`notes/ghidra-extract-*.py`](notes/) — headless Ghidra post-scripts
  (PyGhidra) for reproducing the M2 RE pass against the Windows
  binaries. Generalizes to any device using Roland's `usbdrv8oq`
  framework.

The code under `src/` is a partial Linux ALSA driver that reached
working MIDI (via `snd-usbmidi-lib` and `QUIRK_MIDI_FIXED_ENDPOINT` on
the interrupt-EP alt-setting), working PCM device exposure (6-ch
playback / 20-ch capture stubs that successfully advertised the right
format), and unsuccessful isochronous URB submission (which hard-froze
the host twice; defensive changes in the last commit on this branch
were never validated against hardware). It is preserved as a record of
where the work stopped.

---

# Historical README (preserved verbatim)

> Everything below this line was the active state of the project before
> the conclusion above was reached. Read it as the story of how the
> project arrived at its archived state, not as instructions to follow.

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

*(Both paragraphs of "Goal" are wrong as written — see the Conclusion at the
top. `snd-usb-audio` does bind via the 2013 Roland vendor-class catch-all
quirk; the firmware ≥ 1.60 GENERIC mode is a separate alternative the user
can switch between in the MC-707's system menu.)*

## Status

- **M0 — Environment / scaffolding**: ✅ done.
- **M1 — MIDI working end-to-end**: ✅ done — via
  `__snd_usbmidi_create()` + `QUIRK_MIDI_FIXED_ENDPOINT` over the
  interrupt-EP alt-setting (alt 1 of interface 3).
- **M2 — Audio init sequence (no streaming)**:
  🟢 **RE complete; ALSA PCM stubs implemented**. RE conclusion:
  audio interfaces are UAC1-shaped under the vendor-class wrapper.
  No Roland-specific audio-init protocol exists at the URB layer.
- **M3 — Multichannel audio streaming with implicit feedback**:
  🔴 attempted; iso OUT URB submission hard-froze the host twice
  on bare metal. Defensive changes (no-resubmit-on-error, 1-URB
  throttle, interval=8 instead of 1, USB_SPEED_HIGH guard) applied
  in the final commit on this branch but never validated against
  hardware. Project archived before the QEMU-harness retest could
  exercise them.

See [`/home/xecaz/code/mc707/CLAUDE.md`](../mc707/CLAUDE.md) for the
session-by-session log and the "Hard-won facts" list of upstream-quirk
subtleties that cost the most time.

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

**Caveat (added 2026-05-17):** loading this module is no longer
recommended. It will compete with `snd-usb-audio` for the MC-707's
interfaces and can leave the kernel in a refcount=-1 zombie state if
the iso path is exercised (see CLAUDE.md facts #14, #16, #17, #18). To
use the MC-707 on Linux, just plug it in — `snd-usb-audio` handles it.

## Verifying MIDI works (with this driver loaded)

```
$ amidi -l
Dir Device    Name
IO  hw:1,0,0  Roland MC-707 MIDI 1

amidi -p hw:1,0,0 -S "90 24 7F"   # Note On  ch1, note 36, vel 127 → kick
sleep 0.3
amidi -p hw:1,0,0 -S "80 24 00"   # Note Off
```

The same command works on mainline `snd-usb-audio` with no custom driver
loaded — the device name and port indices may differ.

## Layout

- `src/` — kernel module source.
- `udev/70-roland-mc707.rules` — udev rule granting `plugdev` users RW.
- `notes/` — reverse-engineering write-ups against the Windows driver binaries:
  - `usb-descriptors.md` — full descriptor breakdown (live `lsusb -v` capture).
  - `ioctls.md` — DLL → SYS DeviceIoControl surface (day-1 survey).
  - `audio-init.md` — M2 RE: sample-rate table, IOCTL ↔ ASIO-method mapping, IOCTL buffer sizes, IOCTL dispatcher reconstruction, complete URB inventory, and the UAC1-underneath conclusion.
  - `ghidra-extract-*.py` — headless Ghidra post-scripts (PyGhidra).
  - `session4-post-reboot.md`, `session4-rmmod-oops.txt` — last-session
    field notes before the project was archived.

## Reverse-engineering reference material

We do not redistribute or modify Roland's signed binaries. They live alongside
this repo at `/home/xecaz/code/mc707/mc707_w1011d_v101DL/` and are read-only
input to Ghidra. The PDB path leaked in `RDWM1207.SYS`
(`...usbdrv8oq\Sys\sysw10\x64\207\RDWM1207Full.pdb`) identifies Roland's 8th-gen
shared USB driver framework — protocol details discovered here generalize to
other AIRA/Boutique/Studio-Capture devices using `usbdrv8*`.

### Reproducing the RE pass

The notes folder contains three Python post-scripts for headless Ghidra (12.x +
PyGhidra). To re-run them against the Windows binaries:

```
# 1. Set up PyGhidra in a venv (one-time).
python3 -m venv ~/.venv-ghidra
~/.venv-ghidra/bin/pip install \
    $GHIDRA_HOME/Ghidra/Features/PyGhidra/pypkg/dist/pyghidra-*.whl

# 2. Import + auto-analyze the SYS driver.
$GHIDRA_HOME/support/analyzeHeadless ~/ghidra-projects/mc707 mc707 \
    -import /path/to/RDWM1207.SYS -overwrite -loader PeLoader

# 3. Run an extraction script.
~/.venv-ghidra/bin/python \
    $GHIDRA_HOME/Ghidra/Features/PyGhidra/support/pyghidra_launcher.py \
    $GHIDRA_HOME -H ~/ghidra-projects/mc707 mc707 \
    -process RDWM1207.SYS -noanalysis \
    -scriptPath ./notes -postScript ghidra-extract-urb.py
```
