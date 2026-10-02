"""Survey: recursive-descent disassembly of every known function; counts
mnemonics, operand forms and unresolved indirect jumps."""
import collections
import sys
from pathlib import Path

import capstone
from capstone import x86

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import os as _os, sys as _sys; _sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))  # embedded Python does not add the script directory
from xbelib import Xbe

xbe = Xbe()
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
md.detail = True

entries = set()
for line in open('analysis/decomp/index.tsv', encoding='utf-8'):
    f = line.split('\t')
    if not f[0] or not all(c in '0123456789abcdefABCDEF' for c in f[0]):
        continue
    a = int(f[0], 16)
    if a < 0x80000000:
        entries.add(a)

mn = collections.Counter()
unresolved = collections.Counter()
prefixes = collections.Counter()
seg = collections.Counter()
fails = 0
for e in sorted(entries):
    todo, seen = [e], set()
    while todo:
        a = todo.pop()
        while a not in seen:
            code = xbe.read(a, 16)
            if not code:
                fails += 1
                break
            ins = next(md.disasm(code, a), None)
            if ins is None:
                fails += 1
                break
            seen.add(a)
            m = ins.mnemonic
            mn[m] += 1
            if ins.prefix[0] in (0xF2, 0xF3) and not m.startswith(('rep', 'movs', 'stos', 'cmps', 'scas', 'lods')):
                prefixes[m] += 1
            if ins.prefix[1]:
                seg[(m, ins.prefix[1])] += 1
            if x86.X86_GRP_JUMP in ins.groups:
                op = ins.operands[0]
                if op.type == x86.X86_OP_IMM:
                    todo.append(op.imm)
                    if m == 'jmp':
                        break
                else:
                    unresolved['mem' if op.type == x86.X86_OP_MEM else 'reg'] += 1
                    break
            if m in ('ret', 'retf', 'int3', 'hlt', 'ud2'):
                break
            a += ins.size
print('functions', len(entries), 'decode failures', fails)
print('distinct mnemonics', len(mn))
print(' '.join(f'{m}:{c}' for m, c in mn.most_common()))
print('unresolved indirect jumps', dict(unresolved))
print('segment prefixes', seg.most_common(10))
