# Broader URB-build pattern search: any `*(uint32_t *)X = imm32` where the
# imm32 looks like a URB header (low16 = plausible length, high16 = plausible URB
# function), and the surrounding function calls one of the URB submit helpers.
# @category MC707

from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor
import re

monitor = ConsoleTaskMonitor()
program = currentProgram
ifc = DecompInterface()
ifc.openProgram(program)

URB_FUNC_NAMES = {
    0x00: "SELECT_CONFIGURATION", 0x01: "SELECT_INTERFACE", 0x02: "ABORT_PIPE",
    0x08: "GET_FRAME_LENGTH", 0x09: "BULK_OR_INTERRUPT_TRANSFER",
    0x0a: "ISOCH_TRANSFER", 0x0b: "GET_DESCRIPTOR_FROM_DEVICE",
    0x0c: "SET_DESCRIPTOR_TO_DEVICE", 0x17: "VENDOR_DEVICE",
    0x18: "VENDOR_INTERFACE", 0x19: "VENDOR_ENDPOINT", 0x1a: "CLASS_DEVICE",
    0x1b: "CLASS_INTERFACE", 0x1c: "CLASS_ENDPOINT", 0x1d: "RESET_PIPE",
    0x1e: "RESET_PIPE", 0x1f: "CLASS_OTHER", 0x20: "VENDOR_OTHER",
    0x21: "GET_STATUS_FROM_OTHER", 0x22: "SET_FEATURE_TO_OTHER",
    0x28: "GET_DESCRIPTOR_FROM_INTERFACE", 0x29: "SET_DESCRIPTOR_TO_INTERFACE",
    0x2a: "GET_MS_FEATURE_DESCRIPTOR", 0x30: "SYNC_RESET_PIPE_AND_CLEAR_STALL",
    0x31: "CONTROL_TRANSFER", 0x32: "CONTROL_TRANSFER_EX",
}

# Catch ALL u32 const stores
WRITE_RE = re.compile(r"\*\(undefined4 \*\)\s*([A-Za-z_0-9\(\)\+\s]*?\w)\s*=\s*0x([0-9a-fA-F]+);")
SUBMIT_RE = re.compile(r"FUN_(14000e000|14001dd00)\(")

# Helper: is this dword plausibly a URB header?
def is_urb_header(dword):
    length = dword & 0xffff
    func = (dword >> 16) & 0xffff
    if length < 0x10 or length > 0x200:
        return None
    if func not in URB_FUNC_NAMES:
        return None
    return (length, func)

fm = program.getFunctionManager()
buildsites = []
for fn in fm.getFunctions(True):
    res = ifc.decompileFunction(fn, 60, monitor)
    if not (res and res.getDecompiledFunction()):
        continue
    c = res.getDecompiledFunction().getC()
    if not SUBMIT_RE.search(c):
        continue
    for m in WRITE_RE.finditer(c):
        try:
            v = int(m.group(2), 16)
        except ValueError:
            continue
        urb = is_urb_header(v)
        if urb:
            buildsites.append((fn.getEntryPoint(), fn.getName(), v, urb[0], urb[1]))

print(f"URB-shaped writes found: {len(buildsites)}")
print(f"  {'fn VA':<14s}  {'name':<16s}  hdr-dword   len  function")
for ep, name, dword, length, func in buildsites:
    print(f"  {ep}  {name:<16s}  0x{dword:08x}  0x{length:03x}  0x{func:02x} {URB_FUNC_NAMES[func]}")

# unique functions seen
fns = sorted(set((ep, name) for ep, name, _, _, _ in buildsites))
print(f"\nDistinct functions containing URB builds: {len(fns)}")
for ep, name in fns:
    print(f"  {ep}  {name}")

print("\n[DONE]")
