"""Write Ghidra annotations (names, calling conventions, parameters, classes)
for an XBE, consumed by tools/ghidra/KtApply.java.

Format: one tab-separated record per line
  CLASS   <name>          <base:offset;base:offset...>
  VTABLE  <class>  <va>  <this_offset>  <method_va,method_va,...>
  FUNC    <va>  <conv>    <name>      <param,param,...>
  KIMP    <va>  <name>    <param_count>  <conv>  <stub_va>
  LABEL   <va>  <name>

usage: gen_ghidra_annotations.py <default.xbe> <symbols.txt> <rtti_classes.json> <out.tsv>
"""
import json
import re
import sys

from xbe2elf import kernel_stub
import os as _os, sys as _sys; _sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))  # embedded Python does not add the script directory
from xbelib import ROOT, Xbe, load_symbols


def kernel_signatures():
    """{ordinal: (name, stdcall param count or -1 for cdecl/data)} from nxdk's .def."""
    out = {}
    for line in open(ROOT / "third_party" / "xboxkrnl.exe.def"):
        m = re.match(r"\s+(@?)(\w+?)(?:@(\d+))?\s+@\s*(\d+)", line)
        if m:
            fastcall, name, nbytes, ordinal = m.groups()
            out[int(ordinal)] = (name, int(nbytes) // 4 if nbytes else -1, bool(fastcall))
    return out


def main():
    xbe_path, sym_path, rtti_path, out_path = sys.argv[1:5]
    xbe = Xbe(xbe_path)
    lines = []

    ksig = kernel_signatures()
    for va, ordinal in xbe.kernel_imports():
        name, nparams, fastcall = ksig.get(ordinal, (f"xboxkrnl_{ordinal}", -1, False))
        # Exports without an @N suffix are variables, except the cdecl ones.
        cdecl = name in ("DbgPrint", "snprintf", "sprintf", "vsnprintf", "vsprintf")
        conv = "fastcall" if fastcall else "cdecl" if cdecl else "data" if nparams < 0 else "stdcall"
        lines.append(f"KIMP\t{va:08x}\t{name}\t{nparams}\t{conv}\t{kernel_stub(ordinal):08x}")

    named = set()
    for va, (lib, kind, name, conv, sig) in load_symbols(sym_path).items():
        if kind == "VAR":
            lines.append(f"LABEL\t{va:08x}\t{name}")
            continue
        params = []
        inner = sig[sig.index("(") + 1:sig.rindex(")")] if "(" in sig else ""
        for p in filter(None, (x.strip() for x in inner.split(","))):
            parts = p.split()
            params.append(parts[-1] if len(parts) > 1 else p)
        lines.append(f"FUNC\t{va:08x}\t{conv or 'unknown'}\t{name}\t{','.join(params)}")
        named.add(va)

    classes = json.load(open(rtti_path))
    for c in classes:
        bases = ";".join(f"{b['name']}:{b['offset']}" for b in c.get("bases", []))
        lines.append(f"CLASS\t{c['name']}\t{bases}")
    # Virtual methods: named after the most basic class whose vtable holds them.
    owner = {}
    for c in classes:
        depth = len(c.get("all_bases", []))
        for vt in c.get("vtables", []):
            lines.append(f"LABEL\t{int(vt['va'], 16):08x}\t{c['name']}::vftable")
            methods = ",".join(m[2:] for m in vt["methods"])
            lines.append(f"VTABLE\t{c['name']}\t{int(vt['va'], 16):08x}\t{vt['this_offset']}\t{methods}")
            for i, f in enumerate(vt["methods"]):
                f = int(f, 16)
                if f not in owner or depth < owner[f][0]:
                    owner[f] = (depth, c["name"], i)
    for f, (_, cls, i) in sorted(owner.items()):
        if f not in named:
            lines.append(f"FUNC\t{f:08x}\tthiscall\t{cls}::vf{i}\t")

    lines.append(f"FUNC\t{xbe.entry:08x}\tcdecl\tentry_point\t")
    open(out_path, "w").write("\n".join(lines) + "\n")
    print(f"{out_path}: {len(lines)} records")


if __name__ == "__main__":
    main()
