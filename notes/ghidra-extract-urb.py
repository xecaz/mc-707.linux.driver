# Ghidra post-script: extract URB construction + IOCTL dispatch paths.
# Run after RDWM1207.SYS is auto-analyzed.
# @category MC707

from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor
from ghidra.program.model.symbol import SymbolType

monitor = ConsoleTaskMonitor()
program = currentProgram
listing = program.getListing()
sym_table = program.getSymbolTable()
ref_mgr = program.getReferenceManager()

ifc = DecompInterface()
ifc.openProgram(program)

def hexa(n):
    return "0x{:09x}".format(n)

def addr(n):
    """Make a Ghidra Address from a 64-bit Python int safely."""
    return program.getAddressFactory().getDefaultAddressSpace().getAddress(n)

def decomp(addr_int):
    fn = getFunctionAt(addr(addr_int))
    if fn is None:
        return "  [no function at {}]".format(hexa(addr_int))
    res = ifc.decompileFunction(fn, 120, monitor)
    if res and res.getDecompiledFunction():
        return res.getDecompiledFunction().getC()
    return "  [decompile failed for {}]".format(hexa(addr_int))

print("")
print("=" * 80)
print("PROGRAM:", program.getName())
print("IMAGE BASE:", program.getImageBase())
print("=" * 80)

# Find DriverEntry — it's typically the binary's entry point
ep = program.getSymbolTable().getExternalEntryPointIterator()
ep_addrs = []
while ep.hasNext():
    a = ep.next()
    ep_addrs.append(a)
print("\nEntry points:", [str(a) for a in ep_addrs])

for ep_addr in ep_addrs:
    print("\n" + "=" * 80)
    print("ENTRY POINT @ {}  (likely DriverEntry wrapper)".format(ep_addr))
    print("=" * 80)
    fn = listing.getFunctionContaining(ep_addr) or getFunctionAt(ep_addr)
    if fn:
        res = ifc.decompileFunction(fn, 120, monitor)
        if res and res.getDecompiledFunction():
            print(res.getDecompiledFunction().getC())

# The entry wrapper called FUN_140013890(param_1, param_2) — that's the real
# DriverEntry. Decompile it and look for the IRP_MJ_DEVICE_CONTROL handler.
print("\n" + "=" * 80)
print("REAL DriverEntry @ 0x140013890")
print("=" * 80)
print(decomp(0x140013890))

# Decompile the unified MJ dispatcher + the 4 URB-submit helpers
targets = [
    (0x140013b50, "*** UNIFIED MJ HANDLER (CREATE/CLOSE/DEVICE_CONTROL/POWER/PNP) ***"),
    (0x14000e000, "URB submit helper #1 (busy: 7 callers)"),
    (0x14001dd00, "URB submit helper #2 (3 callers)"),
    (0x1400159e6, "URB submit #3 (fn-ptr only)"),
    (0x140015bc8, "URB submit #4 (fn-ptr only)"),
]
for tgt_addr, label in targets:
    print("\n" + "=" * 80)
    print("FN {}  — {}".format(hexa(tgt_addr), label))
    print("=" * 80)
    print(decomp(tgt_addr))

# Try to find the IOCTL dispatcher: scan all functions for ones that compare
# the rdx-or-edx argument against multiple of our known IOCTL constants.
KNOWN_IOCTLS = {0x0022203c, 0x00222040, 0x00222044, 0x00222048,
                0x0022211c, 0x00222120, 0x00222124, 0x00222128,
                0x00222130, 0x00222138, 0x0022213c, 0x00222140,
                0x00222150, 0x00222154, 0x00222158, 0x0022215c,
                0x002221e8, 0x00222320, 0x00222324, 0x00222348,
                0x002223e8, 0x002223ec, 0x002223f0, 0x002223f4}

print("\n" + "=" * 80)
print("HUNT: functions whose decompiled body contains 2+ known IOCTL constants")
print("(candidate IOCTL dispatcher or sub-dispatchers)")
print("=" * 80)
fn_iter = program.getFunctionManager().getFunctions(True)
candidates = []
i = 0
for fn in fn_iter:
    i += 1
    if i > 5000:
        break  # safety
    res = ifc.decompileFunction(fn, 30, monitor)
    if not (res and res.getDecompiledFunction()):
        continue
    c = res.getDecompiledFunction().getC()
    hits = [ioc for ioc in KNOWN_IOCTLS if "0x{:x}".format(ioc) in c or "0x{:X}".format(ioc) in c]
    if len(hits) >= 2:
        candidates.append((fn.getEntryPoint(), len(hits), fn.getName(), hits))

candidates.sort(key=lambda x: -x[1])
for ep, n, name, hits in candidates[:10]:
    print("\n  fn {}  {}  ({} IOCTL refs)".format(ep, name, n))
    print("    IOCTLs: " + ", ".join("0x{:08x}".format(h) for h in sorted(hits)))

# Dump full decompilation of the top dispatcher candidate
if candidates:
    top = candidates[0]
    print("\n" + "=" * 80)
    print("TOP DISPATCHER CANDIDATE: {}  ({} IOCTL refs)".format(top[0], top[1]))
    print("=" * 80)
    fn = getFunctionAt(top[0])
    res = ifc.decompileFunction(fn, 120, monitor)
    print(res.getDecompiledFunction().getC())

print("\n[DONE]")
