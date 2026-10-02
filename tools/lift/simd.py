"""MMX and SSE (Pentium III) translation for tools/lift/x86c.py.

MMX registers live in c->mm[], XMM registers in c->xmm[] (see kt_lift.h);
operations are written per lane in plain C so the output stays portable.
Both register files are treated as volatile across calls, as the 2001-era
Xbox compilers do.
"""
import re

from capstone import x86

import os as _os, sys as _sys; _sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))  # embedded Python does not add the script directory
from x86c import FunctionLifter, Unsupported


def _mm(name):
    m = re.fullmatch(r'mm(\d)', name)
    return int(m.group(1)) if m else None


def _xmm(name):
    m = re.fullmatch(r'xmm(\d)', name)
    return int(m.group(1)) if m else None


def _src64(op):
    """KtMmx-valued C expression for an MMX source (register or m64)."""
    if op.is_reg():
        i = _mm(op.reg())
        if i is None:
            raise Unsupported(f'mmx operand {op.reg()}')
        return f'c->mm[{i}]'
    return f'kt_mmx_ld({op.addr()})'


def _src128(op):
    if op.is_reg():
        i = _xmm(op.reg())
        if i is None:
            raise Unsupported(f'sse operand {op.reg()}')
        return f'c->xmm[{i}]'
    if op.size == 16:
        return f'kt_xmm_ld({op.addr()})'
    if op.size == 8:
        return f'kt_xmm_ld64({op.addr()})'
    if op.size == 4:
        return f'kt_xmm_ld32({op.addr()})'
    raise Unsupported(f'sse memory size {op.size}')


# Packed integer ops: lane field, lane count, result expression of a, b.
PACKED = {
    'paddb': ('b', 8, 'a + b'), 'paddw': ('w', 4, 'a + b'), 'paddd': ('d', 2, 'a + b'),
    'psubb': ('b', 8, 'a - b'), 'psubw': ('w', 4, 'a - b'), 'psubd': ('d', 2, 'a - b'),
    'paddsb': ('sb', 8, 'kt_sat8(a + b)'), 'paddsw': ('sw', 4, 'kt_sat16(a + b)'),
    'psubsb': ('sb', 8, 'kt_sat8(a - b)'), 'psubsw': ('sw', 4, 'kt_sat16(a - b)'),
    'paddusb': ('b', 8, 'kt_satu8(a + b)'), 'paddusw': ('w', 4, 'kt_satu16(a + b)'),
    'psubusb': ('b', 8, 'kt_satu8(a - b)'), 'psubusw': ('w', 4, 'kt_satu16(a - b)'),
    'pmullw': ('sw', 4, '(int16_t)(a * b)'), 'pmulhw': ('sw', 4, '(int16_t)((a * b) >> 16)'),
    'pmulhuw': ('w', 4, '(uint16_t)(((uint32_t)a * b) >> 16)'),
    'pcmpeqb': ('b', 8, 'a == b ? 0xFF : 0'), 'pcmpeqw': ('w', 4, 'a == b ? 0xFFFF : 0'),
    'pcmpeqd': ('d', 2, 'a == b ? 0xFFFFFFFFu : 0'),
    'pcmpgtb': ('sb', 8, 'a > b ? -1 : 0'), 'pcmpgtw': ('sw', 4, 'a > b ? -1 : 0'),
    'pcmpgtd': ('sd', 2, 'a > b ? -1 : 0'),
    'pavgb': ('b', 8, '(a + b + 1) >> 1'), 'pavgw': ('w', 4, '(a + b + 1) >> 1'),
    'pminub': ('b', 8, 'a < b ? a : b'), 'pmaxub': ('b', 8, 'a > b ? a : b'),
    'pminsw': ('sw', 4, 'a < b ? a : b'), 'pmaxsw': ('sw', 4, 'a > b ? a : b'),
}
LOGIC = {'pand': 'a & b', 'por': 'a | b', 'pxor': 'a ^ b', 'pandn': '~a & b'}
# Shifts: lane field, lane count, bits, kind.
SHIFTS = {
    'psllw': ('w', 4, 16, 'l'), 'pslld': ('d', 2, 32, 'l'), 'psllq': ('q', 1, 64, 'l'),
    'psrlw': ('w', 4, 16, 'r'), 'psrld': ('d', 2, 32, 'r'), 'psrlq': ('q', 1, 64, 'r'),
    'psraw': ('sw', 4, 16, 'a'), 'psrad': ('sd', 2, 32, 'a'),
}
# SSE packed / scalar float ops.
FLOAT = {'add': 'a + b', 'sub': 'a - b', 'mul': 'a * b', 'div': 'a / b', 'min': 'a < b ? a : b',
         'max': 'a > b ? a : b'}
UNARY = {'sqrt': 'sqrtf(b)', 'rsqrt': '1.0f / sqrtf(b)', 'rcp': '1.0f / b'}
FLOAT_LOGIC = {'andps': 'a & b', 'andnps': '~a & b', 'orps': 'a | b', 'xorps': 'a ^ b'}
# cmpXXss/ps predicate number -> C comparison.
CMP = {'eq': 'a == b', 'lt': 'a < b', 'le': 'a <= b', 'unord': 'a != a || b != b', 'neq': '!(a == b)',
       'nlt': '!(a < b)', 'nle': '!(a <= b)', 'ord': 'a == a && b == b'}


def _dst_mm(self, ins):
    o = self.ops(ins)
    i = _mm(o[0].reg()) if o[0].is_reg() else None
    if i is None:
        raise Unsupported(f'{ins.mnemonic} destination')
    return o, i


def _packed(name):
    field, n, expr = PACKED[name]
    def h(self, ins):
        o, d = _dst_mm(self, ins)
        return ['{', f'    KtMmx s = {_src64(o[1])}, r = c->mm[{d}];',
                f'    for (int i = 0; i < {n}; i++) {{ int32_t a = r.{field}[i], b = s.{field}[i]; '
                f'r.{field}[i] = ({expr}); }}', f'    c->mm[{d}] = r;', '}']
    return h


def _logic(name):
    expr = LOGIC[name]
    def h(self, ins):
        o = self.ops(ins)
        if o[0].is_reg() and _xmm(o[0].reg()) is not None:
            d = _xmm(o[0].reg())
            return ['{', f'    KtXmm s = {_src128(o[1])};',
                    f'    for (int i = 0; i < 4; i++) {{ uint32_t a = c->xmm[{d}].d[i], b = s.d[i]; '
                    f'c->xmm[{d}].d[i] = {expr}; }}', '}']
        o, d = _dst_mm(self, ins)
        return ['{', f'    uint64_t a = c->mm[{d}].q, b = ({_src64(o[1])}).q;', f'    c->mm[{d}].q = {expr};', '}']
    return h


def _shift(name):
    field, n, bits, kind = SHIFTS[name]
    def h(self, ins):
        o, d = _dst_mm(self, ins)
        cnt = f'(uint64_t){o[1].op.imm & 0xFF}' if o[1].is_imm() else f'({_src64(o[1])}).q'
        lane = 'r.q' if field == 'q' else f'r.{field}[i]'  # KtMmx.q is a scalar
        if kind == 'l':
            body = f'{lane} = n >= {bits} ? 0 : {lane} << n;'
        elif kind == 'r':
            body = f'{lane} = n >= {bits} ? 0 : {lane} >> n;'
        else:
            body = f'{lane} = {lane} >> (n >= {bits} ? {bits - 1} : n);'
        return ['{', f'    uint64_t n = {cnt};', f'    KtMmx r = c->mm[{d}];',
                f'    for (int i = 0; i < {n}; i++) {body}', f'    c->mm[{d}] = r;', '}']
    return h


def _unpack(field, n, high):
    def h(self, ins):
        o, d = _dst_mm(self, ins)
        base = n // 2 if high else 0
        return ['{', f'    KtMmx s = {_src64(o[1])}, a = c->mm[{d}], r;',
                f'    for (int i = 0; i < {n // 2}; i++) {{ r.{field}[2 * i] = a.{field}[{base} + i]; '
                f'r.{field}[2 * i + 1] = s.{field}[{base} + i]; }}', f'    c->mm[{d}] = r;', '}']
    return h


def i_packssdw(self, ins):
    o, d = _dst_mm(self, ins)
    return ['{', f'    KtMmx s = {_src64(o[1])}, a = c->mm[{d}], r;',
            '    r.sw[0] = kt_sat16(a.sd[0]); r.sw[1] = kt_sat16(a.sd[1]);',
            '    r.sw[2] = kt_sat16(s.sd[0]); r.sw[3] = kt_sat16(s.sd[1]);', f'    c->mm[{d}] = r;', '}']


def i_packsswb(self, ins):
    o, d = _dst_mm(self, ins)
    return ['{', f'    KtMmx s = {_src64(o[1])}, a = c->mm[{d}], r;',
            '    for (int i = 0; i < 4; i++) { r.sb[i] = kt_sat8(a.sw[i]); r.sb[4 + i] = kt_sat8(s.sw[i]); }',
            f'    c->mm[{d}] = r;', '}']


def i_packuswb(self, ins):
    o, d = _dst_mm(self, ins)
    return ['{', f'    KtMmx s = {_src64(o[1])}, a = c->mm[{d}], r;',
            '    for (int i = 0; i < 4; i++) { r.b[i] = kt_satu8(a.sw[i]); r.b[4 + i] = kt_satu8(s.sw[i]); }',
            f'    c->mm[{d}] = r;', '}']


def i_pmaddwd(self, ins):
    o, d = _dst_mm(self, ins)
    return ['{', f'    KtMmx s = {_src64(o[1])}, a = c->mm[{d}], r;',
            '    for (int i = 0; i < 2; i++) r.sd[i] = (int32_t)((uint32_t)(a.sw[2 * i] * s.sw[2 * i]) + '
            '(uint32_t)(a.sw[2 * i + 1] * s.sw[2 * i + 1]));', f'    c->mm[{d}] = r;', '}']


def i_pshufw(self, ins):
    o, d = _dst_mm(self, ins)
    imm = o[2].op.imm & 0xFF
    return ['{', f'    KtMmx s = {_src64(o[1])}, r;',
            '    ' + ' '.join(f'r.w[{i}] = s.w[{(imm >> (2 * i)) & 3}];' for i in range(4)), f'    c->mm[{d}] = r;',
            '}']


def i_movq(self, ins):
    o = self.ops(ins)
    if o[0].is_reg() and _mm(o[0].reg()) is not None:
        return [f'c->mm[{_mm(o[0].reg())}] = {_src64(o[1])};']
    if o[0].is_mem() and o[1].is_reg() and _mm(o[1].reg()) is not None:
        return [f'wr64({o[0].addr()}, c->mm[{_mm(o[1].reg())}].q);']
    raise Unsupported('movq form')


def i_movntq(self, ins):
    return i_movq(self, ins)


def i_movd(self, ins):
    o = self.ops(ins)
    if o[0].is_reg() and _mm(o[0].reg()) is not None:
        d = _mm(o[0].reg())
        return [f'c->mm[{d}].q = (uint64_t)({o[1].read(4)});']
    if o[1].is_reg() and _mm(o[1].reg()) is not None:
        return [o[0].write(f'c->mm[{_mm(o[1].reg())}].d[0]')]
    if o[0].is_reg() and _xmm(o[0].reg()) is not None:
        d = _xmm(o[0].reg())
        return [f'memset(&c->xmm[{d}], 0, 16); c->xmm[{d}].d[0] = {o[1].read(4)};']
    if o[1].is_reg() and _xmm(o[1].reg()) is not None:
        return [o[0].write(f'c->xmm[{_xmm(o[1].reg())}].d[0]')]
    raise Unsupported('movd form')


def i_emms(self, ins):
    return ['c->ftag = 0;']


# ---- SSE ----------------------------------------------------------------------------------

def _dst_xmm(self, ins):
    o = self.ops(ins)
    i = _xmm(o[0].reg()) if o[0].is_reg() else None
    if i is None:
        raise Unsupported(f'{ins.mnemonic} destination')
    return o, i


def _movx(scalar):
    def h(self, ins):
        o = self.ops(ins)
        if o[0].is_reg():
            d = _xmm(o[0].reg())
            if scalar and o[1].is_mem():  # movss xmm, m32 zeroes the upper lanes
                return [f'memset(&c->xmm[{d}], 0, 16); c->xmm[{d}].d[0] = rd32({o[1].addr()});']
            if scalar:
                return [f'c->xmm[{d}].d[0] = c->xmm[{_xmm(o[1].reg())}].d[0];']
            return [f'c->xmm[{d}] = {_src128(o[1])};']
        s = _xmm(o[1].reg())
        if scalar:
            return [f'wr32({o[0].addr()}, c->xmm[{s}].d[0]);']
        return [f'kt_xmm_st({o[0].addr()}, c->xmm[{s}]);']
    return h


def i_movlps(self, ins):
    o = self.ops(ins)
    if o[0].is_reg():
        return [f'c->xmm[{_xmm(o[0].reg())}].q[0] = rd64({o[1].addr()});']
    return [f'wr64({o[0].addr()}, c->xmm[{_xmm(o[1].reg())}].q[0]);']


def i_movhps(self, ins):
    o = self.ops(ins)
    if o[0].is_reg():
        return [f'c->xmm[{_xmm(o[0].reg())}].q[1] = rd64({o[1].addr()});']
    return [f'wr64({o[0].addr()}, c->xmm[{_xmm(o[1].reg())}].q[1]);']


def i_movhlps(self, ins):
    o, d = _dst_xmm(self, ins)
    return [f'c->xmm[{d}].q[0] = c->xmm[{_xmm(o[1].reg())}].q[1];']


def i_movlhps(self, ins):
    o, d = _dst_xmm(self, ins)
    return [f'c->xmm[{d}].q[1] = c->xmm[{_xmm(o[1].reg())}].q[0];']


def _fop(op, packed):
    expr = FLOAT[op]
    def h(self, ins):
        o, d = _dst_xmm(self, ins)
        n = 4 if packed else 1
        return ['{', f'    KtXmm s = {_src128(o[1])};',
                f'    for (int i = 0; i < {n}; i++) {{ float a = c->xmm[{d}].f[i], b = s.f[i]; '
                f'c->xmm[{d}].f[i] = {expr}; }}', '}']
    return h


def _funop(op, packed):
    expr = UNARY[op]
    def h(self, ins):
        o, d = _dst_xmm(self, ins)
        n = 4 if packed else 1
        return ['{', f'    KtXmm s = {_src128(o[1])};',
                f'    for (int i = 0; i < {n}; i++) {{ float b = s.f[i]; c->xmm[{d}].f[i] = {expr}; }}', '}']
    return h


def _flogic(name):
    expr = FLOAT_LOGIC[name]
    def h(self, ins):
        o, d = _dst_xmm(self, ins)
        return ['{', f'    KtXmm s = {_src128(o[1])};',
                f'    for (int i = 0; i < 4; i++) {{ uint32_t a = c->xmm[{d}].d[i], b = s.d[i]; '
                f'c->xmm[{d}].d[i] = {expr}; }}', '}']
    return h


def _fcmp(pred, packed):
    expr = CMP[pred]
    def h(self, ins):
        o, d = _dst_xmm(self, ins)
        n = 4 if packed else 1
        return ['{', f'    KtXmm s = {_src128(o[1])};',
                f'    for (int i = 0; i < {n}; i++) {{ float a = c->xmm[{d}].f[i], b = s.f[i]; '
                f'c->xmm[{d}].d[i] = ({expr}) ? 0xFFFFFFFFu : 0; }}', '}']
    return h


def i_shufps(self, ins):
    o, d = _dst_xmm(self, ins)
    imm = o[2].op.imm & 0xFF
    return ['{', f'    KtXmm s = {_src128(o[1])}, a = c->xmm[{d}], r;',
            f'    r.d[0] = a.d[{imm & 3}]; r.d[1] = a.d[{(imm >> 2) & 3}];',
            f'    r.d[2] = s.d[{(imm >> 4) & 3}]; r.d[3] = s.d[{(imm >> 6) & 3}];', f'    c->xmm[{d}] = r;', '}']


def i_unpcklps(self, ins):
    o, d = _dst_xmm(self, ins)
    return ['{', f'    KtXmm s = {_src128(o[1])}, a = c->xmm[{d}], r;',
            '    r.d[0] = a.d[0]; r.d[1] = s.d[0]; r.d[2] = a.d[1]; r.d[3] = s.d[1];', f'    c->xmm[{d}] = r;', '}']


def i_unpckhps(self, ins):
    o, d = _dst_xmm(self, ins)
    return ['{', f'    KtXmm s = {_src128(o[1])}, a = c->xmm[{d}], r;',
            '    r.d[0] = a.d[2]; r.d[1] = s.d[2]; r.d[2] = a.d[3]; r.d[3] = s.d[3];', f'    c->xmm[{d}] = r;', '}']


def _cvt2si(truncate):
    def h(self, ins):
        o = self.ops(ins)
        src = f'c->xmm[{_xmm(o[1].reg())}].f[0]' if o[1].is_reg() else f'rdf32({o[1].addr()})'
        return [o[0].write(f'(uint32_t)kt_cvt_f2i(c, {src}, {1 if truncate else 0})')]
    return h


def _cvtps2pi(truncate):
    def h(self, ins):
        o, d = _dst_mm(self, ins)
        src = _src128(o[1])
        return ['{', f'    KtXmm s = {src};',
                f'    c->mm[{d}].sd[0] = kt_cvt_f2i(c, s.f[0], {int(truncate)}); '
                f'c->mm[{d}].sd[1] = kt_cvt_f2i(c, s.f[1], {int(truncate)});', '}']
    return h


def i_cvtsi2ss(self, ins):
    o, d = _dst_xmm(self, ins)
    return [f'c->xmm[{d}].f[0] = (float)(int32_t){o[1].read(4)};']


def i_cvtpi2ps(self, ins):
    o, d = _dst_xmm(self, ins)
    return ['{', f'    KtMmx s = {_src64(o[1])};',
            f'    c->xmm[{d}].f[0] = (float)s.sd[0]; c->xmm[{d}].f[1] = (float)s.sd[1];', '}']


def _comiss(self, ins):
    o, d = _dst_xmm(self, ins)
    src = f'c->xmm[{_xmm(o[1].reg())}].f[0]' if o[1].is_reg() else f'rdf32({o[1].addr()})'
    return ['{', f'    uint32_t f = kt_fcomi_flags(c->xmm[{d}].f[0], {src});',
            "    cc_op = KT_CC(KT_CC_EFLAGS, 4); cc_res = f; cc_a = 0; cc_b = 0;", '}']


def i_ldmxcsr(self, ins):
    (o,) = self.ops(ins)
    return [f'c->mxcsr = rd32({o.addr()});']


def i_stmxcsr(self, ins):
    (o,) = self.ops(ins)
    return [f'wr32({o.addr()}, c->mxcsr);']


def install():
    handlers = {}
    for name in PACKED:
        handlers[name] = _packed(name)
    for name in LOGIC:
        handlers[name] = _logic(name)
    for name in SHIFTS:
        handlers[name] = _shift(name)
    for name, field, n, high in [('punpcklbw', 'b', 8, False), ('punpckhbw', 'b', 8, True),
                                 ('punpcklwd', 'w', 4, False), ('punpckhwd', 'w', 4, True),
                                 ('punpckldq', 'd', 2, False), ('punpckhdq', 'd', 2, True)]:
        handlers[name] = _unpack(field, n, high)
    for op in FLOAT:
        handlers[op + 'ps'] = _fop(op, True)
        handlers[op + 'ss'] = _fop(op, False)
    for op in UNARY:
        handlers[op + 'ps'] = _funop(op, True)
        handlers[op + 'ss'] = _funop(op, False)
    for name in FLOAT_LOGIC:
        handlers[name] = _flogic(name)
    for pred in CMP:
        handlers[f'cmp{pred}ps'] = _fcmp(pred, True)
        handlers[f'cmp{pred}ss'] = _fcmp(pred, False)
    handlers.update({
        'packssdw': i_packssdw, 'packsswb': i_packsswb, 'packuswb': i_packuswb, 'pmaddwd': i_pmaddwd,
        'pshufw': i_pshufw, 'movq': i_movq, 'movntq': i_movntq, 'movd': i_movd, 'emms': i_emms,
        'movaps': _movx(False), 'movups': _movx(False), 'movntps': _movx(False), 'movss': _movx(True),
        'movlps': i_movlps, 'movhps': i_movhps, 'movhlps': i_movhlps, 'movlhps': i_movlhps,
        'shufps': i_shufps, 'unpcklps': i_unpcklps, 'unpckhps': i_unpckhps,
        'cvttss2si': _cvt2si(True), 'cvtss2si': _cvt2si(False), 'cvtps2pi': _cvtps2pi(False),
        'cvttps2pi': _cvtps2pi(True), 'cvtsi2ss': i_cvtsi2ss, 'cvtpi2ps': i_cvtpi2ps,
        'comiss': _comiss, 'ucomiss': _comiss, 'ldmxcsr': i_ldmxcsr, 'stmxcsr': i_stmxcsr,
    })
    for name, h in handlers.items():
        setattr(FunctionLifter, 'i_' + name, h)


install()
