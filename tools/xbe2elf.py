"""Convert an XBE into an ELF32 executable carrying every recovered symbol.

Any disassembler/decompiler (Ghidra, IDA, Binary Ninja, rizin...) loads the
result natively, with sections at their XBE addresses and names applied for:
  * kernel imports      -> __imp__<Export> on each thunk slot
  * XDK library code    -> functions/variables found by XbSymbolDatabase
  * C++ RTTI            -> <Class>::vftable, <Class>::`RTTI Type Descriptor',
                           and <Class>::vf<N> for virtual methods

usage: xbe2elf.py <default.xbe> <out.elf> [--symbols symbols.txt] [--rtti rtti_classes.json]
"""
import argparse
import json
import struct

import os as _os, sys as _sys; _sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))  # embedded Python does not add the script directory
from xbelib import Xbe, kernel_names, load_symbols

SHF_WRITE, SHF_ALLOC, SHF_EXECINSTR = 1, 2, 4
STB_LOCAL, STB_GLOBAL = 0, 1
STT_OBJECT, STT_FUNC = 1, 2

# Kernel exports become stub functions in a synthetic section, and the thunk
# table points at them, so calls through it decompile as direct named calls.
KERNEL_BASE = 0x80000000


def kernel_stub(ordinal):
    return KERNEL_BASE + 16 * ordinal


class KernelSection:
    name, flags, va = ".xboxkrnl", 4, KERNEL_BASE


class StrTab:
    def __init__(self):
        self.data = bytearray(b"\0")
        self.index = {}

    def add(self, s):
        if s not in self.index:
            self.index[s] = len(self.data)
            self.data += s.encode("utf-8", "replace") + b"\0"
        return self.index[s]


def collect_symbols(xbe, symbols_path, rtti_path):
    """{va: (name, size, type)}; earlier sources win."""
    syms = {}

    def add(va, name, size, typ):
        if va and va not in syms:
            syms[va] = (name, size, typ)

    names = kernel_names()
    for va, ordinal in xbe.kernel_imports():
        name = names.get(ordinal, f"xboxkrnl_{ordinal}")
        add(va, "__imp__" + name, 4, STT_OBJECT)
        add(kernel_stub(ordinal), name, 16, STT_FUNC)

    if symbols_path:
        for va, (lib, kind, name, conv, sig) in load_symbols(symbols_path).items():
            add(va, name, 4 if kind == "VAR" else 0, STT_OBJECT if kind == "VAR" else STT_FUNC)

    if rtti_path:
        classes = json.load(open(rtti_path))
        # A virtual function is named after the most basic class whose
        # vtable contains it (where it was most likely introduced).
        owner = {}
        for c in classes:
            depth = len(c.get("all_bases", []))
            for vt in c.get("vtables", []):
                for i, f in enumerate(vt["methods"]):
                    f = int(f, 16)
                    if f not in owner or depth < owner[f][0]:
                        owner[f] = (depth, c["name"], i)
        for c in classes:
            for k, vt in enumerate(c.get("vtables", [])):
                suffix = "" if vt.get("this_offset", 0) == 0 else f"{{for +{vt['this_offset']:#x}}}"
                add(int(vt["va"], 16), f"{c['name']}::vftable{suffix}", 4 * len(vt["methods"]), STT_OBJECT)
        for f, (_, cls, i) in owner.items():
            add(f, f"{cls}::vf{i}", 0, STT_FUNC)
    return syms


def build(xbe, syms):
    secs = [s for s in xbe.sections]
    shstr, strtab = StrTab(), StrTab()
    # File layout: ELF header, program headers, XBE header blob, section data,
    # symbol/string tables, section headers.
    phnum = len(secs) + 2  # + XBE header + kernel stubs
    off = 52 + 32 * phnum
    blobs = []

    hdr = xbe.data[:xbe.hdr_size]
    hdr_off = off
    blobs.append(hdr)
    off += len(hdr)

    imports = xbe.kernel_imports()
    placed = []
    for s in secs:
        off = (off + 15) & ~15
        data = bytearray(xbe.data[s.raw:s.raw + s.rsize] + b"\0" * max(0, s.vsize - s.rsize))
        for va, ordinal in imports:  # point thunk slots at the kernel stubs
            if s.va <= va < s.va + len(data):
                struct.pack_into("<I", data, va - s.va, kernel_stub(ordinal))
        placed.append((s, off, bytes(data)))
        blobs.append((off, bytes(data)))
        off += len(data)

    stubs = bytearray(b"\xcc" * (16 * 380))
    for _, ordinal in imports:
        stubs[16 * ordinal] = 0xC3  # ret; real signatures come from KtApply
    off = (off + 15) & ~15
    placed.append((KernelSection, off, bytes(stubs)))
    blobs.append((off, bytes(stubs)))
    off += len(stubs)

    # Section headers: null, .xbeh, XBE sections, .symtab, .strtab, .shstrtab
    shdrs = [(0, 0, 0, 0, 0, 0, 0, 0, 0, 0)]
    shdrs.append((shstr.add(".xbeh"), 1, SHF_ALLOC, xbe.base, hdr_off, len(hdr), 0, 0, 4, 0))
    sec_index = {}
    for s, o, data in placed:
        flags = SHF_ALLOC | (SHF_WRITE if s.flags & 1 else 0) | (SHF_EXECINSTR if s.flags & 4 else 0)
        sec_index[s.name] = len(shdrs)
        shdrs.append((shstr.add(s.name), 1, flags, s.va, o, len(data), 0, 0, 16, 0))

    def shndx(va):
        for s, o, data in placed:
            if s.va <= va < s.va + len(data):
                return sec_index[s.name]
        return 1 if xbe.base <= va < xbe.base + len(hdr) else 0xFFF1  # SHN_ABS

    symtab = bytearray(16)  # null symbol
    for va in sorted(syms):
        name, size, typ = syms[va]
        symtab += struct.pack("<IIIBBH", strtab.add(name), va, size, (STB_GLOBAL << 4) | typ, 0, shndx(va))
    off = (off + 3) & ~3
    symtab_off = off
    off += len(symtab)
    strtab_off = off
    off += len(strtab.data)
    symtab_idx = len(shdrs)
    shdrs.append((shstr.add(".symtab"), 2, 0, 0, symtab_off, len(symtab), symtab_idx + 1, 1, 4, 16))
    shdrs.append((shstr.add(".strtab"), 3, 0, 0, strtab_off, len(strtab.data), 0, 0, 1, 0))
    shstr_name = shstr.add(".shstrtab")
    shstr_off = off + 0  # computed below after adding its own name
    shdrs.append((shstr_name, 3, 0, 0, 0, 0, 0, 0, 1, 0))
    shstr_off = off
    off += len(shstr.data)
    shdrs[-1] = (shstr_name, 3, 0, 0, shstr_off, len(shstr.data), 0, 0, 1, 0)
    off = (off + 3) & ~3
    shoff = off

    out = bytearray(off + 40 * len(shdrs))
    ident = b"\x7fELF" + bytes([1, 1, 1, 0]) + b"\0" * 8
    out[0:52] = ident + struct.pack("<HHIIIIIHHHHHH", 2, 3, 1, xbe.entry, 52, shoff, 0, 52, 32, phnum, 40,
                                    len(shdrs), len(shdrs) - 1)
    ph = 52
    out[ph:ph + 32] = struct.pack("<IIIIIIII", 1, hdr_off, xbe.base, xbe.base, len(hdr), len(hdr), 4, 4)
    ph += 32
    for s, o, data in placed:
        pflags = 4 | (2 if s.flags & 1 else 0) | (1 if s.flags & 4 else 0)
        out[ph:ph + 32] = struct.pack("<IIIIIIII", 1, o, s.va, s.va, len(data), len(data), pflags, 16)
        ph += 32
    out[hdr_off:hdr_off + len(hdr)] = hdr
    for o, data in blobs[1:]:
        out[o:o + len(data)] = data
    out[symtab_off:symtab_off + len(symtab)] = symtab
    out[strtab_off:strtab_off + len(strtab.data)] = strtab.data
    out[shstr_off:shstr_off + len(shstr.data)] = shstr.data
    for i, sh in enumerate(shdrs):
        out[shoff + 40 * i:shoff + 40 * (i + 1)] = struct.pack("<10I", *sh)
    return bytes(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("xbe")
    ap.add_argument("out")
    ap.add_argument("--symbols", help="XbSymbolDatabaseCLI output")
    ap.add_argument("--rtti", help="rtti_classes.json from analyze_rtti.py")
    a = ap.parse_args()
    xbe = Xbe(a.xbe)
    syms = collect_symbols(xbe, a.symbols, a.rtti)
    elf = build(xbe, syms)
    open(a.out, "wb").write(elf)
    kinds = {}
    for name, size, typ in syms.values():
        k = "kernel" if name.startswith("__imp__") else "rtti" if "::vf" in name else "library/other"
        kinds[k] = kinds.get(k, 0) + 1
    print(f"{a.out}: {len(elf)} bytes, {len(syms)} symbols {kinds}")


if __name__ == "__main__":
    main()
