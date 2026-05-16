# MC-707 USB descriptors (vendor mode)

Captured 2026-05-16 via `lsusb -v -d 0582:0229` against a live MC-707
running stock firmware in VENDOR USB mode (default).

```
idVendor    0x0582  Roland Corp.
idProduct   0x0229  MC-707
bcdDevice   1.00
bDeviceClass        0xFF (Vendor Specific)
bMaxPacketSize0     64
bNumConfigurations  1
```

The single configuration has 4 interfaces, all `bInterfaceClass = 0xFF`.

## Interface 0 — control / descriptor (no endpoints)

```
bInterfaceNumber    0
bAlternateSetting   0
bNumEndpoints       0
bInterfaceClass     0xFF
bInterfaceSubClass  0
bInterfaceProtocol  0
```

Probably the configuration anchor / where vendor control transfers land.
Has no `0xF1` descriptors of its own.

## Interface 1 — audio OUT (PC → MC-707)

Three alt settings; alt 0 is the spec-required "idle" with no endpoints.

```
alt 0: bNumEndpoints = 0
alt 1: 6 ch × 24-bit × 44.1 kHz, EP 0x0D iso async, packet 160 B
alt 2: 6 ch × 24-bit × 44.1 kHz, EP 0x0D iso async, packet 56 B  ← low-bw fallback
```

Format descriptor on alt 1 / alt 2 (UAC-1 shape, vendor class):
```
07 24 01 01 00 03 00         ← AC_HEADER-like, subtype 0x01
0b 24 02 01 06 04 18 01 44 ac 00   ← FORMAT_TYPE-like, subtype 0x02:
                                       06 = 6 channels
                                       04 = subframe size (4? — needs RE)
                                       18 = bit resolution = 24
                                       01 44 ac 00 = 0x00AC44 = 44100 Hz
06 24 f1 04 16 00            ← Roland vendor descriptor, subtype 0xF1, payload 04 16 00
```

The `0xF1` payload `04 16 00` differs from the MIDI interface's
`02 01 01` — meaning is unknown until RE.

## Interface 2 — audio IN (MC-707 → PC)

```
alt 0: bNumEndpoints = 0
alt 1: 20 ch × 24-bit × 44.1 kHz, EP 0x8E iso async, packet 524 B,
       usage = Implicit feedback Data
alt 2: 2 ch × 24-bit × 44.1 kHz, EP 0x8E iso async, packet 56 B
```

Format descriptor on alt 1:
```
07 24 01 07 00 03 00
0b 24 02 01 14 04 18 01 44 ac 00   ← 0x14 = 20 channels
06 24 f1 04 16 00
```

EP 0x8E is the **implicit-feedback** source for the playback stream on
EP 0x0D. The driver must consume EP 0x8E whenever playback is active
even if userspace isn't reading capture, and use the framing to time
playback URBs. This is the single hardest piece of the project (M3).

## Interface 3 — MIDI

```
alt 0: bulk EP 0x03 OUT, bulk EP 0x84 IN, both 512 B
alt 1: same EPs but as interrupt transfers (alt transport; unused in M1)
```

Class descriptor on alt 0:
```
06 24 f1 02 01 01
```

First hypothesis (to be confirmed by Ghidra during M1): bytes after
`06 24 f1` are `<subtype> <out-ports> <in-ports>` so `02 01 01` =
"1 OUT port, 1 IN port". Subtype `02` may distinguish from the audio
`04` and idle `01` payloads.

## Capability summary

| Channel direction | Channels | Sample rate | Bit depth | Endpoint | Notes |
|---|---|---|---|---|---|
| Output (PC → device) | 6 | 44100 | 24 | 0x0D iso | 24-bit packing TBD (3-byte vs 24-in-32) |
| Input (device → PC)  | 20 | 44100 | 24 | 0x8E iso | Implicit feedback for playback |
| MIDI OUT | 1 port | n/a | n/a | 0x03 bulk | Roland framing |
| MIDI IN  | 1 port | n/a | n/a | 0x84 bulk | Roland framing |

## Unknowns (need RE / future captures)

1. Exact meaning of the `0xF1` descriptor payloads (`04 16 00`, `02 01 01`).
2. 24-bit sample packing: 3-byte packed vs 24-in-32 LSB vs 24-in-32 MSB.
3. The vendor control transfers (if any) needed before isochronous flow.
4. Per-channel routing inside the 20-channel input stream (Roland's
   support article documents the channel→source mapping at the device
   level; need to confirm USB-stream order matches).
5. Whether 48 kHz is supported via a vendor control transfer despite no
   48 kHz alt setting in the descriptors.
