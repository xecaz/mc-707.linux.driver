# Audio init — vendor control transfers & alt-setting sequence

Working notes for M2. Discovered by static analysis of `RDAS1207.DLL`
(the ASIO user-mode DLL); no USB captures, no Ghidra GUI work yet.

## Sample-rate table (`.data + 0x40`, VA `0x180023040`)

The DLL holds a per-rate parameter table that proves the device supports
**far more than the 44.1 kHz advertised in the USB descriptors**. Each
entry is 28 bytes; 9 rates declared (up to 384 kHz, though MC-707 likely
does not implement all — the table appears to be the union supported by
Roland's `usbdrv8oq` framework, which is reused across multiple
products).

| File off | Rate (Hz) | c1   | c2_signed | c3 | c5 |
|---------:|----------:|-----:|----------:|---:|---:|
| 0x20e40  |    32 000 |   90 |       -32 |  3 | 23 |
| 0x20e5c  |    44 100 |   94 |       -44 |  2 | 17 |
| 0x20e78  |    48 000 |   97 |       -48 |  2 | 16 |
| 0x20e94  |    88 200 |  110 |       -88 |  1 | 10 |
| 0x20eb0  |    96 000 |  112 |       -96 |  1 |  9 |
| 0x20ecc  |   176 400 |  139 |      -176 |  1 |  6 |
| 0x20ee8  |   192 000 |  145 |      -192 |  1 |  6 |
| 0x20f04  |   352 800 |  278 |      -352 |  1 |  6 |
| 0x20f20  |   384 000 |  290 |      -384 |  1 |  6 |

(Columns 4 and 6 are always 0 — probably reserved/pad.)

Clean patterns:

- **`c2 = -(rate / 1000)`** as a signed 32-bit int. Looks like a
  per-rate clock-step / divider value sent to the device.
- **`c3`** = 3 for the lowest rate, 2 for ~CD rates, 1 for everything ≥
  88.2 kHz. Looks like a "speed class" or iso-bandwidth selector.
- **`c1`** and **`c5`** vary monotonically with rate but with breaks at
  88.2 kHz and 176.4 kHz — semantics unclear; possibly latency /
  packets-per-frame / packet-stride constants for iso scheduling.

The table is in `.data`, preceded by 64 bytes of what look like
vtable / class-metadata pointers (GUID-like header at `.data` + 0,
plus VAs into `.rdata` and `.text`). Almost certainly the
`AsioOurs` C++ object's vtable preamble.

> **Implication for our driver:** sample rate cannot be set via USB
> alt-setting selection alone (descriptors advertise only 44.1 kHz).
> Sample rate negotiation must go through vendor control transfers,
> using the per-rate parameters here. The `setClockSource(%d)` method
> name from the ASIO surface independently confirms this.

## DeviceIoControl surface (RDAS1207.DLL → RDWM1207.SYS)

43 `DeviceIoControl` call sites in the DLL. All IOCTLs use
`FILE_DEVICE_UNKNOWN (0x0022)`, `METHOD_BUFFERED (0)`,
`FILE_ANY_ACCESS (0)`. The 24 distinct function codes group into bands:

| Band          | Function codes (hex) | Plausible role |
|---------------|----------------------|----------------|
| `0x80f–0x812` | 80f, 810, 811, 812   | Device open / probe / version query |
| `0x847–0x84f` | 847,848,849,84a,84c,84e,84f | Main control surface (sample rate, clock source, channel config) |
| `0x850–0x857` | 850, 854, 855, 856, 857 | Per-stream / buffer setup |
| `0x87a`       | 87a (×5 calls)       | Frequent — likely status poll or "is device ready" |
| `0x8c8–0x8d2` | 8c8, 8c9, 8d2        | Stream start / stop / state transitions |
| `0x8fa–0x8fd` | 8fa, 8fb, 8fc, 8fd   | Late init / extended ops |

Most-called IOCTLs (in descending call-site count):

| IOCTL        | Calls | Likely role |
|--------------|------:|-------------|
| `0x002221e8` |   5×  | status / readiness poll |
| `0x00222130` |   4×  | TBD (frequent enough to be control-plane staple) |
| `0x00222154` |   4×  | TBD |
| `0x002223e8` |   3×  | TBD |
| `0x00222124` |   3×  | TBD |
| ... 19 others, 1–2 calls each ... | |

Full list of distinct IOCTL constants:

```
0x0022203c  0x00222040  0x00222044  0x00222048
0x0022211c  0x00222120  0x00222124  0x00222128
0x00222130  0x00222138  0x0022213c  0x00222140
0x00222150  0x00222154  0x00222158  0x0022215c
0x002221e8  0x00222320  0x00222324  0x00222348
0x002223e8  0x002223ec  0x002223f0  0x002223f4
```

(Call-site VAs and full distribution captured in
`tools/extract_ioctls.py` output — see "Reproducibility" below.)

## Open questions for the next RE pass

1. **Map each IOCTL → C++ method**. The 13 known `AsioOurs::*` methods
   (`canSampleRate`, `setSampleRate`, `getSampleRate`, `getClockSources`,
   `setClockSource`, `getChannels`, `getChannelInfo`, `getLatencies`,
   `getBufferSize`, `createBuffers`, `disposeBuffers`, `start`, `stop`)
   need to be associated with the IOCTLs they issue. Approach:
   - Walk `.pdata` to get function ranges.
   - For each function, find the `lea rcx, [rip+disp]` that loads its
     log format string — that names the function.
   - Cross-tabulate: function range ↔ IOCTLs ↔ method name.
2. **Recover the input-buffer struct** for the IOCTLs that look like
   "set sample rate" and "set clock source". These structs are what
   the kernel driver eventually maps to vendor control transfers.
3. **Reverse the same surface in `RDWM1207.SYS`** to confirm
   IOCTL → URB mapping (the IOCTL is just the user/kernel boundary;
   the actual USB transfer is built inside the SYS driver). Look for
   `URB_FUNCTION_VENDOR_DEVICE` / `URB_FUNCTION_CONTROL_TRANSFER_EX`
   construction.

## Microsoft public symbol server: not useful (confirmed)

Queried for all five MC-707 binaries:

| Binary       | PDB name           | GUID + Age                          | Result |
|--------------|--------------------|-------------------------------------|--------|
| RDAS1207.DLL | RDAS1207Full.pdb   | `42448ED98EDB41CE92DED35847C45F80`+1 | 404 |
| RDWM1207.SYS | RDWM1207Full.pdb   | `7EEC2031521C4DD89553E1BD3DEB5EE0`+1 | 404 |
| RDDP1207.EXE | RDDP1207Full.pdb   | `13D468A70C85492595AC9D2E47A40BBC`+1 | 404 |
| RDAH1207.EXE | **Prop3264Full.pdb** | `62A80F3C457A4EC692DE0CCD5DF257AA`+1 | 404 |
| RDCP1207.CPL | RDCP1207Full.pdb   | `C78878854D3E49E6BCD3AF41F2C8E082`+1 | 404 |

Roland did not publish PDBs (expected). PDB names + GUIDs recorded in
case a leak ever surfaces. Side discovery: **`RDAH1207.EXE` is a thin
wrapper around `Prop3264Full.pdb`** — a generic Roland property-page
DLL shared across products, not MC-707-specific. Safe to deprioritize.

## Reproducibility

The IOCTL extraction was done with a one-off Python script using
`pefile`. To re-run:

```python
import pefile, struct
pe = pefile.PE("RDAS1207.DLL", fast_load=True)
pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_IMPORT']])
# 1. Find DeviceIoControl IAT slot from imports.
# 2. Scan .text for `FF 15 disp32` (call qword ptr [rip+disp]).
# 3. For each call whose target is the IAT slot, walk back 64 bytes
#    looking for `BA imm32` (mov edx, imm32) — that's the IOCTL code.
```

Sample-rate table extraction: read 9 × 28-byte structs starting at
file offset `0x20e40`, unpack as `<i6i`. The rate is the first u32.
