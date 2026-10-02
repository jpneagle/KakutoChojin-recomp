"""Step 1: list kernel imports and library symbols, with call-site counts from game code.

Outputs analysis/kernel_imports.csv and analysis/library_symbols.csv.
"""
import csv
import struct
from collections import Counter

import os as _os, sys as _sys; _sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))  # embedded Python does not add the script directory
from xbelib import ROOT, Xbe, kernel_names, load_symbols, ANALYSIS

xbe = Xbe()
syms = load_symbols()
knames = kernel_names()
text = xbe.section(".text")
code = xbe.data[text.raw:text.raw + text.rsize]

# Byte-level scan for direct calls/jumps; targets are checked against known
# addresses, so random E8 bytes in data rarely produce a false match.
direct = Counter()
indirect = Counter()
for i in range(len(code) - 5):
    op = code[i]
    if op in (0xE8, 0xE9):
        tgt = text.va + i + 5 + struct.unpack_from("<i", code, i + 1)[0]
        if tgt in syms:
            direct[tgt] += 1
    elif op == 0xFF and code[i + 1] in (0x15, 0x25):
        indirect[struct.unpack_from("<I", code, i + 2)[0]] += 1

out = ANALYSIS
with open(out / "kernel_imports.csv", "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(["thunk_va", "ordinal", "name", "call_sites"])
    for va, ordinal in xbe.kernel_imports():
        w.writerow([f"0x{va:08x}", ordinal, knames.get(ordinal, "?"), indirect[va]])

with open(out / "library_symbols.csv", "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(["va", "section", "lib", "kind", "name", "callconv", "signature", "game_call_sites"])
    for va in sorted(syms):
        lib, kind, name, conv, sig = syms[va]
        sec = xbe.section_of(va)
        w.writerow([f"0x{va:08x}", sec.name if sec else "", lib, kind, name, conv, sig, direct[va]])

kimps = xbe.kernel_imports()
print(f"kernel imports: {len(kimps)}  (unknown names: {sum(1 for _, o in kimps if o not in knames)})")
print(f"library symbols: {len(syms)}  called from .text: {sum(1 for v in syms if direct[v])}")
print("most-called library functions:")
for va, n in direct.most_common(15):
    print(f"  {n:5d}  {syms[va][0]:8s} {syms[va][2]}")
