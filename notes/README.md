# Reverse-engineering notes

Working notes against the Windows driver binaries in
`/home/xecaz/code/mc707/mc707_w1011d_v101DL/files/64bit/Files/`.

## Layout

- `usb-descriptors.md` — facts about the live MC-707's USB descriptors
  captured from `lsusb -v`. Already populated.
- `ioctls.md` — DeviceIoControl codes between user-mode (`RDAS1207.DLL`,
  `RDDP1207.EXE`) and the kernel driver (`RDWM1207.SYS`). To be filled
  in via Ghidra during M1/M2 prep.
- `midi-protocol.md` — Roland's vendor USB-MIDI framing on interface 3.
  To be filled in during M1.
- `audio-init.md` — vendor control transfers and alt-setting sequence
  needed before isochronous streaming. To be filled in during M2.
- `audio-format.md` — exact 24-bit packing layout on EPs 0x0D and 0x8E,
  implicit-feedback semantics. To be filled in during M3.

## Working method

Static reverse-engineering only (per user decision 2026-05-16). Tools:

- **Ghidra 12.1** at `~/.local/opt/ghidra_12.1_PUBLIC/`.
- Primary target: `RDAS1207.DLL` (smallest user-mode binary; cleanest
  IOCTL surface) and `RDWM1207.SYS` (the actual driver).
- The PDB path leaked in `RDWM1207.SYS` is
  `Z:\gitroot\VS2019Win10Build\usbdrv8oq\Sys\sysw10\x64\207\RDWM1207Full.pdb`
  — try Microsoft's symbol server with the matching PDB GUID before doing
  manual function naming; on the off chance Roland published symbols,
  this saves days.

If M2 stalls for 3+ sessions on the same blocker, captures become the
cheapest unblock — but don't suggest that pre-emptively.
