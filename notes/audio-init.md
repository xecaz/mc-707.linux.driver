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

### Per-IOCTL buffer size (statically recoverable)

Roland uses `METHOD_BUFFERED` with **`in_size == out_size`** at almost every
call site — request/reply with the same struct, input is overwritten by
the response. Sizes were recovered by walking back ≤120 bytes from each
`DeviceIoControl` call and matching:

- `41 B9 imm32` → `mov r9d, imm32` (nInBufferSize)
- `45 33 C9`    → `xor r9d, r9d`   (nInBufferSize = 0; null input buffer)
- `C7 44 24 28 imm32` → `mov dword ptr [rsp+0x28], imm32` (nOutBufferSize)

When the immediate-load idiom isn't used (size computed at runtime), `???`
is shown.

| IOCTL        | in / out (B) | First-guess role |
|--------------|--------------|------------------|
| `0x0022203c` | 2056 / 2056  | **largest** — firmware blob? device config block? |
| `0x00222040` |    8 /    8  | pair of u32s |
| `0x00222044` |    0 /    4  | no-input query → u32 result |
| `0x00222048` |    4 /    4  | u32 in/out (toggle / set-mode?) |
| `0x0022211c` |   24 /   24  | **stream start** (called by `AsioOurs::start`) |
| `0x00222120` |   12 /   12  | three u32s |
| `0x00222124` |    0 / ???   | no-input query (4 separate call sites) |
| `0x00222128` |   12 /   12  | three u32s — reached by getLatencies, setSampleRate |
| `0x00222130` |   16 /   16  | **4-method common-query** — createBuffers, getBufferSize, getLatencies, setSampleRate |
| `0x00222138` |  ??? /    8  | pair of u32s |
| `0x0022213c` |   16 /   16  | four u32s |
| `0x00222140` |   32 /   32  | **CDevOne::CreateBuffer per-buffer alloc** |
| `0x00222150` |  ??? /   48  | larger config block |
| `0x00222154` |   48 /   48  | larger config block (called 4× — common operation) |
| `0x00222158` |    8 /    8  | pair of u32s |
| `0x0022215c` |   32 /   32  | eight u32s — used by setSampleRate |
| `0x002221e8` |  340 /  340  | **most-called IOCTL (×5)** — full device state struct? |
| `0x00222320` |    8 /    8  | pair of u32s |
| `0x00222324` |    8 /    8  | called 2× — used by setSampleRate |
| `0x00222348` |    4 /    4  | u32 — used by setSampleRate |
| `0x002223e8` |   12 /   12  | three u32s — used by setSampleRate (×3 sites) |
| `0x002223ec` |   12 /   12  | **canSampleRate probe** — `{rate, clock_src, ?}` |
| `0x002223f0` |  ??? /    8  | pair of u32s |
| `0x002223f4` |  ??? /    8  | pair of u32s — used by setSampleRate |

The 340-byte payload of `0x002221e8` is intriguing. 340 = `0x154` — not
a clean multiple of the sample-rate-table entry size (28 B), but
9×28+88 = 340 (i.e., 9 rate entries + an 88-byte header). Plausibly
"give me the full device capability dump including supported rates"
returned as a parallel of the sample-rate table we found in `.data`.
RE the SYS side to confirm.

### IOCTL → ASIO method (call-graph BFS, direct calls only)

By BFS from each named C++ entry point (15 found, only `AsioOurs::*` and
one `CDevOne::CreateBuffer`) through `call rel32` / `jmp rel32` edges:

| IOCTL        | Reached from |
|--------------|--------------|
| `0x0022211c` | `AsioOurs::start` |
| `0x00222128` | `AsioOurs::getLatencies`, `AsioOurs::setSampleRate` |
| `0x00222130` | `AsioOurs::createBuffers`, `AsioOurs::getBufferSize`, `AsioOurs::getLatencies`, `AsioOurs::setSampleRate` |
| `0x00222140` | `CDevOne::CreateBuffer` |
| `0x0022215c` | `AsioOurs::setSampleRate` |
| `0x00222324` | `AsioOurs::setSampleRate` |
| `0x00222348` | `AsioOurs::setSampleRate` |
| `0x002223e8` | `AsioOurs::setSampleRate` |
| `0x002223ec` | `AsioOurs::canSampleRate`, `AsioOurs::setSampleRate` |
| `0x002223f4` | `AsioOurs::setSampleRate` |

Observations:

- **`AsioOurs::setSampleRate` is the heaviest method** — touches 8 IOCTLs
  (`128, 130, 15c, 324, 348, 3e8, 3ec, 3f4`). That's likely the full
  clock-change sequence: validate → quiesce → set → confirm.
- **`0x002223ec`** is the "is-rate-supported" probe — called from both
  `canSampleRate` (obviously) and `setSampleRate` (validates first).
- **`0x0022211c` is "start streaming"** — exclusive to `AsioOurs::start`.
- **`0x00222140` is "create one buffer"** — exclusive to `CDevOne::CreateBuffer`.
  Plural-`createBuffers` (the ASIO entry) uses a different IOCTL
  (`0x00222130`); likely a setup vs. per-buffer split.
- **14 IOCTLs are not reached** from any named ASIO method via direct
  calls: `0x0022203c, 0x00222040, 0x00222044, 0x00222048, 0x0022211c (also reached above), 0x00222120, 0x00222124, 0x00222138, 0x0022213c, 0x00222150, 0x00222154, 0x00222158, 0x002221e8, 0x00222320, 0x002223f0`.
  These are almost certainly reached through:
  1. **Vtable-indirect calls** (`call [reg+offset]`) which our scanner
     doesn't trace. The device-side object (`CDevOne`) has a vtable;
     methods like `setClockSource`, `disposeBuffers`, `getClockSources`
     do not reach any IOCTL in our BFS, which is impossible — they
     must go through the vtable.
  2. **Constructor / destructor / init paths** (no log string).
  3. **Background poller / audio thread** issuing
     `0x002221e8` × 5 (the most-called IOCTL) — consistent with a
     status-poll loop.
  Mapping these requires vtable analysis (Ghidra-friendly, CLI-painful).
- **Methods that show "no DeviceIoControl reached" in the BFS but
  clearly need to talk to the device**: `setClockSource`,
  `getClockSources`, `getSampleRate`, `getChannelInfo`, `getChannels`,
  `disposeBuffers`, `stop`. All of these almost certainly dispatch via
  vtable.

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

## RDWM1207.SYS surface (kernel driver) — initial pass

The 24 user-mode IOCTLs recovered from `RDAS1207.DLL` are issued *into*
the SYS driver, which then constructs URBs and forwards them to the
parent USB bus driver. Initial CLI-only survey:

**Imports**: `ntoskrnl.exe` (58 symbols — standard WDM:
`IoAllocateIrp`, `IoBuildDeviceIoControlRequest`, `IofCallDriver`,
`MmAllocateContiguousMemory`, `KeWaitForSingleObject`,
`PsCreateSystemThread`, etc.) + `USBD.SYS` (just 2 symbols:
`USBD_CreateConfigurationRequest`, `USBD_ParseConfigurationDescriptor`).

**URB submission sites** (4 total, found by searching for the constant
`IOCTL_INTERNAL_USB_SUBMIT_URB = 0x00220003`):

| Helper fn VA       | Size | Direct callers | L2 ancestors (likely IOCTL handlers) |
|--------------------|-----:|---------------:|--------------------------------------|
| `0x14000e000`      |  186 B | 7 | 9 — likely the shared `SendVendorControl(bRequest, wValue, wIndex, buf, len)` |
| `0x14001dd00`      |  128 B | 3 | 3 — secondary submit path |
| `0x1400159e6`      |  432 B | 0 (called via fn-pointer) | — |
| `0x140015bc8`      |  610 B | 0 (called via fn-pointer) | — |

Two of the four are not called directly — they live in callback /
function-pointer tables (consistent with the WDM completion-routine
pattern, or with vtable dispatch). The other two have small,
tractable caller graphs.

**IOCTL constants in SYS**: only **4 of 24** appear as 4-byte
immediates in `.text` (`0x002221e8`, `0x00222320`, `0x002223e8`,
`0x002223ec`). The other 20 are not present anywhere as raw 4-byte
constants. Most likely the dispatcher is compiled as a jump-table
switch where MSVC emits only the BASE constant and a table of small
deltas. Confirming this requires disassembly inspection — Ghidra
will reconstruct the switch statement automatically.

## RDWM1207.SYS internals — Ghidra decompile pass

Headless Ghidra (12.0.3 + PyGhidra) revealed the SYS architecture.

### The driver is PortClass-based, not a raw USB function driver

`DriverEntry` (real one at VA `0x140013890`, called from PE entry
`0x140061000`) calls **`PcInitializeAdapterDriver`** — that's
`PortCls.sys` (Microsoft's audio driver framework). Implications:

- IRP dispatching uses PortCls's framework, not raw `MajorFunction[]`.
- Stream IRPs go through `PcDispatchIrp` (visible in the unified MJ
  handler).
- The MC-707's audio side is exposed as a KS (Kernel Streaming)
  device, not a custom USB device. Roland still issues vendor control
  transfers via URBs, but the Windows-side application path is
  PortCls / KS — different from how our Linux ALSA driver works
  (we're more like a USB-class function driver). The protocol bytes
  on the wire should still be the same regardless.

A side-find: the driver hard-codes `_DAT_14005e2dc = 0xac44 = 44100`
during `DriverEntry`. So the device default is 44.1 kHz — matches the
USB descriptor's advertised rate.

### Unified MajorFunction handler: `FUN_140013b50`

Assigned to MajorFunctions for CREATE, CLOSE, DEVICE_CONTROL, POWER,
PNP — all routed to the same function. Inside, the first byte of the
`IO_STACK_LOCATION` (the MajorFunction code) is read and dispatched:

- `0x0e` (`IRP_MJ_DEVICE_CONTROL`) → **`FUN_140005a60`** ← the IOCTL dispatcher
- `0x16` (`IRP_MJ_POWER`) → state transitions + `PcDispatchIrp`
- `0x1b` (`IRP_MJ_PNP`) → PnP minor handling + `PcDispatchIrp`
- `0x00` (`IRP_MJ_CREATE`) → ref-count increment
- `0x02` (`IRP_MJ_CLOSE`) → `FUN_140012650`

For `IRP_MJ_DEVICE_CONTROL`, it acquires a wait-mutex
(`KeWaitForSingleObject(lVar2 + 0x4a39a0, ...)`) before calling
`FUN_140005a60`, then signals it. So **IOCTL handling is serialized
device-wide** — no concurrent IOCTLs in flight.

### IOCTL dispatcher: `FUN_140005a60`

A clean three-way switch over the `IoControlCode`. Internal cases
group the 24 user-visible IOCTLs (plus ~13 more we hadn't seen from
the DLL — probably for other tools or sibling products in the
`usbdrv8oq` family) into three sub-handlers:

| Sub-handler | IOCTL count | Role guess | Cases (full set) |
|-------------|------------:|-----------|------------------|
| `FUN_14000e210` (caller ctx = `lVar2+0x30`) | 13 | **Queries** (read state) | `0x222034, 0x222038, 0x22203c, 0x222040, 0x222044, 0x222048, 0x22204c, 0x2221e8, 0x2221ec, 0x2221f8, 0x222640, 0x222644, 0x222648` |
| `FUN_1400169b0` (caller ctx = `lVar2+0xad50`) | 22 | **Control surface** (state mutations) | `0x222134, 0x22211c, 0x222120, 0x222124, 0x222128, 0x22212c, 0x222130, 0x222138, 0x22213c, 0x222140, 0x222144, 0x222150, 0x222154, 0x222158, 0x22215c, 0x222160, 0x222167, 0x2223e8, 0x2223ec, 0x2223f0, 0x2223f4` |
| `FUN_14001fb20` (caller ctx = `lVar2+0xad50`) | 9 | **Stream / PortClass-routed** | `0x222050, 0x222320, 0x222324, 0x222328, 0x22232c, 0x222340, 0x222344, 0x222348` |

So the IOCTL → handler mapping is now resolved. Cross-referenced
with what we know from the DLL side: `setSampleRate`'s 8 IOCTLs
(`128, 130, 15c, 324, 348, 3e8, 3ec, 3f4`) split between
`FUN_1400169b0` (the four `3e8..3f4` + `128`, `130`, `15c`) and
`FUN_14001fb20` (`324, 348`). Confirms our guess that setSampleRate
does both a control-surface mutation AND a PortClass-coordinated
stream rate change.

### URB submission: `FUN_14000e000` (sync) and `FUN_14001dd00` (async)

The decompile makes the pattern explicit. The **sync** submit:

```c
ulonglong urb_submit_sync(DEVICE_EXT *ext, URB *urb) {
    KEVENT done; KeInitializeEvent(&done, NotificationEvent, FALSE);
    IO_STATUS_BLOCK iosb;
    PIRP irp = IoBuildDeviceIoControlRequest(
        IOCTL_INTERNAL_USB_SUBMIT_URB,    // 0x00220003
        ext->parent_usb_DO,                // [ext+0x180]
        NULL, 0, NULL, 0,                  // no buffers
        TRUE,                              // InternalDeviceIoControl
        &done, &iosb);
    irp->Tail.Overlay.CurrentStackLocation->Parameters.Others.Argument1 = urb;
    NTSTATUS s = IofCallDriver(ext->parent_usb_DO, irp);
    if (s == STATUS_PENDING) {
        KeWaitForSingleObject(&done, Executive, KernelMode, FALSE, NULL);
        s = iosb.Status;
    }
    return s;
}
```

The **async** version pre-allocates the IRP (`IoReuseIrp`), sets up
a completion routine (`FUN_1400163b0`), and returns immediately.

**Critical: the URB itself is built by the CALLERS, not in these
helpers.** So the SetupPacket bytes (bRequestType, bRequest, wValue,
wIndex) live one layer up — in the functions that call these helpers.
That's the next decompile target.

## Open questions for the next RE pass

1. ✅ **Partial**: 10 of 24 IOCTLs mapped to ASIO methods via direct-call
   BFS (see table above). The remaining 14 are reached through
   vtable-indirect calls (`call qword ptr [reg+offset]`) which our
   CLI scanner cannot resolve. Ghidra handles vtables natively.
2. **Recover the input/output-buffer struct** for the mapped IOCTLs.
   At each `DeviceIoControl` call site:
   - `R8` holds `lpInBuffer` (loaded via `lea r8, [...]`)
   - `R9d` holds `nInBufferSize` (`mov r9d, imm`)
   - `[rsp+0x20]` holds `lpOutBuffer`, `[rsp+0x28]` holds `nOutBufferSize`
   These are statically discoverable by walking back ~120 bytes from
   each call. Highest value: `0x002223ec` (canSampleRate test) — its
   in-buffer struct probably holds `{rate, clock_source}` or similar.
3. **Identify the AsioOurs / CDevOne vtable.** Likely at the start of
   `.data` (we already saw GUID + function pointers at `.data+0x00`).
   Resolving the vtable would close the remaining 14 IOCTLs.
4. ✅ **Partial**: SYS surveyed. 4 URB submission sites found.
   Outstanding pieces (all Ghidra-only from here):
   - Map each IOCTL → URB helper. The L2-caller sets we have (9 + 3
     functions) are candidates for the per-IOCTL handlers; need to
     decompile each to see which IOCTL it dispatches and what
     SetupPacket it builds.
   - Recover the IOCTL dispatcher itself (jump-table switch).
   - URB initialization is opaque to byte-pattern matching — Roland
     probably memcpy's from a static URB template in `.rdata` or
     `.data`. Need to see the static template content and what
     bRequest/wValue/wIndex bytes appear inside it.
   - Then, finally, **the actual USB SetupPacket bytes** for the
     critical operations (`canSampleRate` probe, `setSampleRate`,
     `setClockSource`, `start`, `stop`) — those are what our Linux
     driver needs to emit verbatim via `usb_control_msg()`.

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
