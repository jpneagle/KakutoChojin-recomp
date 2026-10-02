"""x86 (IA-32 integer subset) to C translation for tools/lift/lift.py.

Generated code conventions (see src/lift/kt_lift.h):
  locals eax..edi, esp and cc_op/cc_res/cc_a/cc_b/cc_cf (lazy flags);
  KT_STORE / KT_LOAD move them to and from the KtCpu around calls;
  guest memory through rd8/16/32 and wr8/16/32.

Calls and tail jumps to other functions are emitted as placeholders
(@@CALL t next@@, @@TAIL t@@) and resolved by resolve_calls() once it is
known which functions were lifted.
"""
import re

from capstone import x86


class Unsupported(Exception):
    pass


REG32 = ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi']
PARENT = {}
for r in REG32:
    PARENT[r] = (r, 4, 0)
for r16, r32 in [('ax', 'eax'), ('cx', 'ecx'), ('dx', 'edx'), ('bx', 'ebx'), ('sp', 'esp'), ('bp', 'ebp'),
                 ('si', 'esi'), ('di', 'edi')]:
    PARENT[r16] = (r32, 2, 0)
for r8, r32 in [('al', 'eax'), ('cl', 'ecx'), ('dl', 'edx'), ('bl', 'ebx')]:
    PARENT[r8] = (r32, 1, 0)
for r8, r32 in [('ah', 'eax'), ('ch', 'ecx'), ('dh', 'edx'), ('bh', 'ebx')]:
    PARENT[r8] = (r32, 1, 8)

UT = {1: 'uint8_t', 2: 'uint16_t', 4: 'uint32_t', 8: 'uint64_t'}
ST = {1: 'int8_t', 2: 'int16_t', 4: 'int32_t', 8: 'int64_t'}
MASK = {1: 0xFF, 2: 0xFFFF, 4: 0xFFFFFFFF}
SIGN = {1: 0x80, 2: 0x8000, 4: 0x80000000}

# Condition code number (x86 encoding) per mnemonic suffix.
CC = {'o': 0, 'no': 1, 'b': 2, 'c': 2, 'nae': 2, 'ae': 3, 'nb': 3, 'nc': 3, 'e': 4, 'z': 4, 'ne': 5, 'nz': 5,
      'be': 6, 'na': 6, 'a': 7, 'nbe': 7, 's': 8, 'ns': 9, 'p': 10, 'pe': 10, 'np': 11, 'po': 11, 'l': 12,
      'nge': 12, 'ge': 13, 'nl': 13, 'le': 14, 'ng': 14, 'g': 15, 'nle': 15}

COND = 'kt_cond({cc}, cc_op, cc_res, cc_a, cc_b, cc_cf)'
FLAGS_ALL = 'kt_eflags(cc_op, cc_res, cc_a, cc_b, cc_cf, c->df)'


def hx(v):
    return f'0x{v & 0xFFFFFFFF:x}u'


class Operand:
    """A capstone operand with C read/write helpers."""

    def __init__(self, fl, ins, op):
        self.fl, self.ins, self.op = fl, ins, op
        self.size = op.size
        self.kind = op.type

    def is_mem(self):
        return self.kind == x86.X86_OP_MEM

    def is_reg(self):
        return self.kind == x86.X86_OP_REG

    def is_imm(self):
        return self.kind == x86.X86_OP_IMM

    def reg(self):
        return self.ins.reg_name(self.op.reg)

    def fs(self):
        return self.is_mem() and self.op.mem.segment == x86.X86_REG_FS

    def addr(self):
        m = self.op.mem
        parts = []
        if m.base:
            b = self.ins.reg_name(m.base)
            if b not in REG32:
                raise Unsupported(f'address base {b}')
            parts.append(b)
        if m.index:
            i = self.ins.reg_name(m.index)
            if i not in REG32:
                raise Unsupported(f'address index {i}')
            parts.append(i if m.scale == 1 else f'{i} * {m.scale}u')
        if m.disp or not parts:
            parts.append(hx(m.disp))
        if m.segment not in (0, x86.X86_REG_FS, x86.X86_REG_DS, x86.X86_REG_SS, x86.X86_REG_ES, x86.X86_REG_CS):
            raise Unsupported('segment override')
        return '(uint32_t)(' + ' + '.join(parts) + ')'

    def read(self, size=None):
        size = size or self.size
        if self.is_imm():
            return hx(self.op.imm & MASK[size])
        if self.is_reg():
            name = self.reg()
            if name not in PARENT:
                raise Unsupported(f'register {name}')
            p, s, sh = PARENT[name]
            if s == 4:
                return p
            if sh:
                return f'(uint8_t)({p} >> 8)'
            return f'({UT[s]}){p}'
        if self.fs():
            return f'({UT[size]})KtFsRead({self.addr()}, {size})'
        if size not in (1, 2, 4):
            raise Unsupported(f'memory read size {size}')
        return f'rd{size * 8}({self.addr()})'

    def write(self, value):
        if self.is_reg():
            name = self.reg()
            if name not in PARENT:
                raise Unsupported(f'register {name}')
            p, s, sh = PARENT[name]
            if s == 4:
                return f'{p} = (uint32_t)({value});'
            if s == 2:
                return f'{p} = ({p} & 0xFFFF0000u) | (uint16_t)({value});'
            if sh:
                return f'{p} = ({p} & 0xFFFF00FFu) | ((uint32_t)(uint8_t)({value}) << 8);'
            return f'{p} = ({p} & 0xFFFFFF00u) | (uint8_t)({value});'
        if self.is_mem():
            if self.fs():
                return f'KtFsWrite({self.addr()}, {self.size}, (uint32_t)({value}));'
            if self.size not in (1, 2, 4):
                raise Unsupported(f'memory write size {self.size}')
            return f'wr{self.size * 8}({self.addr()}, ({UT[self.size]})({value}));'
        raise Unsupported('write to immediate')


class FunctionLifter:
    def __init__(self, entry, insns, labels, tables, entries, hle, index):
        self.entry, self.insns, self.labels, self.tables = entry, insns, set(labels), tables
        self.entries, self.hle, self.index = entries, hle, index
        self.used = set()

    # ---- helpers -------------------------------------------------------------------------

    def ops(self, ins):
        return [Operand(self, ins, o) for o in ins.operands]

    def goto(self, t):
        if t in self.insns:
            self.used.add(t)
            return f'goto L_{t:08x};'
        return f'@@TAIL {t:08x}@@'

    def set_flags(self, op, size, res, a='0', b='0'):
        return f'cc_op = KT_CC({op}, {size}); cc_res = {res}; cc_a = {a}; cc_b = {b};'

    def entry_flags(self, fs_sites, code):
        """Table flags: bit 0 the entry can be patched with a 5-byte jump;
        bit 1 leaf (no calls, jumps out, fs or timing access: safe to run
        twice for verification); bits 8-15 length of the whole instructions
        covering the first 5 bytes when they can be copied to a trampoline
        (no relative branches, no fs patch site), else 0."""
        size, a, relative = 0, self.entry, False
        while size < 5:
            ins = self.insns.get(a)
            if ins is None or (ins.mnemonic in ('ret', 'jmp') and size + ins.size < 5):
                return 0
            if x86.X86_GRP_JUMP in ins.groups or ins.mnemonic == 'call' or a in fs_sites:
                relative = True
            size += ins.size
            a += ins.size
        if any(self.entry < t < self.entry + size for t in self.labels):
            return 0
        flags = 1
        if not relative:
            flags |= size << 8
        if not any(k in code for k in ('@@CALL', '@@TAIL', 'KtCallIndirect', 'KtJumpNative', 'KtCallNative',
                                       'KtFs', 'KtRdtsc', 'KtCpuid', 'KtTrap')):
            flags |= 2
        return flags

    # ---- driver --------------------------------------------------------------------------

    def lift(self):
        chunks = []
        addrs = sorted(self.insns)
        for i, a in enumerate(addrs):
            ins = self.insns[a]
            code = self.translate(ins)
            nxt = a + ins.size
            if not self.terminates(ins):
                if i + 1 < len(addrs) and addrs[i + 1] == nxt:
                    pass
                elif nxt in self.insns:
                    code.append(self.goto(nxt))
                else:
                    code.append(f'@@TAIL {nxt:08x}@@')
            chunks.append((a, ins, code))
        out = [f'void f_{self.entry:08x}(KtCpu* c) {{', '    KT_LOCALS;', '    KT_LOAD;']
        if addrs[0] != self.entry:  # blocks below the entry are emitted first
            self.used.add(self.entry)
            out.append(f'    goto L_{self.entry:08x};')
        for a, ins, code in chunks:
            if a in self.used:
                out.append(f'L_{a:08x}:')
            out.append(f'    /* {a:08x}: {ins.mnemonic} {ins.op_str} */')
            out.extend('    ' + line for line in code)
        out.append('}')
        return '\n'.join(out) + '\n'

    @staticmethod
    def terminates(ins):
        return ins.mnemonic in ('jmp', 'ret', 'retf', 'hlt', 'ud2')

    # ---- translation ---------------------------------------------------------------------

    def translate(self, ins):
        m = ins.mnemonic
        if m.startswith('lock '):  # one guest access at a time: plain semantics suffice
            m = m[5:]
        prefix = ''
        if m.startswith(('rep ', 'repe ', 'repne ')):
            prefix, m = m.split(' ', 1)
        if ins.prefix[0] == 0xF0:  # lock: single-threaded semantics are enough for one access
            pass
        handler = getattr(self, 'i_' + m, None)
        if handler is None:
            jcc = re.fullmatch(r'j(n?[a-z]+)', m)
            if jcc and jcc.group(1) in CC:
                return self.jcc(ins, CC[jcc.group(1)])
            setcc = re.fullmatch(r'set([a-z]+)', m)
            if setcc and setcc.group(1) in CC:
                o = self.ops(ins)
                return [o[0].write(f'(uint8_t){COND.format(cc=CC[setcc.group(1)])}')]
            raise Unsupported(m)
        if prefix and m[:4] in ('movs', 'stos', 'lods', 'cmps', 'scas'):
            return handler(ins, prefix)
        return handler(ins)

    # data movement
    def i_mov(self, ins):
        d, s = self.ops(ins)
        return [d.write(s.read(d.size))]

    def i_movzx(self, ins):
        d, s = self.ops(ins)
        return [d.write(f'(uint32_t){s.read()}')]

    def i_movsx(self, ins):
        d, s = self.ops(ins)
        return [d.write(f'(uint32_t)(int32_t)({ST[s.size]}){s.read()}')]

    def i_lea(self, ins):
        d, s = self.ops(ins)
        return [d.write(s.addr())]

    def i_xchg(self, ins):
        a, b = self.ops(ins)
        return ['{', f'    {UT[a.size]} t0 = {a.read()}, t1 = {b.read()};', '    ' + a.write('t1'),
                '    ' + b.write('t0'), '}']

    def i_push(self, ins):
        (s,) = self.ops(ins)
        if s.size == 2:
            return ['{', f'    uint16_t t = {s.read()};', '    esp -= 2; wr16(esp, t);', '}']
        val = s.read(4) if not s.is_imm() else hx(s.op.imm)
        return ['{', f'    uint32_t t = {val};', '    esp -= 4; wr32(esp, t);', '}']

    def i_pop(self, ins):
        (d,) = self.ops(ins)
        if d.size == 2:
            return ['{', '    uint16_t t = rd16(esp); esp += 2;', '    ' + d.write('t'), '}']
        return ['{', '    uint32_t t = rd32(esp); esp += 4;', '    ' + d.write('t'), '}']

    def i_pushal(self, ins):
        return ['{', '    uint32_t t = esp;', '    esp -= 32;',
                '    wr32(esp + 28, eax); wr32(esp + 24, ecx); wr32(esp + 20, edx); wr32(esp + 16, ebx);',
                '    wr32(esp + 12, t); wr32(esp + 8, ebp); wr32(esp + 4, esi); wr32(esp, edi);', '}']

    def i_popal(self, ins):
        return ['edi = rd32(esp); esi = rd32(esp + 4); ebp = rd32(esp + 8); ebx = rd32(esp + 16);',
                'edx = rd32(esp + 20); ecx = rd32(esp + 24); eax = rd32(esp + 28); esp += 32;']

    def i_pushfd(self, ins):
        return ['{', f'    uint32_t t = {FLAGS_ALL};', '    esp -= 4; wr32(esp, t);', '}']

    def i_popfd(self, ins):
        return ['{', '    uint32_t t = rd32(esp); esp += 4;', '    c->df = (t >> 10) & 1;',
                '    ' + self.set_flags('KT_CC_EFLAGS', 4, 't'), '}']

    def i_lahf(self, ins):
        return [f'eax = (eax & 0xFFFF00FFu) | (({FLAGS_ALL}) & 0xD5u) << 8 | 0x200u;']

    def i_sahf(self, ins):
        return ['{', f'    uint32_t t = ({FLAGS_ALL} & ~0xD5u) | ((eax >> 8) & 0xD5u);',
                '    ' + self.set_flags('KT_CC_EFLAGS', 4, 't'), '}']

    def i_cdq(self, ins):
        return ['edx = (uint32_t)((int32_t)eax >> 31);']

    def i_cwde(self, ins):
        return ['eax = (uint32_t)(int32_t)(int16_t)eax;']

    def i_cbw(self, ins):
        return ['eax = (eax & 0xFFFF0000u) | (uint16_t)(int16_t)(int8_t)eax;']

    def i_cwd(self, ins):
        return ['edx = (edx & 0xFFFF0000u) | (uint16_t)((int16_t)eax >> 15);']

    def i_bswap(self, ins):
        (d,) = self.ops(ins)
        return [d.write(f'kt_bswap({d.read()})')]

    def i_xlatb(self, ins):
        return ['eax = (eax & 0xFFFFFF00u) | rd8(ebx + (uint8_t)eax);']

    def i_nop(self, ins):
        return []

    i_wait = i_nop
    i_prefetchnta = i_prefetcht0 = i_prefetcht1 = i_prefetcht2 = i_sfence = i_nop

    def i_cld(self, ins):
        return ['c->df = 0;']

    def i_std(self, ins):
        return ['c->df = 1;']

    def i_cli(self, ins):
        return []

    i_sti = i_wbinvd = i_cli

    def i_rdtsc(self, ins):
        return ['{', '    uint64_t t = KtRdtsc();', '    eax = (uint32_t)t; edx = (uint32_t)(t >> 32);', '}']

    def i_cpuid(self, ins):
        return ['KT_STORE;', 'KtCpuid(c);', 'KT_LOAD;']

    # arithmetic
    def binop(self, ins, op, expr, cc, write=True, carry=False):
        d, s = self.ops(ins)
        size = d.size
        mask = MASK[size]
        lines = ['{']
        if d.is_mem() and not d.fs():
            lines.append(f'    uint32_t ad = {d.addr()};')
            dread = f'rd{size * 8}(ad)'
            dwrite = lambda v: f'wr{size * 8}(ad, ({UT[size]})({v}));'  # noqa: E731
        else:
            dread = d.read()
            dwrite = d.write
        lines.append(f'    uint32_t a = {dread}, b = {s.read(size) if not s.is_imm() else hx(s.op.imm & mask)};')
        if carry:
            lines.append('    uint32_t ci = kt_cf(cc_op, cc_res, cc_a, cc_b, cc_cf);')
        r = expr
        if size != 4:
            r = f'({expr}) & {hx(mask)}'
        lines.append(f'    uint32_t r = {r};')
        if write:
            lines.append('    ' + dwrite('r'))
        if cc:
            lines.append('    ' + self.set_flags(cc, size, 'r', 'a', 'b') + (' cc_cf = ci;' if carry else ''))
        lines.append('}')
        return lines

    def i_add(self, ins):
        return self.binop(ins, '+', 'a + b', 'KT_CC_ADD')

    def i_adc(self, ins):
        return self.binop(ins, '+', 'a + b + ci', 'KT_CC_ADC', carry=True)

    def i_sub(self, ins):
        d, s = self.ops(ins)
        if d.is_reg() and s.is_reg() and d.reg() == s.reg():
            return [d.write('0'), self.set_flags('KT_CC_SUB', d.size, '0', '0', '0')]
        return self.binop(ins, '-', 'a - b', 'KT_CC_SUB')

    def i_sbb(self, ins):
        return self.binop(ins, '-', 'a - b - ci', 'KT_CC_SBB', carry=True)

    def i_cmp(self, ins):
        return self.binop(ins, '-', 'a - b', 'KT_CC_SUB', write=False)

    def i_and(self, ins):
        return self.binop(ins, '&', 'a & b', 'KT_CC_LOGIC')

    def i_or(self, ins):
        return self.binop(ins, '|', 'a | b', 'KT_CC_LOGIC')

    def i_xor(self, ins):
        d, s = self.ops(ins)
        if d.is_reg() and s.is_reg() and d.reg() == s.reg():
            return [d.write('0'), self.set_flags('KT_CC_LOGIC', d.size, '0')]
        return self.binop(ins, '^', 'a ^ b', 'KT_CC_LOGIC')

    def i_test(self, ins):
        return self.binop(ins, '&', 'a & b', 'KT_CC_LOGIC', write=False)

    def unop(self, ins, expr, cc, keep_cf=False):
        (d,) = self.ops(ins)
        size = d.size
        lines = ['{']
        if d.is_mem() and not d.fs():
            lines.append(f'    uint32_t ad = {d.addr()};')
            lines.append(f'    uint32_t a = rd{size * 8}(ad);')
            wr = f'wr{size * 8}(ad, ({UT[size]})r);'
        else:
            lines.append(f'    uint32_t a = {d.read()};')
            wr = d.write('r')
        r = expr if size == 4 else f'({expr}) & {hx(MASK[size])}'
        lines.append(f'    uint32_t r = {r};')
        lines.append('    ' + wr)
        if cc:
            if keep_cf:
                lines.append('    cc_cf = kt_cf(cc_op, cc_res, cc_a, cc_b, cc_cf);')
            lines.append('    ' + self.set_flags(cc, size, 'r', 'a', '1'))
        lines.append('}')
        return lines

    def i_inc(self, ins):
        return self.unop(ins, 'a + 1', 'KT_CC_INC', keep_cf=True)

    def i_dec(self, ins):
        return self.unop(ins, 'a - 1', 'KT_CC_DEC', keep_cf=True)

    def i_neg(self, ins):
        return self.unop(ins, '0u - a', 'KT_CC_NEG')

    def i_not(self, ins):
        return self.unop(ins, '~a', None)

    def i_imul(self, ins):
        o = self.ops(ins)
        if len(o) == 1:
            s = o[0]
            if s.size == 4:
                return ['{', f'    int64_t p = (int64_t)(int32_t)eax * (int32_t){s.read()};',
                        '    eax = (uint32_t)p; edx = (uint32_t)((uint64_t)p >> 32);',
                        '    cc_cf = p != (int64_t)(int32_t)eax;', '    ' + self.set_flags('KT_CC_MUL', 4, 'eax'), '}']
            if s.size == 1:
                return ['{', f'    int32_t p = (int32_t)(int8_t)eax * (int8_t){s.read()};',
                        '    eax = (eax & 0xFFFF0000u) | (uint16_t)p;', '    cc_cf = p != (int8_t)p;',
                        '    ' + self.set_flags('KT_CC_MUL', 1, '(uint32_t)p'), '}']
            if s.size == 2:
                return ['{', f'    int32_t p = (int32_t)(int16_t)eax * (int16_t){s.read()};',
                        '    eax = (eax & 0xFFFF0000u) | (uint16_t)p;',
                        '    edx = (edx & 0xFFFF0000u) | (uint16_t)((uint32_t)p >> 16);',
                        '    cc_cf = p != (int16_t)p;', '    ' + self.set_flags('KT_CC_MUL', 2, '(uint32_t)p'), '}']
            raise Unsupported('imul size')
        d = o[0]
        a, b = (o[0], o[1]) if len(o) == 2 else (o[1], o[2])
        size = d.size
        if size == 2:
            return ['{', f'    int32_t p = (int32_t)(int16_t){a.read()} * (int16_t){b.read(2)};',
                    '    uint32_t r = (uint16_t)p;', '    ' + d.write('r'), '    cc_cf = p != (int16_t)p;',
                    '    ' + self.set_flags('KT_CC_MUL', 2, 'r'), '}']
        if size != 4:
            raise Unsupported('imul size')
        return ['{', f'    int64_t p = (int64_t)(int32_t){a.read()} * (int32_t){b.read(4)};',
                '    uint32_t r = (uint32_t)p;', '    ' + d.write('r'), '    cc_cf = p != (int64_t)(int32_t)r;',
                '    ' + self.set_flags('KT_CC_MUL', 4, 'r'), '}']

    def i_mul(self, ins):
        (s,) = self.ops(ins)
        if s.size == 4:
            return ['{', f'    uint64_t p = (uint64_t)eax * {s.read()};',
                    '    eax = (uint32_t)p; edx = (uint32_t)(p >> 32);', '    cc_cf = edx != 0;',
                    '    ' + self.set_flags('KT_CC_MUL', 4, 'eax'), '}']
        if s.size == 1:
            return ['{', f'    uint32_t p = (uint32_t)(uint8_t)eax * {s.read()};',
                    '    eax = (eax & 0xFFFF0000u) | (uint16_t)p;', '    cc_cf = (p >> 8) != 0;',
                    '    ' + self.set_flags('KT_CC_MUL', 1, 'p'), '}']
        raise Unsupported('mul16')

    def i_div(self, ins):
        (s,) = self.ops(ins)
        if s.size == 4:
            return ['{', f'    uint32_t d = {s.read()};', '    uint64_t n = ((uint64_t)edx << 32) | eax;',
                    '    eax = (uint32_t)(n / d); edx = (uint32_t)(n % d);', '}']
        if s.size == 1:
            return ['{', f'    uint32_t d = {s.read()}, n = (uint16_t)eax;',
                    '    eax = (eax & 0xFFFF0000u) | (uint8_t)(n / d) | (uint32_t)(uint8_t)(n % d) << 8;', '}']
        if s.size == 2:
            return ['{', f'    uint32_t d = {s.read()}, n = ((edx & 0xFFFF) << 16) | (eax & 0xFFFF);',
                    '    eax = (eax & 0xFFFF0000u) | (uint16_t)(n / d);',
                    '    edx = (edx & 0xFFFF0000u) | (uint16_t)(n % d);', '}']
        raise Unsupported('div size')

    def i_idiv(self, ins):
        (s,) = self.ops(ins)
        if s.size == 4:
            return ['{', f'    int64_t d = (int32_t){s.read()};', '    int64_t n = (int64_t)(((uint64_t)edx << 32) | eax);',
                    '    eax = (uint32_t)(int32_t)(n / d); edx = (uint32_t)(int32_t)(n % d);', '}']
        if s.size == 1:
            return ['{', f'    int32_t d = (int8_t){s.read()}, n = (int16_t)eax;',
                    '    eax = (eax & 0xFFFF0000u) | (uint8_t)(n / d) | (uint32_t)(uint8_t)(n % d) << 8;', '}']
        raise Unsupported('idiv size')

    # shifts and rotates
    def shift(self, ins, kind):
        o = self.ops(ins)
        d = o[0]
        size = d.size
        bits = size * 8
        count = o[1].read(1) if len(o) > 1 else '1'
        lines = ['{', f'    uint32_t n = ({count}) & 31;']
        if d.is_mem() and not d.fs():
            lines.append(f'    uint32_t ad = {d.addr()};')
            lines.append(f'    uint32_t a = rd{bits}(ad);')
            wr = f'wr{bits}(ad, ({UT[size]})r);'
        else:
            lines.append(f'    uint32_t a = {d.read()};')
            wr = d.write('r')
        lines.append('    if (n) {')
        m = hx(MASK[size])
        if kind == 'shl':
            lines += [f'        uint32_t r = n < 32 ? (a << n) & {m} : 0;',
                      f'        cc_cf = n <= {bits} ? (a >> ({bits} - n)) & 1 : 0;',
                      '        ' + wr, '        ' + self.set_flags('KT_CC_SHL', size, 'r', 'a', 'n')]
        elif kind == 'shr':
            lines += ['        uint32_t r = n < 32 ? a >> n : 0;', '        cc_cf = (a >> (n - 1)) & 1;',
                      '        ' + wr, '        ' + self.set_flags('KT_CC_SHR', size, 'r', 'a', 'n')]
        elif kind == 'sar':
            lines += [f'        int32_t sa = ({ST[size]})a;',
                      f'        uint32_t r = (uint32_t)(sa >> (n < {bits} ? n : {bits - 1})) & {m};',
                      f'        cc_cf = (uint32_t)(sa >> (n - 1 < {bits} ? n - 1 : {bits - 1})) & 1;',
                      '        ' + wr, '        ' + self.set_flags('KT_CC_SAR', size, 'r', 'a', 'n')]
        elif kind in ('rol', 'ror'):
            if size != 4:
                lines += [f'        uint32_t k = n % {bits};']
                if kind == 'rol':
                    lines += [f'        uint32_t r = k ? ((a << k) | (a >> ({bits} - k))) & {m} : a;']
                else:
                    lines += [f'        uint32_t r = k ? ((a >> k) | (a << ({bits} - k))) & {m} : a;']
            else:
                lines += [f'        uint32_t r = kt_{kind}32(a, n);']
            cf = '(r & 1)' if kind == 'rol' else f'((r >> {bits - 1}) & 1)'
            of = f'(((r >> {bits - 1}) ^ r) & 1)' if kind == 'rol' else f'(((r >> {bits - 1}) ^ (r >> {bits - 2})) & 1)'
            lines += ['        ' + wr,
                      f'        uint32_t f = ({FLAGS_ALL} & ~(KT_F_CF | KT_F_OF)) | {cf} | {of} << 11;',
                      '        ' + self.set_flags('KT_CC_EFLAGS', 4, 'f')]
        elif kind in ('rcl', 'rcr'):
            lines += [f'        uint32_t f = {FLAGS_ALL};', '        uint32_t cf = f & 1, r = a;',
                      f'        for (uint32_t i = 0; i < n % {bits + 1}; i++) {{']
            if kind == 'rcr':
                lines += ['            uint32_t out = r & 1;',
                          f'            r = (r >> 1) | (cf << {bits - 1});', '            cf = out;']
            else:
                lines += [f'            uint32_t out = (r >> {bits - 1}) & 1;',
                          f'            r = ((r << 1) | cf) & {m};', '            cf = out;']
            lines += ['        }', '        ' + wr, '        f = (f & ~1u) | cf;',
                      '        ' + self.set_flags('KT_CC_EFLAGS', 4, 'f')]
        lines += ['    }', '}']
        return lines

    def i_shl(self, ins):
        return self.shift(ins, 'shl')

    i_sal = i_shl

    def i_shr(self, ins):
        return self.shift(ins, 'shr')

    def i_sar(self, ins):
        return self.shift(ins, 'sar')

    def i_rol(self, ins):
        return self.shift(ins, 'rol')

    def i_ror(self, ins):
        return self.shift(ins, 'ror')

    def i_rcr(self, ins):
        return self.shift(ins, 'rcr')

    def i_rcl(self, ins):
        return self.shift(ins, 'rcl')

    def i_shld(self, ins):
        d, s, n = self.ops(ins)
        if d.size != 4:
            raise Unsupported('shld16')
        return ['{', f'    uint32_t n = ({n.read(1)}) & 31, a = {d.read()}, b = {s.read()};', '    if (n) {',
                '        uint32_t r = (a << n) | (b >> (32 - n));', '        cc_cf = (a >> (32 - n)) & 1;',
                '        ' + d.write('r'), '        ' + self.set_flags('KT_CC_SHL', 4, 'r', 'a', 'n'), '    }', '}']

    def i_shrd(self, ins):
        d, s, n = self.ops(ins)
        if d.size != 4:
            raise Unsupported('shrd16')
        return ['{', f'    uint32_t n = ({n.read(1)}) & 31, a = {d.read()}, b = {s.read()};', '    if (n) {',
                '        uint32_t r = (a >> n) | (b << (32 - n));', '        cc_cf = (a >> (n - 1)) & 1;',
                '        ' + d.write('r'), '        ' + self.set_flags('KT_CC_SHR', 4, 'r', 'a', 'n'), '    }', '}']

    # bit operations
    def bitop(self, ins, update):
        d, s = self.ops(ins)
        if d.is_mem():
            # Register offsets address a bit string (signed, in dwords from
            # the operand); immediate offsets stay within the operand.
            if d.size != 4:
                raise Unsupported('bt memory size')
            if s.is_imm():
                ad = d.addr()
                n = f'{s.op.imm & 31}u'
            else:
                ad = f'{d.addr()} + (uint32_t)(((int32_t){s.read(4)} >> 5) * 4)'
                n = f'({s.read(4)}) & 31'
            lines = ['{', f'    uint32_t ad = {ad}, n = {n}, a = rd32(ad);',
                     f'    uint32_t f = ({FLAGS_ALL} & ~1u) | ((a >> n) & 1);']
            if update:
                lines.append(f'    wr32(ad, {update});')
            lines += ['    ' + self.set_flags('KT_CC_EFLAGS', 4, 'f'), '}']
            return lines
        lines = ['{', f'    uint32_t a = {d.read()}, n = ({s.read(4)}) & {d.size * 8 - 1};',
                 f'    uint32_t f = ({FLAGS_ALL} & ~1u) | ((a >> n) & 1);']
        if update:
            lines.append('    ' + d.write(update))
        lines += ['    ' + self.set_flags('KT_CC_EFLAGS', 4, 'f'), '}']
        return lines

    def i_bt(self, ins):
        return self.bitop(ins, None)

    def i_bts(self, ins):
        return self.bitop(ins, 'a | (1u << n)')

    def i_btr(self, ins):
        return self.bitop(ins, 'a & ~(1u << n)')

    def i_bsf(self, ins):
        d, s = self.ops(ins)
        return ['{', f'    uint32_t v = {s.read()};', f'    uint32_t f = {FLAGS_ALL} & ~KT_F_ZF;',
                '    if (v) { ' + d.write('kt_bsf(v)') + ' } else f |= KT_F_ZF;',
                '    ' + self.set_flags('KT_CC_EFLAGS', 4, 'f'), '}']

    def i_bsr(self, ins):
        d, s = self.ops(ins)
        return ['{', f'    uint32_t v = {s.read()};', f'    uint32_t f = {FLAGS_ALL} & ~KT_F_ZF;',
                '    if (v) { ' + d.write('kt_bsr(v)') + ' } else f |= KT_F_ZF;',
                '    ' + self.set_flags('KT_CC_EFLAGS', 4, 'f'), '}']

    def i_cmpxchg(self, ins):
        d, s = self.ops(ins)
        if d.size != 4:
            raise Unsupported('cmpxchg size')
        return ['{', f'    uint32_t a = eax, b = {d.read()}, r = a - b;',
                '    ' + self.set_flags('KT_CC_SUB', 4, 'r', 'a', 'b'),
                '    if (r == 0) { ' + d.write(s.read()) + ' } else eax = b;', '}']

    def i_xadd(self, ins):
        d, s = self.ops(ins)
        if d.size != 4:
            raise Unsupported('xadd size')
        return ['{', f'    uint32_t a = {d.read()}, b = {s.read()}, r = a + b;', '    ' + s.write('a'),
                '    ' + d.write('r'), '    ' + self.set_flags('KT_CC_ADD', 4, 'r', 'a', 'b'), '}']

    # string instructions
    def string(self, ins, prefix, kind, size):
        step = f'(c->df ? (uint32_t)-{size} : {size}u)'
        rd, wr = f'rd{size * 8}', f'wr{size * 8}'
        mask = hx(MASK[size])
        if kind == 'movs':
            body = [f'{wr}(edi, {rd}(esi));', f'esi += {step}; edi += {step};']
        elif kind == 'stos':
            body = [f'{wr}(edi, ({UT[size]})eax);', f'edi += {step};']
        elif kind == 'lods':
            keep = {1: 'eax & 0xFFFFFF00u', 2: 'eax & 0xFFFF0000u', 4: '0'}[size]
            body = [f'eax = ({keep}) | {rd}(esi);', f'esi += {step};']
        elif kind == 'cmps':
            body = [f'{{ uint32_t a = {rd}(esi), b = {rd}(edi), r = (a - b) & {mask};',
                    '  ' + self.set_flags('KT_CC_SUB', size, 'r', 'a', 'b') + ' }',
                    f'esi += {step}; edi += {step};']
        elif kind == 'scas':
            body = [f'{{ uint32_t a = eax & {mask}, b = {rd}(edi), r = (a - b) & {mask};',
                    '  ' + self.set_flags('KT_CC_SUB', size, 'r', 'a', 'b') + ' }', f'edi += {step};']
        else:
            raise Unsupported(kind)
        if not prefix:
            return ['{'] + ['    ' + b for b in body] + ['}']
        lines = ['while (ecx) {'] + ['    ' + b for b in body] + ['    ecx--;']
        if kind in ('cmps', 'scas'):
            if prefix == 'repe':
                lines.append('    if (!kt_zf(cc_op, cc_res)) break;')
            elif prefix == 'repne':
                lines.append('    if (kt_zf(cc_op, cc_res)) break;')
        lines.append('}')
        return lines

    def _str(kind, size):  # noqa: N805
        def h(self, ins, prefix=''):
            return self.string(ins, prefix, kind, size)
        return h

    i_movsb, i_movsw, i_movsd = _str('movs', 1), _str('movs', 2), _str('movs', 4)
    i_stosb, i_stosw, i_stosd = _str('stos', 1), _str('stos', 2), _str('stos', 4)
    i_lodsb, i_lodsw, i_lodsd = _str('lods', 1), _str('lods', 2), _str('lods', 4)
    i_cmpsb, i_cmpsw, i_cmpsd = _str('cmps', 1), _str('cmps', 2), _str('cmps', 4)
    i_scasb, i_scasw, i_scasd = _str('scas', 1), _str('scas', 2), _str('scas', 4)

    # control flow
    def jcc(self, ins, cc):
        t = ins.operands[0].imm & 0xFFFFFFFF
        return [f'if ({COND.format(cc=cc)}) {{ {self.goto(t)} }}']

    def i_jecxz(self, ins):
        t = ins.operands[0].imm & 0xFFFFFFFF
        return [f'if (ecx == 0) {{ {self.goto(t)} }}']

    def i_jmp(self, ins):
        (o,) = self.ops(ins)
        if o.is_imm():
            return [self.goto(o.op.imm & 0xFFFFFFFF)]
        if ins.address in self.tables:
            reg, targets, first = self.tables[ins.address]
            lines = [f'switch ((int32_t){reg}) {{']
            for i, t in enumerate(targets):
                if t is None:
                    continue
                self.used.add(t)
                lines.append(f'    case {first + i}: goto L_{t:08x};')
            lines += [f'    default: KT_STORE; KtTrap(c, 0x{ins.address:08x}u, "jump table"); return;', '}']
            return lines
        # Unresolved indirect jump: a tail call when it lands on a function
        # entry (the stack top is then the return address), otherwise the
        # rest of the function continues as original code.
        # Targets inside this function (e.g. tables indexed backwards, as in
        # the CRT memmove) stay in C via a switch over its instructions.
        local = [f'    switch (t) {{']
        for x in sorted(self.insns):
            if x != ins.address:
                self.used.add(x)
                local.append(f'        case 0x{x:08x}u: goto L_{x:08x};')
        local.append('        default: break;')
        local.append('    }')
        return ['{', f'    uint32_t t = {o.read(4)};'] + local + ['    KT_STORE;', '    KtJumpIndirect(c, t);',
                                                                      '    return;', '}']

    def i_call(self, ins):
        (o,) = self.ops(ins)
        nxt = ins.address + ins.size
        if o.is_imm():
            t = o.op.imm & 0xFFFFFFFF
            if t == nxt:  # call $+5 (get EIP): push and fall through
                return [f'esp -= 4; wr32(esp, 0x{nxt:08x}u);']
            return [f'@@CALL {t:08x} {nxt:08x}@@']
        return ['{', f'    uint32_t t = {o.read(4)};', '    KT_STORE;',
                f'    KtCallIndirect(c, t, 0x{nxt:08x}u);', '    KT_LOAD;', '}']

    def i_ret(self, ins):
        n = ins.operands[0].imm if ins.operands else 0
        return ['KT_STORE;', f'c->eip = rd32(c->esp); c->esp += {4 + n}u;', 'return;']

    def i_leave(self, ins):
        return ['esp = ebp;', 'ebp = rd32(esp); esp += 4;']

    def i_int(self, ins):
        n = ins.operands[0].imm & 0xFF
        if n == 0x2D:  # debug service (OutputDebugString): no debugger attached
            return []
        return ['KT_STORE;', f'KtTrap(c, 0x{ins.address:08x}u, "int 0x{n:x}");', 'return;']

    def i_in(self, ins):
        return ['KT_STORE;', f'KtTrap(c, 0x{ins.address:08x}u, "port input");', 'return;']

    i_out = i_in

    def i_retf(self, ins):  # far return: not used by flat-model code
        return ['KT_STORE;', f'KtTrap(c, 0x{ins.address:08x}u, "retf");', 'return;']

    def i_fnsave(self, ins):
        (o,) = self.ops(ins)
        return [f'kt_fnsave(c, {o.addr()});']

    def i_frstor(self, ins):
        (o,) = self.ops(ins)
        return [f'kt_frstor(c, {o.addr()});']

    def i_int3(self, ins):
        return ['KT_STORE;', f'KtTrap(c, 0x{ins.address:08x}u, "int3");', 'return;']

    def i_hlt(self, ins):
        return ['KT_STORE;', f'KtTrap(c, 0x{ins.address:08x}u, "hlt");', 'return;']

    i_ud2 = i_hlt


def resolve_calls(code, lifted, index):
    """Replaces @@CALL/@@TAIL placeholders once the lifted set is known."""

    def call(m):
        t, nxt = int(m.group(1), 16), int(m.group(2), 16)
        if t in lifted:
            return (f'KT_STORE; if (kt_on_{t:08x}) {{ c->esp -= 4; wr32(c->esp, 0x{nxt:08x}u); f_{t:08x}(c); }} '
                    f'else KtCallNative(c, 0x{t:08x}u); KT_LOAD;')
        return f'KT_STORE; KtCallNative(c, 0x{t:08x}u); KT_LOAD;'

    def tail(m):
        t = int(m.group(1), 16)
        native = (f'{{ uint32_t r = rd32(c->esp); c->esp += 4; KtCallNative(c, 0x{t:08x}u); c->eip = r; }} return;')
        if t in lifted:
            return f'KT_STORE; if (kt_on_{t:08x}) {{ f_{t:08x}(c); return; }} {native}'
        return f'KT_STORE; {native}'

    code = re.sub(r'@@CALL ([0-9a-f]{8}) ([0-9a-f]{8})@@', call, code)
    return re.sub(r'@@TAIL ([0-9a-f]{8})@@', tail, code)


# ---- x87 ----------------------------------------------------------------------------------

def _fpu_reg(name):
    m = re.fullmatch(r'st\((\d)\)', name)
    return int(m.group(1)) if m else None


def _fval(op, ins):
    """C expression for an x87 source operand (st(i) or memory float)."""
    if op.is_reg():
        i = _fpu_reg(op.reg())
        if i is None:
            raise Unsupported(f'x87 operand {op.reg()}')
        return f'ST({i})'
    a = op.addr()
    if op.size == 4:
        return f'(double)rdf32({a})'
    if op.size == 8:
        return f'rdf64({a})'
    if op.size == 10:
        return f'kt_f80_load({a})'
    raise Unsupported(f'x87 memory size {op.size}')


def _fint(op):
    a = op.addr()
    return {2: f'(double)(int16_t)rd16({a})', 4: f'(double)(int32_t)rd32({a})',
            8: f'(double)(int64_t)rd64({a})'}[op.size]


def _fstore(op, value):
    if op.is_reg():
        return f'ST({_fpu_reg(op.reg())}) = {value};'
    a = op.addr()
    if op.size == 4:
        return f'wrf32({a}, (float)({value}));'
    if op.size == 8:
        return f'wrf64({a}, {value});'
    if op.size == 10:
        return f'kt_f80_store({a}, {value});'
    raise Unsupported(f'x87 store size {op.size}')


def _farith(sym, rev, pop, integer=False):
    def h(self, ins):
        o = self.ops(ins)
        if len(o) == 2:
            dst, src = f'ST({_fpu_reg(o[0].reg())})', _fval(o[1], ins)
        elif len(o) == 1:
            if pop:
                dst, src = f'ST({_fpu_reg(o[0].reg())})', 'ST(0)'
            elif o[0].is_mem():
                dst, src = 'ST(0)', (_fint(o[0]) if integer else _fval(o[0], ins))
            else:
                dst, src = 'ST(0)', _fval(o[0], ins)
        else:
            dst, src = 'ST(1)', 'ST(0)'
        expr = f's {sym} d' if rev else f'd {sym} s'
        lines = ['{', f'    double s = {src}, d = {dst};', f'    {dst} = {expr};', '}']
        if pop:
            lines.append('kt_fpop(c);')
        return lines
    return h


def _fcom(pops):
    def h(self, ins):
        o = self.ops(ins)
        src = _fval(o[0], ins) if o else 'ST(1)'
        return [f'kt_fcom(c, ST(0), {src});'] + ['kt_fpop(c);'] * pops
    return h


def _funary(expr):
    def h(self, ins):
        return [f'ST(0) = {expr};']
    return h


def _fconst(value):
    def h(self, ins):
        return [f'kt_fpush(c, {value});']
    return h


def _fld(self, ins):
    (o,) = self.ops(ins)
    return ['{', f'    double v = {_fval(o, ins)};', '    kt_fpush(c, v);', '}']


def _fild(self, ins):
    (o,) = self.ops(ins)
    return ['{', f'    double v = {_fint(o)};', '    kt_fpush(c, v);', '}']


def _fst(pop):
    def h(self, ins):
        (o,) = self.ops(ins)
        return [_fstore(o, 'ST(0)')] + (['kt_fpop(c);'] if pop else [])
    return h


def _fist(pop):
    def h(self, ins):
        (o,) = self.ops(ins)
        a = o.addr()
        store = {2: f'wr16({a}, (uint16_t)kt_fist16(c, ST(0)));', 4: f'wr32({a}, (uint32_t)kt_fist32(c, ST(0)));',
                 8: f'wr64({a}, (uint64_t)kt_fist64(c, ST(0)));'}[o.size]
        return [store] + (['kt_fpop(c);'] if pop else [])
    return h


def _fxch(self, ins):
    o = self.ops(ins)
    i = max(_fpu_reg(x.reg()) for x in o) if o else 1
    return ['{', f'    double t = ST(0); ST(0) = ST({i}); ST({i}) = t;', '}']


def _ffree(self, ins):
    (o,) = self.ops(ins)
    return [f'c->ftag &= ~(1u << ((c->ftop + {_fpu_reg(o.reg())}) & 7));']


def _fnstsw(self, ins):
    (o,) = self.ops(ins)
    if o.is_reg():
        return ['eax = (eax & 0xFFFF0000u) | (uint16_t)kt_fstsw(c);']
    return [f'wr16({o.addr()}, (uint16_t)kt_fstsw(c));']


def _fnstcw(self, ins):
    (o,) = self.ops(ins)
    return [f'wr16({o.addr()}, (uint16_t)c->fcw);']


def _fldcw(self, ins):
    (o,) = self.ops(ins)
    return [f'c->fcw = rd16({o.addr()});']


X87 = {
    'fld': _fld, 'fild': _fild, 'fst': _fst(False), 'fstp': _fst(True), 'fist': _fist(False), 'fistp': _fist(True),
    'fadd': _farith('+', False, False), 'faddp': _farith('+', False, True),
    'fsub': _farith('-', False, False), 'fsubp': _farith('-', False, True),
    'fsubr': _farith('-', True, False), 'fsubrp': _farith('-', True, True),
    'fmul': _farith('*', False, False), 'fmulp': _farith('*', False, True),
    'fdiv': _farith('/', False, False), 'fdivp': _farith('/', False, True),
    'fdivr': _farith('/', True, False), 'fdivrp': _farith('/', True, True),
    'fiadd': _farith('+', False, False, True), 'fisub': _farith('-', False, False, True),
    'fisubr': _farith('-', True, False, True), 'fimul': _farith('*', False, False, True),
    'fidiv': _farith('/', False, False, True), 'fidivr': _farith('/', True, False, True),
    'fcom': _fcom(0), 'fcomp': _fcom(1), 'fcompp': _fcom(2), 'fucom': _fcom(0), 'fucomp': _fcom(1),
    'fucompp': _fcom(2),
    'ftst': lambda self, ins: ['kt_fcom(c, ST(0), 0.0);'],
    'fxam': lambda self, ins: ['kt_fxam(c);'],
    'fchs': _funary('-ST(0)'), 'fabs': _funary('fabs(ST(0))'), 'fsqrt': _funary('sqrt(ST(0))'),
    'fsin': _funary('sin(ST(0))'), 'fcos': _funary('cos(ST(0))'), 'frndint': _funary('kt_fround(c, ST(0))'),
    'f2xm1': _funary('exp2(ST(0)) - 1.0'),
    'fscale': _funary('ldexp(ST(0), (int)trunc(ST(1)))'),
    'fsincos': lambda self, ins: ['{', '    double v = ST(0);', '    ST(0) = sin(v);', '    kt_fpush(c, cos(v));', '}'],
    'fptan': lambda self, ins: ['ST(0) = tan(ST(0));', 'kt_fpush(c, 1.0);'],
    'fpatan': lambda self, ins: ['{', '    double x = ST(0), y = ST(1);', '    kt_fpop(c);', '    ST(0) = atan2(y, x);', '}'],
    'fyl2x': lambda self, ins: ['{', '    double x = ST(0), y = ST(1);', '    kt_fpop(c);', '    ST(0) = y * log2(x);', '}'],
    'fld1': _fconst('1.0'), 'fldz': _fconst('0.0'), 'fldpi': _fconst('3.14159265358979323846'),
    'fldl2e': _fconst('1.44269504088896340736'), 'fldln2': _fconst('0.69314718055994530942'),
    'fldlg2': _fconst('0.30102999566398119521'), 'fldl2t': _fconst('3.32192809488736234787'),
    'fxch': _fxch, 'ffree': _ffree, 'fnstsw': _fnstsw, 'fnstcw': _fnstcw, 'fldcw': _fldcw,
    'fnclex': lambda self, ins: ['c->fsw &= ~0x80FFu;'],
    # Partial remainder: done in one step (C2 = 0); C0 C3 C1 = quotient bits 2 1 0.
    'fprem': lambda self, ins: ['kt_fprem(c, 0);'],
    'fprem1': lambda self, ins: ['kt_fprem(c, 1);'],
    'fwait': lambda self, ins: [],
}
for _name, _h in X87.items():
    setattr(FunctionLifter, 'i_' + _name, _h)
