"""Lifts the functions of an XBE from x86 machine code to C.

Every function becomes `void f_XXXXXXXX(KtCpu* c)` (see src/lift/kt_lift.h).
Registers and lazy flags are C locals inside a function and are written
back to the KtCpu around calls and returns; guest memory, the guest stack
and calling conventions are kept exactly, so lifted functions and the
original machine code can call each other (mixed mode, kt_host).

A function containing an instruction the lifter does not support is not
emitted; calls to it go to the original code.

usage: lift.py <default.xbe> <manifest.txt> <out dir> [--functions index.tsv]
                [--group N]
  --functions  function entry list (first column hex address), e.g. the
               Ghidra export analysis/decomp/index.tsv; without it entries
               are discovered from the entry point by following calls.
"""
import argparse
import collections
import os
import re
import sys
from pathlib import Path

import capstone
from capstone import x86

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
sys.path.insert(0, str(Path(__file__).resolve().parent))  # embedded Python does not add it
from xbelib import Xbe  # noqa: E402

from x86c import FunctionLifter, Unsupported, resolve_calls  # noqa: E402
import simd  # noqa: E402,F401  (registers the MMX/SSE handlers)

MD = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
MD.detail = True


def read_manifest(path):
    hle, fs = set(), set()
    for line in open(path, encoding='utf-8'):
        f = line.split()
        if not f:
            continue
        if f[0] == 'hle':
            hle.add(int(f[1], 16))
        elif f[0] == 'fs':
            fs.add(int(f[1], 16))
    return hle, fs


def read_function_list(path):
    out = set()
    for line in open(path, encoding='utf-8'):
        f = line.split('\t')[0].strip()
        try:
            a = int(f, 16)
        except ValueError:
            continue
        if a < 0x80000000:
            out.add(a)
    return out


class Decoder:
    def __init__(self, xbe):
        self.xbe = xbe
        self.cache = {}
        text = [s for s in xbe.sections if s.flags & 4]  # executable sections
        self.exec_ranges = [(s.va, s.va + s.rsize) for s in text]

    def is_code(self, a):
        return any(lo <= a < hi for lo, hi in self.exec_ranges)

    def at(self, a):
        ins = self.cache.get(a)
        if ins is None and a not in self.cache:
            code = self.xbe.read(a, 16) if self.is_code(a) else None
            ins = next(MD.disasm(code, a), None) if code else None
            self.cache[a] = ins
        return ins


def jump_table(dec, insns, ins, func_entries):
    """Targets of `jmp [reg*4 + table]` as (index register, [target or None],
    first index), or None. The bound comes from a preceding `cmp reg, N` /
    `and reg, N` on the index register; entries that are not code (unused
    slots) are None. A `neg reg` just before the jump indexes the table
    backwards from its base (the CRT memmove does this): the targets then
    cover indices -(N-1) .. 0."""
    op = ins.operands[0]
    if op.type != x86.X86_OP_MEM or op.mem.base != 0 or op.mem.index == 0 or op.mem.scale != 4:
        return None
    table = op.mem.disp & 0xFFFFFFFF
    index_reg = ins.reg_name(op.mem.index)
    bound = None
    negated = False
    for a in reversed(sorted(a for a in insns if a < ins.address)[-12:]):
        p = insns[a]
        if not p.operands or p.operands[0].type != x86.X86_OP_REG or p.reg_name(p.operands[0].reg) != index_reg:
            continue
        if p.mnemonic == 'neg' and len(p.operands) == 1 and not negated:
            negated = True
            continue  # the bound is on the value before negation
        if len(p.operands) == 2:
            if p.mnemonic in ('cmp', 'and') and p.operands[1].type == x86.X86_OP_IMM:
                bound = (p.operands[1].imm & 0xFFFFFFFF) + 1
            break  # the closest instruction writing/testing the index decides
    def entry(i):
        raw = dec.xbe.read(table + 4 * i, 4)
        if raw is None:
            return None
        t = dec.xbe.u32(table + 4 * i)
        return t if dec.is_code(t) else None
    step = -1 if negated else 1
    if bound is not None and bound <= 1024:
        targets = [entry(step * i) for i in range(bound)]
        if sum(t is not None for t in targets) * 2 < len(targets):
            return None
    else:
        targets = []
        for i in range(1024):
            t = entry(step * i)
            if t is None or t in func_entries or (targets and abs(t - targets[0]) > 0x10000):
                break
            targets.append(t)
    if not any(t is not None for t in targets):
        return None
    if negated:  # targets[k] is index -k: store them from the lowest index
        return index_reg, targets[::-1], -(len(targets) - 1)
    return index_reg, targets, 0


def looks_like_function(dec, t, code_sections):
    """Is an immediate operand plausibly the address of a function (an
    exception handler or callback) rather than a number or a data address?
    XBE sections are all marked executable, so only sections that hold known
    functions count as code."""
    if not any(lo <= t < hi for lo, hi in code_sections):
        return False
    prev = dec.xbe.read(t - 3, 3)
    if prev is None:
        return t % 16 == 0
    # Function starts follow alignment padding or the previous function's ret.
    return t % 16 == 0 or prev[2] in (0xCC, 0x90, 0xC3) or (prev[0] == 0xC2 and prev[2] == 0)


def discover(dec, entry, func_entries, hle):
    """Recursive descent over one function. Returns (insns, labels, calls,
    tables, error)."""
    insns, labels, calls, tables = {}, set(), set(), {}
    todo = [entry]
    while todo:
        a = todo.pop()
        while a not in insns:
            if a != entry and a in func_entries:
                break  # fell into another function: tail call
            ins = dec.at(a)
            if ins is None:
                return None, None, None, None, f'decode failure at {a:08x}'
            insns[a] = ins
            m = ins.mnemonic
            if m == 'call':
                op = ins.operands[0]
                if op.type == x86.X86_OP_IMM:
                    calls.add(op.imm & 0xFFFFFFFF)
            elif x86.X86_GRP_JUMP in ins.groups:
                op = ins.operands[0]
                if op.type == x86.X86_OP_IMM:
                    t = op.imm & 0xFFFFFFFF
                    if t not in func_entries or t == entry:
                        labels.add(t)
                        todo.append(t)
                    if m == 'jmp':
                        break
                else:
                    jt = jump_table(dec, insns, ins, func_entries) if m == 'jmp' else None
                    if jt:
                        tables[a] = jt
                        for t in jt[1]:
                            if t is not None:
                                labels.add(t)
                                todo.append(t)
                    break
            if m in ('ret', 'retf', 'hlt', 'ud2'):
                break
            a += ins.size
    return insns, labels, calls, tables, None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('xbe')
    ap.add_argument('manifest')
    ap.add_argument('out')
    ap.add_argument('--functions')
    ap.add_argument('--group', type=int, default=400, help='functions per C file')
    ap.add_argument('--extra', help='more entry addresses (one hex address per line), e.g. the lift_gaps.txt '
                                    'written by kt_host in KT_LIFT_STRICT mode')
    args = ap.parse_args()

    xbe = Xbe(args.xbe)
    dec = Decoder(xbe)
    hle, fs_sites = read_manifest(args.manifest)
    entries = set(read_function_list(args.functions)) if args.functions else set()
    entries.add(xbe.entry)
    extra = read_function_list(args.extra) if args.extra and os.path.exists(args.extra) else set()
    entries |= extra
    entries |= {h for h in hle if dec.is_code(h)}

    # Discover functions (following direct calls to new entries).
    code_sections = []
    for sec in xbe.sections:
        lo, hi = sec.va, sec.va + sec.rsize
        # MSVC data sections never hold code, whatever the analysis says.
        if sec.name not in ('.data', '.rdata', '.bss') and any(lo <= e < hi for e in entries):
            code_sections.append((lo, hi))
    functions = {}
    guessed = set()
    # Function pointers stored in data (initialiser tables, callback tables).
    for sec in xbe.sections:
        if any(lo <= sec.va < hi for lo, hi in code_sections) or sec.name.startswith('$$'):
            continue
        raw = xbe.read(sec.va, sec.rsize) or b''
        for off in range(0, len(raw) - 3, 4):
            t = int.from_bytes(raw[off:off + 4], 'little')
            if t not in entries and looks_like_function(dec, t, code_sections):
                guessed.add(t)
                entries.add(t)
    todo = sorted(entries)
    seen = set()
    while todo:
        e = todo.pop()
        # HLE functions are lifted too: whether the host replaces one is
        # decided at run time (kt_host leaves patched entries alone).
        if e in seen or not dec.is_code(e):
            continue
        seen.add(e)
        functions[e] = discover(dec, e, entries, hle)
        # Direct call targets, and code addresses used as immediates
        # (exception handlers, callbacks, thread routines) are functions too.
        targets = set(functions[e][2] or ())
        for ins in (functions[e][0] or {}).values():
            # Handlers and callbacks are pushed as arguments or stored into
            # objects; `mov reg, imm` is mostly a data address and is skipped.
            if ins.mnemonic == 'push' or (ins.mnemonic == 'mov' and ins.operands[0].type == x86.X86_OP_MEM):
                for op in ins.operands:
                    if op.type == x86.X86_OP_IMM and op.size == 4 and looks_like_function(dec, op.imm & 0xFFFFFFFF, code_sections):
                        targets.add(op.imm & 0xFFFFFFFF)
                        guessed.add(op.imm & 0xFFFFFFFF)
        for t in targets:
            if t not in entries and dec.is_code(t) and t not in (functions[e][0] or {}):
                entries.add(t)
                todo.append(t)
    # Guessed entries that do not decode as code were not functions.
    for t in guessed:
        if t in functions and functions[t][4]:
            del functions[t]
            entries.discard(t)

    # Lift.
    # Guessed entries must also lift (junk bytes decode as odd instructions).
    for t in sorted(guessed & set(functions)):
        insns, labels, calls, tables, err = functions[t]
        try:
            FunctionLifter(t, insns, labels, tables, entries, hle, {}).lift()
        except Unsupported:
            del functions[t]
            entries.discard(t)

    order = sorted(functions)
    index = {e: i for i, e in enumerate(order)}
    lifted, failed = {}, collections.Counter()
    for e in order:
        insns, labels, calls, tables, err = functions[e]
        if err:
            failed[err.split(' at ')[0]] += 1
            continue
        fl = FunctionLifter(e, insns, labels, tables, entries, hle, index)
        try:
            code = fl.lift()
            flags = fl.entry_flags(fs_sites, code)
            # Never overwrite bytes at guessed entries (pointers found in data
            # or immediates, run-time gaps): they may be tables inside code
            # sections. Lifted callers reach them directly anyway.
            if e in guessed or e in extra or not any(lo <= e < hi for lo, hi in code_sections):
                flags &= 2
            lifted[e] = (code, flags)
        except Unsupported as u:
            failed[str(u)] += 1

    lifted_set = set(lifted)
    for e in lifted:
        lifted[e] = (resolve_calls(lifted[e][0], lifted_set, index), lifted[e][1])

    os.makedirs(args.out, exist_ok=True)
    names = sorted(lifted)
    outputs = {}
    for g in range(0, len(names), args.group):
        group = names[g:g + args.group]
        body = ''.join(lifted[e][0] + '\n' for e in group)
        used = sorted(set(re.findall(r'\bf_([0-9a-f]{8})\(c\)', body)) | {f'{e:08x}' for e in group})
        flags = sorted(set(re.findall(r'\bkt_on_([0-9a-f]{8})\b', body)))
        decls = ''.join(f'void f_{x}(KtCpu* c);\n' for x in used)
        decls += ''.join(f'extern uint8_t kt_on_{x};\n' for x in flags)
        outputs[f'lifted_{g // args.group:03d}.c'] = (
            "/* Generated by tools/lift/lift.py from the user's XBE. Do not redistribute. */\n"
            '#include "kt_lift.h"\n\n' + decls + '\n' + body)
    # The table: entry address, lifted function, flags (see entry_flags) and
    # the function's enable flag, which kt_host sets from KT_LIFT.
    parts = ['/* Generated by tools/lift/lift.py. */\n#include "kt_lift.h"\n\n']
    parts += [f'void f_{e:08x}(KtCpu* c);\nuint8_t kt_on_{e:08x};\n' for e in names]
    parts += ['\ntypedef struct { uint32_t va; void (*fn)(KtCpu*); uint32_t flags; uint8_t* on; } KtLiftEntry;\n',
              'static const KtLiftEntry kt_table[] = {\n']
    for e in order:
        parts.append(f'    {{0x{e:08x}, f_{e:08x}, 0x{lifted[e][1]:x}, &kt_on_{e:08x}}},\n' if e in lifted
                     else f'    {{0x{e:08x}, 0, 0, 0}},\n')
    parts += ['};\n\n', 'KT_LIFT_EXPORT const KtLiftEntry* KtLiftTable(uint32_t* count) {\n',
              f'    *count = {len(order)};\n    return kt_table;\n}}\n']
    outputs['lifted_table.c'] = ''.join(parts)
    # Only rewrite files whose content changed, so rebuilds stay incremental;
    # drop files a previous run produced beyond the current set.
    changed = 0
    for old in os.listdir(args.out):
        if (old.startswith('lifted_') and old.endswith('.c') or old == 'kt_lifted.h') and old not in outputs:
            os.remove(os.path.join(args.out, old))
    for name, text in outputs.items():
        path = os.path.join(args.out, name)
        if os.path.exists(path) and open(path, encoding='utf-8', newline='').read() == text:
            continue
        with open(path, 'w', encoding='utf-8', newline='\n') as f:
            f.write(text)
        changed += 1
    files = [n for n in outputs if n.startswith('lifted_0')]
    print(f'{changed} of {len(outputs)} output files changed')

    leaves = sum(1 for e in lifted if lifted[e][1] & 2 and lifted[e][1] >> 8)
    print(f'verifiable leaf functions {leaves}')
    print(f'functions {len(order)}, lifted {len(lifted)} ({100 * len(lifted) / max(len(order), 1):.1f}%), '
          f'{len(files)} files in {args.out}')
    for reason, n in failed.most_common(25):
        print(f'  {n:6d}  {reason}')


if __name__ == '__main__':
    main()
