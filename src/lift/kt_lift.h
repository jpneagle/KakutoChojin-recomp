/* Runtime interface for code lifted from Xbox x86 by tools/lift/lift.py.
 *
 * Every Xbox function becomes `void f_XXXXXXXX(KtCpu* c)`. The guest stack,
 * calling conventions and memory layout are kept exactly: arguments and
 * return addresses live on the guest stack in guest memory, so lifted and
 * original functions can call each other freely.
 *
 * Guest memory is accessed at KT_MEMBASE + address. In mixed mode (32-bit
 * Windows, kt_host) the XBE is mapped at its real addresses and KT_MEMBASE
 * is 0; a 64-bit or non-x86 host reserves 4 GB and points KT_MEMBASE at it.
 *
 * Plain C99 so the generated code builds anywhere. */
#ifndef KT_LIFT_H
#define KT_LIFT_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _MSC_VER
#define KT_INLINE static __forceinline
#else
#define KT_INLINE static inline __attribute__((always_inline))
#endif

typedef union KtMmx {
    uint64_t q;
    uint32_t d[2];
    int32_t sd[2];
    uint16_t w[4];
    int16_t sw[4];
    uint8_t b[8];
    int8_t sb[8];
} KtMmx;

typedef union KtXmm {
    float f[4];
    uint32_t d[4];
    int32_t sd[4];
    uint64_t q[2];
} KtXmm;

/* The first fields are read by the mixed-mode thunks (kt_host lift.cpp);
 * keep their offsets. */
typedef struct KtCpu {
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi; /* 0x00 */
    uint32_t eip;     /* 0x20: return target of the last lifted `ret` */
    uint32_t host_sp; /* 0x24: mixed mode: top of the free host stack */
    uint32_t df;      /* direction flag */
    /* Lazy flags: the last flag-setting operation (see KT_CC_*). */
    uint32_t cc_op, cc_res, cc_a, cc_b, cc_cf;
    /* x87: st(i) = st[(ftop + i) & 7]. */
    double st[8];
    uint32_t ftop;
    uint32_t fsw, fcw;
    uint32_t ftag; /* bit i: physical register i holds a value */
    KtMmx mm[8];
    KtXmm xmm[8];
    uint32_t mxcsr;
} KtCpu;

/* Registers and lazy flags live in locals inside a lifted function. */
#define KT_LOCALS uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi, cc_op, cc_res, cc_a, cc_b, cc_cf
#define KT_LOAD                                                                                         (eax = c->eax, ecx = c->ecx, edx = c->edx, ebx = c->ebx, esp = c->esp, ebp = c->ebp, esi = c->esi,      edi = c->edi, cc_op = c->cc_op, cc_res = c->cc_res, cc_a = c->cc_a, cc_b = c->cc_b, cc_cf = c->cc_cf)
#define KT_STORE                                                                                        (c->eax = eax, c->ecx = ecx, c->edx = edx, c->ebx = ebx, c->esp = esp, c->ebp = ebp, c->esi = esi,      c->edi = edi, c->cc_op = cc_op, c->cc_res = cc_res, c->cc_a = cc_a, c->cc_b = cc_b, c->cc_cf = cc_cf)

/* ---- Guest memory ------------------------------------------------------- */

#if defined(_WIN32) && UINTPTR_MAX == 0xFFFFFFFFu && defined(KT_HOST_BUILD)
#define KT_API __declspec(dllexport)
#elif defined(_WIN32) && UINTPTR_MAX == 0xFFFFFFFFu
#define KT_API __declspec(dllimport)
#else
#define KT_API /* 64-bit: lifted code is linked into the runtime */
#endif

/* The lift table (KtLiftTable) is exported from kt_lifted.dll in mixed mode. */
#if defined(_WIN32) && UINTPTR_MAX == 0xFFFFFFFFu
#define KT_LIFT_EXPORT __declspec(dllexport)
#else
#define KT_LIFT_EXPORT
#endif
/* Tables generated before KT_LIFT_EXPORT spelled out __declspec(dllexport). */
#if !defined(_MSC_VER) && !defined(__declspec)
#define __declspec(x)
#endif

/* Verification builds (KT_VERIFY) report every guest memory write so the
 * host can undo a lifted run and compare it with the original code. */
#ifdef KT_VERIFY
KT_API extern volatile long kt_wlog_any;
KT_API void kt_wlog(uint32_t addr, uint32_t size);
#define KT_WLOG(a, n) \
    do { \
        if (kt_wlog_any) kt_wlog((a), (n)); \
    } while (0)
#else
#define KT_WLOG(a, n) ((void)0)
#endif

#ifndef KT_MEMBASE
#if UINTPTR_MAX == 0xFFFFFFFFu
#define KT_MEMBASE ((uint8_t*)0)
#else
extern uint8_t* g_guest_base; /* guest.h */
#define KT_MEMBASE g_guest_base
#endif
#endif
#define KT_PTR(a) ((void*)(KT_MEMBASE + (uint32_t)(a)))

KT_INLINE uint8_t rd8(uint32_t a) { return *(volatile uint8_t*)KT_PTR(a); }
KT_INLINE uint16_t rd16(uint32_t a) { uint16_t v; memcpy(&v, KT_PTR(a), 2); return v; }
KT_INLINE uint32_t rd32(uint32_t a) { uint32_t v; memcpy(&v, KT_PTR(a), 4); return v; }
KT_INLINE uint64_t rd64(uint32_t a) { uint64_t v; memcpy(&v, KT_PTR(a), 8); return v; }
KT_INLINE float rdf32(uint32_t a) { float v; memcpy(&v, KT_PTR(a), 4); return v; }
KT_INLINE double rdf64(uint32_t a) { double v; memcpy(&v, KT_PTR(a), 8); return v; }
KT_INLINE void wr8(uint32_t a, uint8_t v) { KT_WLOG(a, 1); *(volatile uint8_t*)KT_PTR(a) = v; }
KT_INLINE void wr16(uint32_t a, uint16_t v) { KT_WLOG(a, 2); memcpy(KT_PTR(a), &v, 2); }
KT_INLINE void wr32(uint32_t a, uint32_t v) { KT_WLOG(a, 4); memcpy(KT_PTR(a), &v, 4); }
KT_INLINE void wr64(uint32_t a, uint64_t v) { KT_WLOG(a, 8); memcpy(KT_PTR(a), &v, 8); }
KT_INLINE void wrf32(uint32_t a, float v) { KT_WLOG(a, 4); memcpy(KT_PTR(a), &v, 4); }
KT_INLINE void wrf64(uint32_t a, double v) { KT_WLOG(a, 8); memcpy(KT_PTR(a), &v, 8); }

/* ---- Lazy flags --------------------------------------------------------- */

enum {
    KT_CC_ADD = 1, KT_CC_ADC, KT_CC_SUB, KT_CC_SBB, KT_CC_LOGIC, KT_CC_INC, KT_CC_DEC, KT_CC_NEG,
    KT_CC_SHL, KT_CC_SHR, KT_CC_SAR, KT_CC_MUL, /* CF = OF = cc_cf */
    KT_CC_EFLAGS /* cc_res holds materialised EFLAGS bits */
};
/* cc_op = operation | (operand bytes << 8). */
#define KT_CC(op, size) ((op) | ((size) << 8))

#define KT_F_CF 0x001
#define KT_F_PF 0x004
#define KT_F_AF 0x010
#define KT_F_ZF 0x040
#define KT_F_SF 0x080
#define KT_F_DF 0x400
#define KT_F_OF 0x800

KT_INLINE uint32_t kt_szmask(uint32_t sz) { return sz == 1 ? 0xFFu : sz == 2 ? 0xFFFFu : 0xFFFFFFFFu; }
KT_INLINE uint32_t kt_msb(uint32_t v, uint32_t sz) { return (v >> (sz * 8 - 1)) & 1; }

KT_INLINE uint32_t kt_cf(uint32_t op, uint32_t res, uint32_t a, uint32_t b, uint32_t cf) {
    uint32_t sz = op >> 8, m = kt_szmask(sz);
    res &= m, a &= m, b &= m;
    switch (op & 0xFF) {
        case KT_CC_ADD: return res < a;
        case KT_CC_ADC: return cf ? res <= a : res < a;
        case KT_CC_SUB: return a < b;
        case KT_CC_SBB: return cf ? a <= b : a < b;
        case KT_CC_NEG: return a != 0;
        case KT_CC_LOGIC: return 0;
        case KT_CC_EFLAGS: return res & 1;
        default: return cf; /* INC/DEC (preserved), shifts, MUL */
    }
}
KT_INLINE uint32_t kt_zf(uint32_t op, uint32_t res) {
    if ((op & 0xFF) == KT_CC_EFLAGS) return (res >> 6) & 1;
    return (res & kt_szmask(op >> 8)) == 0;
}
KT_INLINE uint32_t kt_sf(uint32_t op, uint32_t res) {
    if ((op & 0xFF) == KT_CC_EFLAGS) return (res >> 7) & 1;
    return kt_msb(res, op >> 8);
}
KT_INLINE uint32_t kt_of(uint32_t op, uint32_t res, uint32_t a, uint32_t b, uint32_t cf) {
    uint32_t sz = op >> 8, sign = 1u << (sz * 8 - 1), m = kt_szmask(sz);
    switch (op & 0xFF) {
        case KT_CC_ADD:
        case KT_CC_ADC: return kt_msb((a ^ res) & (b ^ res), sz);
        case KT_CC_SUB:
        case KT_CC_SBB: return kt_msb((a ^ b) & (a ^ res), sz);
        case KT_CC_INC: return (res & m) == sign;
        case KT_CC_DEC: return (res & m) == sign - 1;
        case KT_CC_NEG: return (res & m) == sign;
        case KT_CC_SHL: return kt_msb(res, sz) ^ cf; /* exact for 1-bit shifts */
        case KT_CC_SHR: return kt_msb(a, sz);
        case KT_CC_MUL: return cf;
        case KT_CC_EFLAGS: return (res >> 11) & 1;
        default: return 0; /* LOGIC, SAR */
    }
}
KT_INLINE uint32_t kt_pf(uint32_t op, uint32_t res) {
    if ((op & 0xFF) == KT_CC_EFLAGS) return (res >> 2) & 1;
    uint32_t v = res & 0xFF;
    v ^= v >> 4, v ^= v >> 2, v ^= v >> 1;
    return (~v) & 1;
}
KT_INLINE uint32_t kt_af(uint32_t op, uint32_t res, uint32_t a, uint32_t b) {
    if ((op & 0xFF) == KT_CC_EFLAGS) return (res >> 4) & 1;
    return ((a ^ b ^ res) >> 4) & 1;
}
/* Condition codes in x86 encoding order (jo .. jg). */
KT_INLINE int kt_cond(uint32_t cc, uint32_t op, uint32_t res, uint32_t a, uint32_t b, uint32_t cf) {
    switch (cc) {
        case 0x0: return kt_of(op, res, a, b, cf);
        case 0x1: return !kt_of(op, res, a, b, cf);
        case 0x2: return kt_cf(op, res, a, b, cf);
        case 0x3: return !kt_cf(op, res, a, b, cf);
        case 0x4: return kt_zf(op, res);
        case 0x5: return !kt_zf(op, res);
        case 0x6: return kt_cf(op, res, a, b, cf) | kt_zf(op, res);
        case 0x7: return !(kt_cf(op, res, a, b, cf) | kt_zf(op, res));
        case 0x8: return kt_sf(op, res);
        case 0x9: return !kt_sf(op, res);
        case 0xA: return kt_pf(op, res);
        case 0xB: return !kt_pf(op, res);
        case 0xC: return kt_sf(op, res) != kt_of(op, res, a, b, cf);
        case 0xD: return kt_sf(op, res) == kt_of(op, res, a, b, cf);
        case 0xE: return kt_zf(op, res) || kt_sf(op, res) != kt_of(op, res, a, b, cf);
        default: return !kt_zf(op, res) && kt_sf(op, res) == kt_of(op, res, a, b, cf);
    }
}
KT_INLINE uint32_t kt_eflags(uint32_t op, uint32_t res, uint32_t a, uint32_t b, uint32_t cf, uint32_t df) {
    if ((op & 0xFF) == KT_CC_EFLAGS) return (res & ~KT_F_DF) | (df ? KT_F_DF : 0) | 2;
    return kt_cf(op, res, a, b, cf) | kt_pf(op, res) << 2 | kt_af(op, res, a, b) << 4 | kt_zf(op, res) << 6 |
           kt_sf(op, res) << 7 | (df ? KT_F_DF : 0) | kt_of(op, res, a, b, cf) << 11 | 2;
}

/* ---- Runtime services (provided by the host) ------------------------------ */


/* Calls guest code at `target` that is not lifted (or not enabled): the
 * guest return address pushed for it is a host trampoline. */
KT_API void KtCallNative(KtCpu* c, uint32_t target);
/* Calls whatever is at `target` (lifted if enabled, else native); `ret` is
 * the guest return address pushed for a lifted callee. */
KT_API void KtCallIndirect(KtCpu* c, uint32_t target, uint32_t ret);
/* Continues at `target` as original code and never returns: the rest of
 * the current function runs natively (an unresolved indirect jump). */
KT_API void KtJumpNative(KtCpu* c, uint32_t target);
/* `jmp target` out of the current function: a lifted function runs in
 * place (its ret consumes the current stack top), host code is a tail call,
 * anything else continues as original code (KtJumpNative). */
KT_API void KtJumpIndirect(KtCpu* c, uint32_t target);
/* fs: accesses (Xbox KPCR / Win32 TEB exception list). */
KT_API uint32_t KtFsRead(uint32_t offset, uint32_t size);
KT_API void KtFsWrite(uint32_t offset, uint32_t size, uint32_t value);
/* Unreachable or unsupported code was executed. */
KT_API void KtTrap(KtCpu* c, uint32_t addr, const char* what);
/* Privileged / system instructions handled by the host. */
KT_API uint64_t KtRdtsc(void);
KT_API void KtCpuid(KtCpu* c);

/* ---- x87 helpers ---------------------------------------------------------- */

#define ST(i) c->st[(c->ftop + (i)) & 7]
KT_INLINE void kt_fpush(KtCpu* c, double v) {
    c->ftop = (c->ftop - 1) & 7;
    c->st[c->ftop] = v;
    c->ftag |= 1u << c->ftop;
}
KT_INLINE double kt_fpop(KtCpu* c) {
    double v = c->st[c->ftop];
    c->ftag &= ~(1u << c->ftop);
    c->ftop = (c->ftop + 1) & 7;
    return v;
}
/* fxam: C3 C2 C0 class, C1 sign. */
KT_INLINE void kt_fxam(KtCpu* c) {
    double v = ST(0);
    uint32_t f;
    if (!(c->ftag & (1u << c->ftop))) f = 0x4100;          /* empty */
    else if (isnan(v)) f = 0x0100;                         /* NaN */
    else if (isinf(v)) f = 0x0500;                         /* infinity */
    else if (v == 0) f = 0x4000;                           /* zero */
    else if (fabs(v) < 2.2250738585072014e-308) f = 0x4400; /* denormal */
    else f = 0x0400;                                       /* normal */
    if (signbit(v)) f |= 0x0200;
    c->fsw = (c->fsw & ~0x4700u) | f;
}
/* fcom: C3 C2 C0 = 000 (>), 001 (<), 100 (=), 111 (unordered). */
KT_INLINE void kt_fcom(KtCpu* c, double a, double b) {
    uint32_t f = (a > b) ? 0 : (a < b) ? 0x0100 : (a == b) ? 0x4000 : 0x4500;
    c->fsw = (c->fsw & ~0x4700u) | f;
}
KT_INLINE uint32_t kt_fstsw(KtCpu* c) { return (c->fsw & ~0x3800u) | ((c->ftop & 7) << 11); }
/* fcomi-style EFLAGS from a comparison (ZF PF CF). */
KT_INLINE uint32_t kt_fcomi_flags(double a, double b) {
    return (a > b) ? 0 : (a < b) ? KT_F_CF : (a == b) ? KT_F_ZF : (KT_F_ZF | KT_F_PF | KT_F_CF);
}
/* Rounding per the control word (RC bits 10-11). */
KT_INLINE double kt_fround(KtCpu* c, double v) {
    switch ((c->fcw >> 10) & 3) {
        case 0: return nearbyint(v);
        case 1: return floor(v);
        case 2: return ceil(v);
        default: return trunc(v);
    }
}
KT_INLINE int32_t kt_fist32(KtCpu* c, double v) {
    double r = kt_fround(c, v);
    return (r >= -2147483648.0 && r < 2147483648.0) ? (int32_t)r : (int32_t)0x80000000;
}
KT_INLINE int16_t kt_fist16(KtCpu* c, double v) {
    double r = kt_fround(c, v);
    return (r >= -32768.0 && r < 32768.0) ? (int16_t)r : (int16_t)0x8000;
}
KT_INLINE int64_t kt_fist64(KtCpu* c, double v) {
    double r = kt_fround(c, v);
    return (r >= -9223372036854775808.0 && r < 9223372036854775808.0) ? (int64_t)r : (int64_t)0x8000000000000000ull;
}
/* 80-bit extended <-> double (fld/fstp tbyte). */
KT_API double kt_f80_load(uint32_t addr);
KT_API void kt_f80_store(uint32_t addr, double v);

/* fprem (truncated quotient) / fprem1 (round-to-nearest quotient). */
KT_INLINE void kt_fprem(KtCpu* c, int ieee) {
    double a = ST(0), b = ST(1);
    double q = ieee ? nearbyint(a / b) : trunc(a / b);
    ST(0) = ieee ? remainder(a, b) : fmod(a, b);
    uint64_t qi = (uint64_t)fabs(q);
    uint32_t f = ((qi >> 2) & 1) << 8 | ((qi >> 1) & 1) << 14 | (qi & 1) << 9; /* C0, C3, C1 */
    c->fsw = (c->fsw & ~0x4700u) | f;                                          /* C2 = 0: complete */
}

/* fnsave / frstor (108-byte protected-mode image); fnsave reinitialises. */
KT_INLINE void kt_fnsave(KtCpu* c, uint32_t a) {
    uint32_t tw = 0;
    for (uint32_t p = 0; p < 8; p++)
        if (!(c->ftag & (1u << p))) tw |= 3u << (2 * p);
    wr32(a, c->fcw | 0xFFFF0000u);
    wr32(a + 4, ((c->fsw & ~0x3800u) | ((c->ftop & 7) << 11)) | 0xFFFF0000u);
    wr32(a + 8, tw | 0xFFFF0000u);
    for (uint32_t i = 12; i < 28; i += 4) wr32(a + i, 0);
    for (uint32_t i = 0; i < 8; i++) kt_f80_store(a + 28 + 10 * i, c->st[(c->ftop + i) & 7]);
    c->fcw = 0x037F, c->fsw = 0, c->ftop = 0, c->ftag = 0;
}
KT_INLINE void kt_frstor(KtCpu* c, uint32_t a) {
    uint32_t sw = rd16(a + 4), tw = rd16(a + 8);
    c->fcw = rd16(a);
    c->fsw = sw & ~0x3800u;
    c->ftop = (sw >> 11) & 7;
    c->ftag = 0;
    for (uint32_t i = 0; i < 8; i++) {
        uint32_t p = (c->ftop + i) & 7;
        c->st[p] = kt_f80_load(a + 28 + 10 * i);
        if (((tw >> (2 * p)) & 3) != 3) c->ftag |= 1u << p;
    }
}

/* ---- Integer helpers ------------------------------------------------------ */

KT_INLINE uint32_t kt_rol32(uint32_t v, uint32_t n) { n &= 31; return n ? (v << n) | (v >> (32 - n)) : v; }
KT_INLINE uint32_t kt_ror32(uint32_t v, uint32_t n) { n &= 31; return n ? (v >> n) | (v << (32 - n)) : v; }
KT_INLINE uint32_t kt_bsf(uint32_t v) { uint32_t i = 0; while (!(v & 1)) v >>= 1, i++; return i; }
KT_INLINE uint32_t kt_bsr(uint32_t v) { uint32_t i = 31; while (!(v & 0x80000000u)) v <<= 1, i--; return i; }
KT_INLINE uint32_t kt_bswap(uint32_t v) {
    return (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24);
}

/* ---- MMX / SSE helpers ---------------------------------------------------- */

KT_INLINE KtMmx kt_mmx_ld(uint32_t a) { KtMmx v; memcpy(&v, KT_PTR(a), 8); return v; }
KT_INLINE KtXmm kt_xmm_ld(uint32_t a) { KtXmm v; memcpy(&v, KT_PTR(a), 16); return v; }
KT_INLINE KtXmm kt_xmm_ld64(uint32_t a) { KtXmm v; memset(&v, 0, 16); memcpy(&v, KT_PTR(a), 8); return v; }
KT_INLINE KtXmm kt_xmm_ld32(uint32_t a) { KtXmm v; memset(&v, 0, 16); memcpy(&v, KT_PTR(a), 4); return v; }
KT_INLINE void kt_xmm_st(uint32_t a, KtXmm v) { KT_WLOG(a, 16); memcpy(KT_PTR(a), &v, 16); }

KT_INLINE int16_t kt_sat16(int32_t v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
KT_INLINE uint16_t kt_satu16(int32_t v) { return (uint16_t)(v > 65535 ? 65535 : v < 0 ? 0 : v); }
KT_INLINE int8_t kt_sat8(int32_t v) { return (int8_t)(v > 127 ? 127 : v < -128 ? -128 : v); }
KT_INLINE uint8_t kt_satu8(int32_t v) { return (uint8_t)(v > 255 ? 255 : v < 0 ? 0 : v); }
KT_INLINE int32_t kt_cvt_f2i(KtCpu* c, float v, int truncate) {
    double r = truncate ? trunc((double)v) : nearbyint((double)v);
    (void)c;
    return (r >= -2147483648.0 && r < 2147483648.0) ? (int32_t)r : (int32_t)0x80000000;
}

#ifdef __cplusplus
}
#endif
#endif /* KT_LIFT_H */
