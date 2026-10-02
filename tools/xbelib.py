"""Minimal XBE loader used by the KakutoChojin-recomp analysis scripts."""
import os
import re
import struct
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
# The title being analysed: KT_XBE / KT_ANALYSIS select another XBE and
# output directory (default: xbe/default.xbe and analysis/).
XBE_PATH = Path(os.environ.get("KT_XBE") or ROOT / "xbe" / "default.xbe")
ANALYSIS = Path(os.environ.get("KT_ANALYSIS") or ROOT / "analysis")
RETAIL_ENTRY_KEY = 0xA8FC57AB
RETAIL_THUNK_KEY = 0x5B6D40B6


@dataclass
class Section:
    name: str
    flags: int
    va: int
    vsize: int
    raw: int
    rsize: int


class Xbe:
    def __init__(self, path=XBE_PATH):
        self.data = open(path, "rb").read()
        d = self.data
        self.base, = struct.unpack_from("<I", d, 0x104)
        (self.hdr_size, self.img_size, _, self.timestamp, self.cert_va, nsec, sec_va,
         _, entry, self.tls_va, self.stack_commit, self.heap_reserve, self.heap_commit,
         _, _, _, _, _, _, _, kthunk) = struct.unpack_from("<21I", d, 0x108)
        self.entry = entry ^ RETAIL_ENTRY_KEY
        self.kthunk = kthunk ^ RETAIL_THUNK_KEY
        self.sections = []
        for i in range(nsec):
            o = sec_va - self.base + i * 0x38
            fl, va, vs, ra, rs, na = struct.unpack_from("<6I", d, o)
            self.sections.append(Section(self._hdr_cstr(na), fl, va, vs, ra, rs))

    def _hdr_cstr(self, va):
        o = va - self.base
        return d_cstr(self.data, o)

    def section(self, name):
        return next(s for s in self.sections if s.name == name)

    def section_of(self, va):
        for s in self.sections:
            if s.va <= va < s.va + s.vsize:
                return s
        return None

    def off(self, va):
        """File offset of a virtual address, or None if it is not file-backed."""
        for s in self.sections:
            if s.va <= va < s.va + s.rsize:
                return s.raw + va - s.va
        return None

    def read(self, va, n):
        o = self.off(va)
        return None if o is None else self.data[o:o + n]

    def u32(self, va):
        b = self.read(va, 4)
        return None if b is None or len(b) < 4 else struct.unpack("<I", b)[0]

    def cstr(self, va):
        o = self.off(va)
        return None if o is None else d_cstr(self.data, o)

    def kernel_imports(self):
        """[(thunk_va, ordinal)] for the kernel thunk table."""
        out, va = [], self.kthunk
        while True:
            x = self.u32(va)
            if not x:
                return out
            out.append((va, x & 0x7FFFFFFF))
            va += 4


def d_cstr(d, o):
    return d[o:d.index(b"\0", o)].decode("latin1")


def kernel_names():
    names = {}
    for line in open(ROOT / "third_party" / "xboxkrnl.exe.def"):
        m = re.match(r"\s+@?(\w+?)(?:@\d+)?\s+@\s*(\d+)", line)
        if m:
            names[int(m.group(2))] = m.group(1)
    return names


def load_symbols(path=ANALYSIS / "symbols.txt"):
    """Parse XbSymbolDatabaseCLI output into {va: (lib, kind, name, callconv, signature)}."""
    syms = {}
    for line in open(path):
        m = re.match(r"(\w+?)__(FUN|VAR|UNK)__(.*) = 0x([0-9a-f]+)", line.strip())
        if not m:
            continue
        lib, kind, rest, addr = m.groups()
        if kind == "FUN":
            conv, _, sig = rest.partition("__")
            name = sig.split("(")[0]
        else:
            conv, sig, name = "", rest, rest
        syms[int(addr, 16)] = (lib, kind, name, conv, sig)
    return syms
