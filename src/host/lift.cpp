#include <cstdint>
#if UINTPTR_MAX == 0xFFFFFFFFu  // 32-bit x86 only
// Mixed-mode execution of lifted code (tools/lift): C functions generated
// from the title's x86 run alongside the original machine code.
//
// kt_lifted.dll (built from the lifter output) exports KtLiftTable. For each
// enabled function the original entry is patched with a jump to a stub that
// enters the C version; lifted code calls original code through
// KtCallNative. Both sides share the guest stack (the thread's real stack),
// so arguments and return addresses are exactly where either side expects.
// Lifted C code itself runs on a separate per-thread host stack.
//
// KT_LIFT selects functions: "all", or comma-separated hex addresses/ranges
// ("12340", "10000-20000"); a leading '-' excludes ("all,-15000-16000").
//
// KT_VERIFY (same syntax) instead checks lifted leaf functions against the
// original code: on every call the lifted version runs first with its memory
// writes logged, the writes are undone, the original code runs from the same
// state, and registers, x87 state and written memory are compared. The
// original results are kept, so a broken lifted function cannot derail the
// run. Mismatches are logged ("verify:").
//
// KT_LIFT_STRICT=1 (with KT_LIFT=all) proves that no title machine code runs:
// the XBE's executable sections become non-executable. Calls to HLE-patched
// entries go straight to the host implementation, host callbacks into title
// functions fault and are redirected to the lifted versions, and any other
// native execution is logged ("strict:") as a gap.
#include <intrin.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "host.h"
#include "manifest.h"
#define KT_HOST_BUILD
#define KT_VERIFY
#include "../lift/kt_lift.h"

static_assert(offsetof(KtCpu, eax) == 0x00 && offsetof(KtCpu, ecx) == 0x04 && offsetof(KtCpu, edx) == 0x08 &&
                  offsetof(KtCpu, ebx) == 0x0C && offsetof(KtCpu, esp) == 0x10 && offsetof(KtCpu, ebp) == 0x14 &&
                  offsetof(KtCpu, esi) == 0x18 && offsetof(KtCpu, edi) == 0x1C && offsetof(KtCpu, eip) == 0x20 &&
                  offsetof(KtCpu, host_sp) == 0x24,
              "KtCpu offsets are used by the thunks below");

namespace {

struct LiftEntry {
    uint32_t va;
    void (*fn)(KtCpu*);
    uint32_t flags;  // bit 0: entry patchable, bit 1: leaf, bits 8-15: copyable prologue length
    uint8_t* on;     // enabled: lifted callers call the C version
};

const LiftEntry* g_table = nullptr;
uint32_t g_count = 0;
std::unordered_map<uint32_t, uint32_t> g_index;  // va -> table index
std::unordered_map<uint32_t, uint8_t*> g_stubs;  // strict mode: lifted entry -> enter stub
std::unordered_map<uint32_t, uint32_t> g_hle_targets;
std::mutex g_hle_mu;
uint32_t g_text_lo = 0xFFFFFFFF, g_text_hi = 0;  // executable XBE sections
std::unordered_map<uint32_t, bool> g_fs_sites;     // fs: patch sites (not HLE)

bool InText(uint32_t a) { return a >= g_text_lo && a < g_text_hi; }

// Calls `f(va, size)` for each executable XBE section (headers: count
// @0x11C, table @0x120 of the image at 0x10000; see xbelib.py).
template <typename F>
void ForEachTextSection(F f) {
    const uint8_t* base = reinterpret_cast<const uint8_t*>(uintptr_t(0x10000));
    uint32_t count, table;
    memcpy(&count, base + 0x11C, 4);
    memcpy(&table, base + 0x120, 4);
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t* h = reinterpret_cast<const uint8_t*>(uintptr_t(table + i * 0x38));
        uint32_t flags, va, size;
        memcpy(&flags, h, 4), memcpy(&va, h + 4, 4), memcpy(&size, h + 8, 4);
        if (flags & 4) f(va, size);
    }
}


constexpr size_t kHostStack = 1 << 20;
__declspec(thread) KtCpu* t_cpu = nullptr;

}  // namespace

extern "C" {

// The calling thread's lifted CPU state (created on first use).
KtCpu* __cdecl KtThreadCpu() {
    if (!t_cpu) {
        auto* c = static_cast<KtCpu*>(VirtualAlloc(nullptr, sizeof(KtCpu) + kHostStack, MEM_COMMIT | MEM_RESERVE,
                                                   PAGE_READWRITE));
        if (!c) Fatal("lift: cannot allocate CPU state");
        c->fcw = 0x027F;
        c->mxcsr = 0x1F80;
        c->host_sp = uint32_t(reinterpret_cast<uintptr_t>(c) + sizeof(KtCpu) + kHostStack - 16);
        t_cpu = c;
    }
    return t_cpu;
}

void __cdecl KtNativeReturn();

// The x87 stack lives in the hardware registers while original code runs
// and in KtCpu.st while lifted code runs; it moves at every transition.
// FNSAVE layout: cw @0, sw @4, tag word @8, st(0)..st(7) as 80-bit @28.
KtCpu* __cdecl KtFpuFromHw(KtCpu* c) {
    uint8_t env[28];
    __asm {
        lea eax, env
        fnstenv [eax]
    }
    uint16_t cw, sw, tw;
    memcpy(&cw, env, 2), memcpy(&sw, env + 4, 2), memcpy(&tw, env + 8, 2);
    c->fcw = cw;
    c->fsw = sw & ~0x3800u;
    c->ftop = (sw >> 11) & 7;
    c->ftag = 0;
    if (tw == 0xFFFF) return c;  // empty: nothing to move
    uint8_t buf[108];
    __asm {
        lea eax, buf
        fnsave [eax]
    }
    for (uint32_t i = 0; i < 8; i++) {
        uint32_t p = (c->ftop + i) & 7;
        if (((tw >> (2 * p)) & 3) == 3) continue;
        const uint8_t* src = buf + 28 + 10 * i;
        double d;
        __asm {
            mov eax, src
            fld tbyte ptr [eax]
            fstp d
        }
        c->st[p] = d;
        c->ftag |= 1u << p;
    }
    return c;
}

KtCpu* __cdecl KtFpuToHw(KtCpu* c) {
    uint16_t cw = uint16_t(c->fcw);
    if (!c->ftag) {
        __asm {
            fninit
            fldcw cw
        }
        return c;
    }
    uint8_t buf[108] = {};
    __asm { fninit }
    uint16_t tw = 0;
    for (uint32_t i = 0; i < 8; i++) {
        uint32_t p = (c->ftop + i) & 7;
        if (!(c->ftag & (1u << p))) {
            tw |= 3u << (2 * p);
            continue;
        }
        uint8_t* dst = buf + 28 + 10 * i;
        double d = c->st[p];
        __asm {
            mov eax, dst
            fld d
            fstp tbyte ptr [eax]
        }
    }
    uint16_t sw = uint16_t((c->fsw & ~0x3800u) | ((c->ftop & 7) << 11));
    memcpy(buf, &cw, 2), memcpy(buf + 4, &sw, 2), memcpy(buf + 8, &tw, 2);
    __asm {
        lea eax, buf
        frstor [eax]
    }
    return c;
}

// Native -> lifted. Entered from a per-function stub `push fn; jmp
// KtEnterLifted` placed at the original entry: [esp] = fn, [esp+4] = the
// guest return address.
__declspec(naked) void KtEnterLifted() {
    __asm {
        push eax
        push ecx
        push edx
        call KtThreadCpu
        mov [eax + 0x0C], ebx
        mov [eax + 0x14], ebp
        mov [eax + 0x18], esi
        mov [eax + 0x1C], edi
        pop edx
        mov [eax + 0x08], edx
        pop ecx
        mov [eax + 0x04], ecx
        pop edx
        mov [eax + 0x00], edx
        pop edx                 // lifted function
        mov [eax + 0x10], esp   // guest esp -> return address
        mov esp, [eax + 0x24]   // host stack
        push edx
        push eax
        push eax
        call KtFpuFromHw
        add esp, 4
        mov eax, [esp]
        mov edx, [esp + 4]
        push eax
        call edx
        add esp, 4
        mov eax, [esp]
        push eax
        call KtFpuToHw
        add esp, 4
        pop eax
        add esp, 4
        mov esp, [eax + 0x10]   // guest esp after the lifted `ret`
        mov ebx, [eax + 0x0C]
        mov ebp, [eax + 0x14]
        mov esi, [eax + 0x18]
        mov edi, [eax + 0x1C]
        mov ecx, [eax + 0x04]
        push dword ptr [eax + 0x20]  // continue at the popped return address
        mov edx, [eax + 0x08]
        mov eax, [eax + 0x00]
        ret
    }
}

void __cdecl KtCallNativeRaw(KtCpu* c, uint32_t target);
static uint32_t ResolveHlePatch(uint32_t target);
static bool g_strict = false;

void KtCallNative(KtCpu* c, uint32_t target) {
    if (g_strict) target = ResolveHlePatch(target);
    KtCallNativeRaw(c, target);
}

// Lifted -> native: runs guest code at `target` with the CPU state in `c`;
// it returns into KtNativeReturn, which switches back to the host stack.
__declspec(naked) void __cdecl KtCallNativeRaw(KtCpu* c, uint32_t target) {
    __asm {
        push dword ptr [esp + 4]
        call KtFpuToHw
        add esp, 4
        mov eax, [esp + 4]
        mov edx, [esp + 8]
        push ebx
        push esi
        push edi
        push ebp
        push dword ptr [eax + 0x24]  // outer host_sp, restored on return
        mov [eax + 0x24], esp
        mov esp, [eax + 0x10]        // guest stack
        mov ecx, offset KtNativeReturn
        push ecx                     // return address for the callee
        push edx                     // target
        mov ebx, [eax + 0x0C]
        mov ebp, [eax + 0x14]
        mov esi, [eax + 0x18]
        mov edi, [eax + 0x1C]
        mov ecx, [eax + 0x04]
        mov edx, [eax + 0x08]
        mov eax, [eax + 0x00]
        ret
    }
}

__declspec(naked) void __cdecl KtNativeReturn() {
    __asm {
        push eax
        push ecx
        push edx
        call KtThreadCpu
        mov [eax + 0x0C], ebx
        mov [eax + 0x14], ebp
        mov [eax + 0x18], esi
        mov [eax + 0x1C], edi
        pop edx
        mov [eax + 0x08], edx
        pop ecx
        mov [eax + 0x04], ecx
        pop edx
        mov [eax + 0x00], edx
        mov [eax + 0x10], esp
        mov esp, [eax + 0x24]
        push eax
        call KtFpuFromHw
        add esp, 4
        pop dword ptr [eax + 0x24]
        pop ebp
        pop edi
        pop esi
        pop ebx
        ret
    }
}

// Abandons the current lifted call chain on the host stack: guest code at
// `target` continues with the guest stack as it is and eventually returns
// to whatever original return address lies on it.
__declspec(naked) void __cdecl KtJumpNative(KtCpu* c, uint32_t target) {
    __asm {
        push dword ptr [esp + 4]
        call KtFpuToHw
        add esp, 4
        mov eax, [esp + 4]
        mov edx, [esp + 8]
        mov esp, [eax + 0x10]
        push edx
        mov ebx, [eax + 0x0C]
        mov ebp, [eax + 0x14]
        mov esi, [eax + 0x18]
        mov edi, [eax + 0x1C]
        mov ecx, [eax + 0x04]
        mov edx, [eax + 0x08]
        mov eax, [eax + 0x00]
        ret
    }
}

static void RecordGap(uint32_t ip);

void KtJumpIndirect(KtCpu* c, uint32_t target) {
    auto it = g_index.find(target);
    if (it != g_index.end() && g_table[it->second].fn && *g_table[it->second].on) {
        g_table[it->second].fn(c);  // jmp: no return address pushed
        return;
    }
    if (!InText(target)) {  // kernel / HLE code in the host: a tail call
        uint32_t r = rd32(c->esp);
        c->esp += 4;
        KtCallNative(c, target);
        c->eip = r;
        return;
    }
    if (g_strict) RecordGap(target);
    KtJumpNative(c, target);
}

void KtCallIndirect(KtCpu* c, uint32_t target, uint32_t ret) {
    auto it = g_index.find(target);
    if (it != g_index.end() && g_table[it->second].fn && *g_table[it->second].on) {
        c->esp -= 4;
        wr32(c->esp, ret);
        g_table[it->second].fn(c);
    } else {
        KtCallNative(c, target);
    }
}

// fs:[0] is the Win32 exception list (title SEH runs on Win32 SEH); other
// offsets are the thread's fake Xbox KPCR (see kpcr.cpp).
uint32_t KtFsRead(uint32_t off, uint32_t size) {
    const uint8_t* p;
    if (off < 4) {
        static thread_local uint32_t list;
        list = __readfsdword(0);
        p = reinterpret_cast<const uint8_t*>(&list) + off;
    } else {
        XThread* t = XThreadCurrent();
        if (!t) Fatal("lift: fs:[%x] read on a thread without a KPCR", off);
        p = t->kpcr + off;
    }
    uint32_t v = 0;
    memcpy(&v, p, size);
    return v;
}

#pragma warning(suppress : 4733)  // fs:[0] is the Win32 SEH list on purpose
void KtFsWrite(uint32_t off, uint32_t size, uint32_t value) {
    if (off == 0 && size == 4) {
        __writefsdword(0, value);
        return;
    }
    XThread* t = XThreadCurrent();
    if (!t) Fatal("lift: fs:[%x] write on a thread without a KPCR", off);
    memcpy(t->kpcr + off, &value, size);
}

void KtTrap(KtCpu* c, uint32_t addr, const char* what) {
    Log("lift: trap at %08x (%s) esp=%08x eax=%08x ecx=%08x edx=%08x", addr, what, c->esp, c->eax, c->ecx, c->edx);
    LogTitleStack(reinterpret_cast<const void*>(uintptr_t(c->esp)));
    Fatal("lift: trap at %08x (%s)", addr, what);
}

uint64_t KtRdtsc() { return __rdtsc(); }

void KtCpuid(KtCpu* c) {
    int r[4];
    __cpuidex(r, int(c->eax), int(c->ecx));
    c->eax = r[0], c->ebx = r[1], c->ecx = r[2], c->edx = r[3];
}

// 80-bit extended values (fld/fstp tbyte) via the host x87.
// ---- Verification write log ------------------------------------------------

volatile long kt_wlog_any = 0;

}  // extern "C"

namespace {

struct WriteRecord {
    uint32_t addr, size;
    uint8_t old[16];
};
thread_local std::vector<WriteRecord> t_wlog;
thread_local bool t_logging = false;

}  // namespace

extern "C" {

void kt_wlog(uint32_t addr, uint32_t size) {
    if (!t_logging) return;
    WriteRecord r{addr, size > 16 ? 16 : size, {}};
    memcpy(r.old, KT_PTR(addr), r.size);
    t_wlog.push_back(r);
}

double kt_f80_load(uint32_t addr) {
    double d;
    const void* p = KT_PTR(addr);
    __asm {
        mov eax, p
        fld tbyte ptr [eax]
        fstp d
    }
    return d;
}

void kt_f80_store(uint32_t addr, double v) {
    if (kt_wlog_any) kt_wlog(addr, 10);
    void* p = KT_PTR(addr);
    __asm {
        mov eax, p
        fld v
        fstp tbyte ptr [eax]
    }
}

}  // extern "C"

namespace {

struct VerifyRec {
    void (*fn)(KtCpu*);
    uint32_t va;
    uint32_t tramp;  // copy of the original first instructions + jmp back
    std::atomic<uint32_t> calls{0}, mismatches{0}, float_diffs{0};
};

std::mutex g_verify_mu;
std::vector<VerifyRec*> g_verify;
std::atomic<uint64_t> g_verify_calls{0};

// Lifted x87 code computes in double rather than 80-bit extended, and SSE
// rcp/rsqrt are exact rather than the hardware's 12-bit approximations; such
// differences are reported separately from real mismatches.
bool NearlyEqual(double a, double b, double tolerance = 1e-4) {
    if (a == b || (std::isnan(a) && std::isnan(b))) return true;
    double m = std::max(std::fabs(a), std::fabs(b));
    return std::fabs(a - b) <= m * tolerance || std::fabs(a - b) < 1e-30;
}

void VerifySummary() {
    uint32_t exercised = 0, bad = 0, fdiff = 0;
    for (VerifyRec* r : g_verify) {
        if (r->calls) exercised++;
        if (r->mismatches) bad++;
        if (r->float_diffs) fdiff++;
    }
    Log("verify: %llu calls, %u functions exercised, %u with mismatches, %u with floating-point precision differences only",
        (unsigned long long)g_verify_calls.load(), exercised, bad, fdiff);
}

}  // namespace

extern "C" void __cdecl KtVerifyRun(KtCpu* c, VerifyRec* r) {
    KtCpu s0 = *c;

    // Lifted run with writes logged.
    t_wlog.clear();
    t_logging = true;
    _InterlockedIncrement(&kt_wlog_any);
    r->fn(c);
    _InterlockedDecrement(&kt_wlog_any);
    t_logging = false;
    KtCpu s1 = *c;
    std::vector<WriteRecord> writes = std::move(t_wlog);
    std::vector<std::vector<uint8_t>> finals;
    finals.reserve(writes.size());
    for (const WriteRecord& w : writes) {
        const uint8_t* p = static_cast<const uint8_t*>(KT_PTR(w.addr));
        finals.emplace_back(p, p + w.size);
    }
    for (size_t i = writes.size(); i-- > 0;) memcpy(KT_PTR(writes[i].addr), writes[i].old, writes[i].size);

    // Original run from the same state: the copied prologue jumps back into
    // the original body (the entry itself is patched).
    *c = s0;
    uint32_t ret = rd32(c->esp);
    c->esp += 4;
    KtCallNativeRaw(c, r->tramp);
    c->eip = ret;

    // Compare.
    char diff[512];
    int n = 0;
    diff[0] = 0;
    auto add = [&](const char* fmt, auto... args) {
        if (n < int(sizeof diff) - 64) n += snprintf(diff + n, sizeof diff - n, fmt, args...);
    };
    static const char* kRegs[8] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"};
    const uint32_t* a = &s1.eax;
    const uint32_t* b = &c->eax;
    bool fdiff = false;
    for (int i = 0; i < 8; i++) {
        if (a[i] == b[i]) continue;
        // x87 exception flags (fnstsw) are not emulated.
        if (i == 0 && ((a[i] ^ b[i]) & ~0x3Fu) == 0) {
            fdiff = true;
            continue;
        }
        add(" %s=%08x/%08x", kRegs[i], a[i], b[i]);
    }
    if (s1.ftop != c->ftop || s1.ftag != c->ftag) {
        add(" x87 top/tag %u,%02x/%u,%02x", s1.ftop, s1.ftag, c->ftop, c->ftag);
    }
    for (int i = 0; i < 8; i++) {
        if (!(c->ftag & (1u << i))) continue;
        if (s1.st[i] != c->st[i]) {
            if (NearlyEqual(s1.st[i], c->st[i])) {
                fdiff = true;
            } else {
                add(" st%d=%g/%g", (i - int(c->ftop)) & 7, s1.st[i], c->st[i]);
                    }
        }
    }
    for (size_t i = 0; i < writes.size(); i++) {
        const WriteRecord& w = writes[i];
        // Below the final esp is dead stack (the return thunk also uses it).
        if (w.addr + w.size <= c->esp && w.addr + 0x10000 > c->esp) continue;
        const uint8_t* now = static_cast<const uint8_t*>(KT_PTR(w.addr));
        if (memcmp(now, finals[i].data(), w.size) == 0) continue;
        // Stores of x87 results may round differently (double vs extended).
        if (w.size == 4) {
            float x, y;
            memcpy(&x, finals[i].data(), 4), memcpy(&y, now, 4);
            if (NearlyEqual(x, y)) {
                fdiff = true;
                continue;
            }
        } else if (w.size == 8) {
            double x, y;
            memcpy(&x, finals[i].data(), 8), memcpy(&y, now, 8);
            if (NearlyEqual(x, y)) {
                fdiff = true;
                continue;
            }
        } else if (w.size == 16) {
            float x[4], y[4];
            memcpy(x, finals[i].data(), 16), memcpy(y, now, 16);
            bool close = true;
            for (int k = 0; k < 4; k++) close = close && NearlyEqual(x[k], y[k], 1e-3);
            if (close) {
                fdiff = true;
                continue;
            }
        }
        uint32_t lv = 0, nv = 0;
        memcpy(&lv, finals[i].data(), std::min<uint32_t>(w.size, 4));
        memcpy(&nv, now, std::min<uint32_t>(w.size, 4));
        add(" [%08x]/%u=%08x/%08x", w.addr, w.size, lv, nv);
    }
    r->calls++;
    uint64_t total = ++g_verify_calls;
    if (n) {
        if (r->mismatches++ < 3) Log("verify: %08x mismatch (lifted/original):%s", r->va, diff);
    } else if (fdiff) {
        if (r->float_diffs++ < 1) Log("verify: %08x floating-point precision differences only", r->va);
    }
    if (total % 200000 == 0) VerifySummary();
}

extern "C" __declspec(naked) void KtEnterVerify() {
    // Like KtEnterLifted, with [esp] = VerifyRec* and KtVerifyRun(cpu, rec)
    // in place of the lifted function.
    __asm {
        push eax
        push ecx
        push edx
        call KtThreadCpu
        mov [eax + 0x0C], ebx
        mov [eax + 0x14], ebp
        mov [eax + 0x18], esi
        mov [eax + 0x1C], edi
        pop edx
        mov [eax + 0x08], edx
        pop ecx
        mov [eax + 0x04], ecx
        pop edx
        mov [eax + 0x00], edx
        pop edx                 // VerifyRec*
        mov [eax + 0x10], esp
        mov esp, [eax + 0x24]
        push edx
        push eax
        push eax
        call KtFpuFromHw
        add esp, 4
        mov eax, [esp]
        mov edx, [esp + 4]
        push edx
        push eax
        call KtVerifyRun
        add esp, 8
        mov eax, [esp]
        push eax
        call KtFpuToHw
        add esp, 4
        pop eax
        add esp, 4
        mov esp, [eax + 0x10]
        mov ebx, [eax + 0x0C]
        mov ebp, [eax + 0x14]
        mov esi, [eax + 0x18]
        mov edi, [eax + 0x1C]
        mov ecx, [eax + 0x04]
        push dword ptr [eax + 0x20]
        mov edx, [eax + 0x08]
        mov eax, [eax + 0x00]
        ret
    }
}

namespace {

struct Range {
    uint32_t lo, hi;
    bool on;
};

std::vector<Range> ParseSelection(const char* s, bool* all) {
    std::vector<Range> out;
    *all = false;
    std::string spec(s);
    size_t pos = 0;
    while (pos <= spec.size()) {
        size_t end = spec.find(',', pos);
        std::string tok = spec.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        pos = end == std::string::npos ? spec.size() + 1 : end + 1;
        if (tok.empty()) continue;
        bool on = tok[0] != '-';
        if (!on) tok.erase(0, 1);
        if (tok == "all") {
            if (on) *all = true;
            continue;
        }
        unsigned lo = 0, hi = 0;
        if (sscanf(tok.c_str(), "%x-%x", &lo, &hi) == 2)
            out.push_back({lo, hi, on});
        else if (sscanf(tok.c_str(), "%x", &lo) == 1)
            out.push_back({lo, lo + 1, on});
    }
    return out;
}

}  // namespace

// Loads kt_lifted.dll and switches the selected functions to their lifted
// versions. Call after the HLE and fs patches are in place.
static void VerifyInstall(const char* spec);
static const LiftEntry* LoadTable();
static void StrictInstall();

void LiftInstall(const Manifest& m) {
    for (const FsPatch& p : m.fs) g_fs_sites[p.va] = true;
    char spec[4096];
    if (GetEnvironmentVariableA("KT_VERIFY", spec, sizeof spec)) {
        VerifyInstall(spec);
        return;
    }
    if (!GetEnvironmentVariableA("KT_LIFT", spec, sizeof spec)) return;
    if (!LoadTable()) return;
    ForEachTextSection([](uint32_t va, uint32_t size) {
        g_text_lo = std::min(g_text_lo, va);
        g_text_hi = std::max(g_text_hi, va + size);
    });
    bool all;
    std::vector<Range> sel = ParseSelection(spec, &all);
    uint32_t enabled = 0, patched = 0, available = 0, hle = 0;
    for (uint32_t i = 0; i < g_count; i++) {
        const LiftEntry& e = g_table[i];
        if (!e.fn) continue;
        available++;
        bool on = all;
        for (const Range& r : sel)
            if (e.va >= r.lo && e.va < r.hi) on = r.on;
        if (!on) continue;
        if (ResolveHlePatch(e.va) != e.va) {  // replaced by the host: keep that
            hle++;
            continue;
        }
        *e.on = 1;
        enabled++;
        if (!(e.flags & 1)) continue;
        // push fn; jmp KtEnterLifted
        uint8_t* stub = ExecAlloc(10);
        uint32_t fn = uint32_t(reinterpret_cast<uintptr_t>(e.fn));
        stub[0] = 0x68;
        memcpy(stub + 1, &fn, 4);
        stub[5] = 0xE9;
        uint32_t rel = uint32_t(reinterpret_cast<uintptr_t>(&KtEnterLifted)) - uint32_t(uintptr_t(stub + 10));
        memcpy(stub + 6, &rel, 4);
        auto* site = reinterpret_cast<uint8_t*>(uintptr_t(e.va));
        site[0] = 0xE9;
        rel = uint32_t(reinterpret_cast<uintptr_t>(stub)) - (e.va + 5);
        memcpy(site + 1, &rel, 4);
        patched++;
    }
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    Log("lift: %u functions in table, %u lifted, %u enabled, %u entries patched, %u left to HLE (KT_LIFT=%s)",
        g_count, available, enabled, patched, hle, spec);
    if (GetEnvironmentVariableA("KT_LIFT_STRICT", nullptr, 0)) StrictInstall();
}

static const LiftEntry* LoadTable() {
    HMODULE dll = LoadLibraryW((HostDir() / "kt_lifted.dll").c_str());
    if (!dll) {
        Log("lift: kt_lifted.dll not found (%lu)", GetLastError());
        return nullptr;
    }
    using TableFn = const LiftEntry* (*)(uint32_t*);
    auto table = reinterpret_cast<TableFn>(GetProcAddress(dll, "KtLiftTable"));
    if (!table) Fatal("lift: kt_lifted.dll has no KtLiftTable");
    g_table = table(&g_count);
    for (uint32_t i = 0; i < g_count; i++) g_index[g_table[i].va] = i;
    return g_table;
}

// KT_VERIFY: patch the selected verifiable leaf functions with stubs that
// run both versions (see KtVerifyRun). No function is enabled for lifted
// calls, so every call to them goes through the patched entry.
static void VerifyInstall(const char* spec) {
    if (!g_table && !LoadTable()) return;
    bool all;
    std::vector<Range> sel = ParseSelection(spec, &all);
    for (uint32_t i = 0; i < g_count; i++) {
        const LiftEntry& e = g_table[i];
        uint32_t copy = (e.flags >> 8) & 0xFF;
        if (!e.fn || !(e.flags & 1) || !(e.flags & 2) || !copy) continue;
        bool on = all;
        for (const Range& r : sel)
            if (e.va >= r.lo && e.va < r.hi) on = r.on;
        if (!on) continue;
        auto* site = reinterpret_cast<uint8_t*>(uintptr_t(e.va));
        // Trampoline: the original first instructions, then jmp to the rest.
        uint8_t* tramp = ExecAlloc(copy + 5);
        memcpy(tramp, site, copy);
        tramp[copy] = 0xE9;
        uint32_t rel = (e.va + copy) - uint32_t(uintptr_t(tramp + copy + 5));
        memcpy(tramp + copy + 1, &rel, 4);
        auto* rec = new VerifyRec;
        rec->fn = e.fn, rec->va = e.va, rec->tramp = uint32_t(uintptr_t(tramp));
        g_verify.push_back(rec);
        // push rec; jmp KtEnterVerify
        uint8_t* stub = ExecAlloc(10);
        uint32_t rp = uint32_t(uintptr_t(rec));
        stub[0] = 0x68;
        memcpy(stub + 1, &rp, 4);
        stub[5] = 0xE9;
        rel = uint32_t(reinterpret_cast<uintptr_t>(&KtEnterVerify)) - uint32_t(uintptr_t(stub + 10));
        memcpy(stub + 6, &rel, 4);
        site[0] = 0xE9;
        rel = uint32_t(uintptr_t(stub)) - (e.va + 5);
        memcpy(site + 1, &rel, 4);
    }
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    atexit(VerifySummary);
    Log("verify: %zu leaf functions instrumented (KT_VERIFY=%s)", g_verify.size(), spec);
}

// ---- Strict mode -------------------------------------------------------------

namespace {


uint8_t* MakeEnterStub(void (*fn)(KtCpu*)) {
    // push fn; jmp KtEnterLifted
    uint8_t* stub = ExecAlloc(10);
    uint32_t f = uint32_t(reinterpret_cast<uintptr_t>(fn));
    stub[0] = 0x68;
    memcpy(stub + 1, &f, 4);
    stub[5] = 0xE9;
    uint32_t rel = uint32_t(reinterpret_cast<uintptr_t>(&KtEnterLifted)) - uint32_t(uintptr_t(stub + 10));
    memcpy(stub + 6, &rel, 4);
    return stub;
}

}  // namespace

// HLE replaces title functions by patching `jmp host_fn` over their entry;
// in strict mode that jump is read instead of executed.
static uint32_t ResolveHlePatch(uint32_t target) {
    if (!InText(target) || g_fs_sites.count(target)) return target;
    std::lock_guard<std::mutex> lk(g_hle_mu);
    auto it = g_hle_targets.find(target);
    if (it != g_hle_targets.end()) return it->second;
    uint32_t resolved = target;
    const uint8_t* p = static_cast<const uint8_t*>(KT_PTR(target));
    if (p[0] == 0xE9) {
        uint32_t rel;
        memcpy(&rel, p + 1, 4);
        uint32_t dest = target + 5 + rel;
        if (!InText(dest)) resolved = dest;
    }
    g_hle_targets[target] = resolved;
    return resolved;
}

// Vectored handler hook: an execute fault in the (non-executable) title
// text is either a host callback into a lifted function or a gap.
std::atomic<uint32_t> g_callbacks{0};

bool LiftHandleExecFault(EXCEPTION_POINTERS* ep) {
    if (!g_strict || ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return false;
    if (ep->ExceptionRecord->ExceptionInformation[0] != 8) return false;  // not an execute fault
    uint32_t ip = ep->ContextRecord->Eip;
    if (!InText(ip)) return false;
    auto it = g_stubs.find(ip);
    if (it != g_stubs.end()) {
        if (g_callbacks++ < 5) Log("strict: host call into title function %08x redirected", ip);
        ep->ContextRecord->Eip = uint32_t(reinterpret_cast<uintptr_t>(it->second));
        return true;
    }
    uint32_t hle = ResolveHlePatch(ip);
    if (hle != ip) {
        ep->ContextRecord->Eip = hle;
        return true;
    }
    // A gap: log it, let that page execute natively from now on and carry
    // on, so one run lists every gap.
    uint32_t ret = 0;
    memcpy(&ret, reinterpret_cast<const void*>(uintptr_t(ep->ContextRecord->Esp)), 4);
    static std::atomic<uint32_t> gaps{0};
    Log("strict: gap %u: native title code executed at %08x (stack top %08x)", ++gaps, ip, ret);
    // Record calls (not cascades after one) for the lifter.
    if (ret == uint32_t(reinterpret_cast<uintptr_t>(&KtNativeReturn))) RecordGap(ip);
    DWORD old;
    // Two pages: the instruction may cross into the next one.
    VirtualProtect(reinterpret_cast<void*>(uintptr_t(ip & ~0xFFFu)), 0x2000, PAGE_EXECUTE_READWRITE, &old);
    return true;
}

static void StrictInstall() {
    for (uint32_t i = 0; i < g_count; i++)
        if (g_table[i].fn && *g_table[i].on) g_stubs[g_table[i].va] = MakeEnterStub(g_table[i].fn);
    uint32_t protected_bytes = 0;
    ForEachTextSection([&](uint32_t va, uint32_t size) {
        DWORD old;
        if (VirtualProtect(reinterpret_cast<void*>(uintptr_t(va)), size, PAGE_READWRITE, &old)) protected_bytes += size;
    });
    // The loader is built without NX compatibility; turn DEP on so that
    // non-executable really means it.
    BOOL dep = SetProcessDEPPolicy(PROCESS_DEP_ENABLE);
    if (!dep) Log("strict: SetProcessDEPPolicy failed (%lu); native execution cannot be detected", GetLastError());
    g_strict = true;
    Log("strict: title code %08x-%08x made non-executable (%u bytes), %zu entry stubs", g_text_lo, g_text_hi,
        protected_bytes, g_stubs.size());
}

// Code reached natively that the lifter should know about; fed back with
// tools/lift/lift.py --extra lift_gaps.txt.
static void RecordGap(uint32_t ip) {
    static std::mutex mu;
    std::lock_guard<std::mutex> lk(mu);
    if (FILE* f = os::OpenFile(HostDir() / "lift_gaps.txt", "a")) {
        fprintf(f, "%08x\n", ip);
        fclose(f);
    }
}

#endif  // 32-bit x86
