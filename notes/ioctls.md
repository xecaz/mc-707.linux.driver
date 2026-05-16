# DeviceIoControl surface — RDAS1207.DLL → RDWM1207.SYS

## Status

Day-1 static survey only. Deep IOCTL mapping requires Ghidra analysis
(see "Next steps" below).

## Why this binary first

`RDAS1207.DLL` is the smallest user-mode binary (158 KB) and is **not
stripped** — C++ class method names like `AsioOurs::*` and `CDevOne::*`
are intact in the binary, embedded inside debug-printf format strings.
That makes Ghidra's auto-naming pass land on near-original function
names with no manual work. This is the cheapest entry point into the
protocol.

Confirmed via `strings -n 6 RDAS1207.DLL`:

```
AsioOurs::canSampleRate(%f [%d])... %s
AsioOurs::createBuffers() : ch=%d, size=%d... %s
AsioOurs::disposeBuffers()... %s
AsioOurs::getBufferSize() : min=%d, max=%d, prefer=%d, granu=%d [%s]
AsioOurs::getChannelInfo()... %s
AsioOurs::getChannels()... %s
AsioOurs::getClockSources()... %s
AsioOurs::getLatencies()... %s
AsioOurs::getSampleRate(%3.1f)... %s
AsioOurs::setClockSource(%d)... %s
AsioOurs::start() %s
AsioOurs::stop()... %s
>>CDevOne::CreateBuffer %s: max %d(%d samples), req %d(%d samples), chs %d
```

Imports include `DeviceIoControl`, `CreateFileA/W`, `CloseHandle`,
`FlushFileBuffers` (all `KERNEL32.dll`). Standard Win32 driver IPC pattern.

The error string `"Can not found a device. Please connect the device."`
[sic] is a useful Ghidra anchor — its xrefs lead to the device-probe
function that opens a handle to the SYS driver via `CreateFileW(L"\\.\..."")`.

## What we already know (from descriptors + ASIO method names)

The ASIO method `setClockSource(%d)` confirms the device supports
**multiple clock sources** even though the USB descriptors show only a
single sample rate (44.1 kHz) alt-setting. Implication: clock source and
sample rate are negotiated via **vendor control transfers**, not just by
selecting USB alt-settings. We will need to RE which DeviceIoControl
code translates to which vendor control URB.

The `getLatencies()` and `getBufferSize(min, max, prefer, granu)` methods
hint at the buffer-size constraints the device actually accepts. These
constants live in `.rdata`/`.data` as 32-bit ints — Ghidra will surface
them once the binary is loaded.

## What's NOT in the strings

- Sample-rate literals (44100, 48000, etc.) — stored as 64-bit doubles
  in `.rdata`. Find via Ghidra "Search → Memory → Float" once loaded.
- IOCTL codes (CTL_CODE-encoded 32-bit constants) — stored as immediates
  in `mov` instructions before `DeviceIoControl` calls. Standard pattern
  to recover: `Search → Defined Strings → "DeviceIoControl"` →
  `References to → DeviceIoControl` → for each xref, walk back to the
  preceding immediate load into RDX (the second argument).

## Negative findings on other binaries

- `RDDP1207.EXE` (userspace daemon, 3 MB): no `ClassName::method`-style
  debug strings found. Likely a different code-style (C, not C++) or
  shipped with logging disabled. Less promising for quick static RE.
  Revisit during M2 when we need the init-handshake call site.
- `RDAW1207.DLL`: 32-bit WOW64 shim. Ignored.

## Next steps (deferred to first Ghidra session)

1. Load `RDAS1207.DLL` into a new Ghidra project. Auto-analyze with
   default settings; let the C++ class-name reconstruction pass run.
2. Find xrefs to `DeviceIoControl`. For each, identify the IOCTL code
   constant and the input/output buffer struct.
3. Cross-tabulate each `AsioOurs::*` method against the IOCTLs it issues.
   Build the table below.
4. **Try Microsoft's public symbol server with each binary's PDB GUID
   first** (Ghidra menu: "File → Configure → Path → Symbol Path"). If
   Roland accidentally published PDBs, this collapses days of work into
   minutes.
5. Once IOCTLs are mapped on the user-mode side, repeat on
   `RDWM1207.SYS` to map IOCTL → URB construction.

## IOCTL map (to be filled in)

| IOCTL code (hex) | Caller (`AsioOurs::*`) | Input buffer | Output buffer | Sends URB? | Notes |
|---|---|---|---|---|---|
| `0x00000000` | _placeholder_ | | | | |
