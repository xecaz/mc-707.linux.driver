# Ghidra post-script: decompile the IOCTL sub-handlers + URB-submit callers.
# @category MC707

from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

monitor = ConsoleTaskMonitor()
program = currentProgram
ifc = DecompInterface()
ifc.openProgram(program)

def addr(n):
    return program.getAddressFactory().getDefaultAddressSpace().getAddress(n)

def decomp(addr_int):
    fn = getFunctionAt(addr(addr_int))
    if fn is None:
        return "  [no function at 0x{:x}]".format(addr_int)
    res = ifc.decompileFunction(fn, 180, monitor)
    if res and res.getDecompiledFunction():
        return res.getDecompiledFunction().getC()
    return "  [decompile failed]"

targets = [
    # === 3 IOCTL sub-handlers ===
    (0x14000e210, "*** SUB-HANDLER: queries (13 IOCTLs)"),
    (0x1400169b0, "*** SUB-HANDLER: control surface (22 IOCTLs) + DIRECT URB-submit caller"),
    (0x14001fb20, "*** SUB-HANDLER: stream / PortClass-routed (9 IOCTLs)"),
    # === Direct callers of FUN_14000e000 (sync URB submit) ===
    (0x14000e0f0, "URB-submit caller #1 (278 B)"),
    (0x14000ecf3, "URB-submit caller #2 (240 B)"),
    (0x14000ede3, "URB-submit caller #3 (261 B)"),
    (0x14000f7e0, "URB-submit caller #4 (462 B)"),
    (0x14000ff32, "URB-submit caller #5 (180 B)"),
    (0x1400162d0, "URB-submit caller #6 (166 B)"),
    # 0x1400169b0 already in sub-handler list above
    # === Direct callers of FUN_14001dd00 (async URB submit) ===
    (0x140019a60, "Async URB-submit caller #1 (81 B)"),
    (0x14001a9cb, "Async URB-submit caller #2 (315 B)"),
    (0x14001e2b9, "Async URB-submit caller #3 (372 B)"),
]

for tgt_addr, label in targets:
    print("\n" + "=" * 80)
    print("FN 0x{:x}  — {}".format(tgt_addr, label))
    print("=" * 80)
    print(decomp(tgt_addr))

print("\n[DONE]")
