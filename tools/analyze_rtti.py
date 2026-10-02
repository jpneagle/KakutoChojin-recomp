"""Step 2: recover C++ classes, inheritance and vtables from MSVC RTTI.

Outputs analysis/rtti_classes.json, analysis/rtti_classes.csv and
analysis/class_hierarchy.txt.
"""
import csv
import json
import re
import struct

import os as _os, sys as _sys; _sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))  # embedded Python does not add the script directory
from xbelib import ROOT, Xbe, ANALYSIS

xbe = Xbe()
text = xbe.section(".text")
data_secs = [s for s in xbe.sections if s.name in (".rdata", ".data")]


def demangle(raw):
    """'.?AVFoo@ns@@' -> 'ns::Foo'. Templates are left in mangled form."""
    body = raw[4:-2]
    if "?$" in body:
        return raw
    return "::".join(reversed([p for p in body.split("@") if p]))


def is_code(va):
    return text.va <= va < text.va + text.rsize


# TypeDescriptor = { void* pVFTable; void* spare; char name[]; }
types = {}
for s in data_secs:
    blob = xbe.data[s.raw:s.raw + s.rsize]
    for m in re.finditer(rb"\.\?A[VU][\x21-\x7e]+?@@\x00", blob):
        td = s.va + m.start() - 8
        raw = m.group()[:-1].decode()
        types[td] = {"td": td, "mangled": raw, "name": demangle(raw),
                     "kind": "struct" if raw[3] == "U" else "class"}

# CompleteObjectLocator = { sig, offset, cdOffset, TypeDescriptor*, ClassHierarchyDescriptor* }
cols = {}
for s in data_secs:
    for o in range(0, s.rsize - 20, 4):
        sig, off, cdoff, td, chd = struct.unpack_from("<5I", xbe.data, s.raw + o)
        if sig == 0 and td in types and xbe.section_of(chd) and off < 0x10000:
            cols[s.va + o] = (off, td, chd)

# vtable[-1] points at a COL
vtables = []
for s in data_secs:
    for o in range(0, s.rsize - 8, 4):
        p, = struct.unpack_from("<I", xbe.data, s.raw + o)
        if p in cols and is_code(xbe.u32(s.va + o + 4) or 0):
            vt = s.va + o + 4
            funcs = []
            while True:
                f = xbe.u32(vt + 4 * len(funcs))
                # stop at the next vtable's COL pointer or any non-code value
                if f is None or not is_code(f) or (len(funcs) and xbe.u32(vt + 4 * len(funcs)) in cols):
                    break
                funcs.append(f)
            off, td, _ = cols[p]
            vtables.append({"va": vt, "td": td, "this_offset": off, "methods": funcs})


def bases_of(chd):
    # ClassHierarchyDescriptor = { sig, attributes, numBaseClasses, BaseClassArray* }
    _, attrs, n, arr = struct.unpack("<4I", xbe.read(chd, 16))
    out = []
    for i in range(n):
        bcd = xbe.u32(arr + 4 * i)
        # BaseClassDescriptor = { TypeDescriptor*, numContainedBases, mdisp, pdisp, vdisp, attributes }
        td, ncont, mdisp, pdisp, vdisp, _ = struct.unpack("<IIiiiI", xbe.read(bcd, 24))
        out.append({"td": td, "contained": ncont, "mdisp": mdisp, "vbase": pdisp != -1})
    return attrs, out


classes = {}
for col, (off, td, chd) in cols.items():
    if td in classes:
        continue
    attrs, bl = bases_of(chd)
    t = types[td]
    # bl[0] is the class itself; its direct bases are the entries not
    # contained inside an earlier base's subtree.
    direct, i = [], 1
    while i < len(bl):
        direct.append(bl[i])
        i += 1 + bl[i]["contained"]
    classes[td] = {
        "name": t["name"], "mangled": t["mangled"], "kind": t["kind"],
        "multiple_inheritance": bool(attrs & 1), "virtual_inheritance": bool(attrs & 2),
        "bases": [{"name": types[b["td"]]["name"] if b["td"] in types else hex(b["td"]),
                   "offset": b["mdisp"], "virtual": b["vbase"]} for b in direct],
        "all_bases": [types[b["td"]]["name"] for b in bl[1:] if b["td"] in types],
        "vtables": [],
    }
for vt in vtables:
    classes[vt["td"]]["vtables"].append(
        {"va": f"0x{vt['va']:08x}", "this_offset": vt["this_offset"],
         "methods": [f"0x{f:08x}" for f in vt["methods"]]})

# Types that have a TypeDescriptor but no vtable (used only by catch/typeid)
for td, t in types.items():
    classes.setdefault(td, {"name": t["name"], "mangled": t["mangled"], "kind": t["kind"],
                            "bases": [], "all_bases": [], "vtables": [], "no_vtable": True})

out = ANALYSIS
ordered = sorted(classes.values(), key=lambda c: c["name"].lower())
json.dump(ordered, open(out / "rtti_classes.json", "w"), indent=1)

with open(out / "rtti_classes.csv", "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(["name", "namespace", "bases", "vtable", "num_virtuals"])
    for c in ordered:
        ns = c["name"].rpartition("::")[0]
        vt = c["vtables"][0] if c["vtables"] else None
        w.writerow([c["name"], ns, ";".join(b["name"] for b in c["bases"]),
                    vt["va"] if vt else "", len(vt["methods"]) if vt else 0])

children = {}
for c in ordered:
    for b in c["bases"]:
        children.setdefault(b["name"], []).append(c["name"])
roots = [c["name"] for c in ordered if not c["bases"]]


def tree(name, depth, fh, seen):
    n = len(children.get(name, []))
    fh.write("  " * depth + name + (f"  ({n})" if n else "") + "\n")
    if name in seen:
        return
    seen.add(name)
    for ch in children.get(name, []):
        tree(ch, depth + 1, fh, seen)


with open(out / "class_hierarchy.txt", "w") as fh:
    for r in roots:
        tree(r, 0, fh, set())

ns_count = {}
for c in ordered:
    ns = c["name"].rpartition("::")[0] or "(global)"
    ns_count[ns] = ns_count.get(ns, 0) + 1
print(f"type descriptors: {len(types)}  COLs: {len(cols)}  vtables: {len(vtables)}")
print(f"classes with vtable: {sum(1 for c in ordered if c['vtables'])}")
print(f"virtual methods (unique): {len({f for v in vtables for f in v['methods']})}")
print("namespaces:", sorted(ns_count.items(), key=lambda x: -x[1]))
