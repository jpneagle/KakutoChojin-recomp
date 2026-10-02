// Runtime for hosts without native title code (64-bit builds).
//
// The title runs only as lifted C (tools/lift, linked into this program).
// Guest memory is the 4 GB region at g_guest_base (guest_mem.cpp); each host
// thread that runs title code gets a KtCpu and a guest stack. Calls out of
// lifted code reach host implementations through HleCallFromLifted (HLE
// exports, kernel imports, vtables built with HleGuestCallable).
#if UINTPTR_MAX != 0xFFFFFFFFu

#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <thread>
#include <unordered_map>
#include <vector>

#include "hle.h"
#include "host.h"
#include "manifest.h"
#include "platform/launcher.h"

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#define KT_X86_HOST 1
#elif defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#include <x86intrin.h>
#define KT_X86_HOST 1
#endif

#if KT_WIN32_API
#include <shellapi.h>
#endif

namespace {

struct LiftEntry {
    uint32_t va;
    void (*fn)(KtCpu*);
    uint32_t flags;
    uint8_t* on;
};

std::unordered_map<uint32_t, const LiftEntry*> g_lifted;
thread_local KtCpu* t_cpu = nullptr;
constexpr size_t kGuestStack = 1 << 20;
constexpr uint32_t kReturnMarker = 0xFFFFFFF0;  // return address of host -> guest calls

void (*LiftedAt(uint32_t va))(KtCpu*) {
    auto it = g_lifted.find(va);
    return it != g_lifted.end() && it->second->fn && *it->second->on ? it->second->fn : nullptr;
}

[[noreturn]] void NoCode(KtCpu* c, uint32_t target, const char* how) {
    const char* name = KernelImportName(target);
    Fatal("lift: %s %08x%s%s with no lifted code or host implementation (esp=%08x, return %08x)", how, target,
          name ? " = kernel " : "", name ? name : "", c->esp, rd32(c->esp));
}

}  // namespace

extern "C" {

KtCpu* KtThreadCpu() {
    if (!t_cpu) {
        auto* c = new KtCpu{};
        c->fcw = 0x027F;
        c->mxcsr = 0x1F80;
        auto* stack = static_cast<uint8_t*>(GuestAlloc(kGuestStack));
        if (!stack) Fatal("lift: cannot allocate a guest stack");
        c->esp = H2G(stack) + uint32_t(kGuestStack) - 16;
        t_cpu = c;
    }
    return t_cpu;
}

// Lifted code calls a target that is not enabled lifted code: the return
// address has not been pushed yet (on x86 the native call pushes it).
void KtCallNative(KtCpu* c, uint32_t target) {
    c->esp -= 4;
    wr32(c->esp, kReturnMarker);
    if (HleCallFromLifted(c, target)) return;
    if (auto fn = LiftedAt(target)) {
        fn(c);
        return;
    }
    NoCode(c, target, "call to");
}

void KtCallIndirect(KtCpu* c, uint32_t target, uint32_t ret) {
    if (auto fn = LiftedAt(target)) {
        c->esp -= 4;
        wr32(c->esp, ret);
        fn(c);
        return;
    }
    c->esp -= 4;
    wr32(c->esp, ret);
    if (HleCallFromLifted(c, target)) return;
    NoCode(c, target, "indirect call to");
}

void KtJumpIndirect(KtCpu* c, uint32_t target) {
    if (auto fn = LiftedAt(target)) {
        fn(c);
        return;
    }
    if (HleCallFromLifted(c, target)) return;  // tail call: [esp] is the return address
    NoCode(c, target, "jump to");
}

void KtJumpNative(KtCpu* c, uint32_t target) { KtJumpIndirect(c, target); }

// fs: is the thread's Xbox KPCR (in guest memory); fs:[0] is its NtTib
// exception list, which lifted SEH code maintains.
uint32_t KtFsRead(uint32_t off, uint32_t size) {
    XThread* t = XThreadCurrent();
    if (!t) Fatal("lift: fs:[%x] read on a thread without a KPCR", off);
    uint32_t v = 0;
    memcpy(&v, t->kpcr + off, size);
    return v;
}

void KtFsWrite(uint32_t off, uint32_t size, uint32_t value) {
    XThread* t = XThreadCurrent();
    if (!t) Fatal("lift: fs:[%x] write on a thread without a KPCR", off);
    memcpy(t->kpcr + off, &value, size);
}

void KtTrap(KtCpu* c, uint32_t addr, const char* what) {
    Fatal("lift: trap at %08x (%s) esp=%08x eax=%08x", addr, what, c->esp, c->eax);
}

#ifdef KT_X86_HOST
uint64_t KtRdtsc() { return __rdtsc(); }

void KtCpuid(KtCpu* c) {
#ifdef _MSC_VER
    int r[4];
    __cpuidex(r, int(c->eax), int(c->ecx));
    c->eax = r[0], c->ebx = r[1], c->ecx = r[2], c->edx = r[3];
#else
    unsigned a, b, cc, d;
    __cpuid_count(c->eax, c->ecx, a, b, cc, d);
    c->eax = a, c->ebx = b, c->ecx = cc, c->edx = d;
#endif
}
#else
// Other hosts: a 733 MHz time stamp counter and the Xbox's Pentium III.
uint64_t KtRdtsc() {
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch());
    return uint64_t(ns.count()) * 733 / 1000;
}

void KtCpuid(KtCpu* c) {
    switch (c->eax) {
        case 0:  // max leaf 2, "GenuineIntel"
            c->eax = 2, c->ebx = 0x756E6547, c->edx = 0x49656E69, c->ecx = 0x6C65746E;
            break;
        case 1:  // family 6 model 8 (Coppermine): FPU TSC CX8 CMOV MMX FXSR SSE
            c->eax = 0x0000068A, c->ebx = 0, c->ecx = 0, c->edx = 0x0383F9FF;
            break;
        default:
            c->eax = c->ebx = c->ecx = c->edx = 0;
            break;
    }
}
#endif

// 80-bit extended <-> double without an x87.
double kt_f80_load(uint32_t addr) {
    uint64_t mant;
    uint16_t se;
    memcpy(&mant, KT_PTR(addr), 8);
    memcpy(&se, KT_PTR(addr + 8), 2);
    int exp = se & 0x7FFF;
    double sign = (se & 0x8000) ? -1.0 : 1.0;
    if (exp == 0 && mant == 0) return sign * 0.0;
    if (exp == 0x7FFF) return (mant << 1) ? NAN : sign * INFINITY;
    return sign * std::ldexp(double(mant), exp - 16383 - 63);
}

void kt_f80_store(uint32_t addr, double v) {
    uint64_t mant = 0;
    uint16_t se = std::signbit(v) ? 0x8000 : 0;
    if (std::isnan(v)) {
        se |= 0x7FFF, mant = 0xC000000000000000ull;
    } else if (std::isinf(v)) {
        se |= 0x7FFF, mant = 0x8000000000000000ull;
    } else if (v != 0) {
        int e;
        double m = std::frexp(std::fabs(v), &e);  // m in [0.5, 1)
        mant = uint64_t(std::ldexp(m, 64));
        se |= uint16_t(e - 1 + 16383);
    }
    memcpy(KT_PTR(addr), &mant, 8);
    memcpy(KT_PTR(addr + 8), &se, 2);
}

const LiftEntry* KtLiftTable(uint32_t* count);

}  // extern "C"

// Host -> title call on this thread's guest stack. The caller's lifted state
// (if this thread is inside title code) is preserved around the call.
uint32_t CallGuest(uint32_t fn, std::initializer_list<uint32_t> args) {
    KtCpu* c = KtThreadCpu();
    KtCpu saved = *c;
    uint32_t sp = c->esp & ~3u;
    for (auto it = args.end(); it != args.begin();) {
        --it;
        sp -= 4;
        wr32(sp, *it);
    }
    sp -= 4;
    wr32(sp, kReturnMarker);
    c->esp = sp;
    if (auto f = LiftedAt(fn)) {
        f(c);
    } else if (!HleCallFromLifted(c, fn)) {
        NoCode(c, fn, "host call to");
    }
    uint32_t eax = c->eax;
    double st0 = ST(0);
    uint32_t ftop = c->ftop, ftag = c->ftag;
    *c = saved;
    // A floating-point result stays on the x87 stack for the caller.
    if (ftop != saved.ftop) {
        kt_fpush(c, st0);
        (void)ftag;
    }
    return eax;
}

// ---- Startup -------------------------------------------------------------------------------

namespace {

constexpr uint32_t kXbeBase = 0x10000;
constexpr uint32_t kEntryKeys[2] = {0xA8FC57AB, 0x94859D4B};  // retail, debug
constexpr uint32_t kThunkKeys[2] = {0x5B6D40B6, 0xEFB1F152};

#pragma pack(push, 1)
struct XbeHeader {
    uint32_t magic;
    uint8_t signature[256];
    uint32_t base, headers_size, image_size, image_header_size, timestamp, cert_addr;
    uint32_t num_sections, section_headers_addr, init_flags, entry, tls_addr;
    uint32_t stack_size, heap_reserve, heap_commit, pe_base, pe_size, pe_checksum, pe_timestamp;
    uint32_t debug_path_addr, debug_file_addr, debug_ufile_addr, kernel_thunk;
};
struct XbeSection {
    uint32_t flags, va, vsize, raw, rsize, name_addr, ref_count;
    uint32_t head_ref_addr, tail_ref_addr;
    uint8_t digest[20];
};
#pragma pack(pop)

std::vector<uint8_t> ReadFileBytes(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) Fatal("cannot open %s", path.string().c_str());
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

// Copies the XBE into guest memory at its own addresses.
void LoadXbe(const std::vector<uint8_t>& xbe, uint32_t* entry, uint32_t* thunk) {
    auto* h = reinterpret_cast<const XbeHeader*>(xbe.data());
    if (h->magic != 'HEBX') Fatal("not an XBE (bad magic)");
    if (h->base != kXbeBase) Fatal("unsupported XBE base 0x%08x", h->base);
    if (!GuestCommit(kXbeBase, h->image_size, PAGE_READWRITE)) Fatal("cannot commit the XBE image");
    memcpy(G2H(kXbeBase), xbe.data(), h->headers_size);
    auto* secs = reinterpret_cast<const XbeSection*>(xbe.data() + h->section_headers_addr - kXbeBase);
    for (uint32_t i = 0; i < h->num_sections; i++) {
        const XbeSection& s = secs[i];
        if (s.raw + s.rsize > xbe.size()) Fatal("XBE section %u exceeds the file", i);
        GuestCommit(s.va, s.vsize > s.rsize ? s.vsize : s.rsize, PAGE_READWRITE);
        memcpy(G2H(s.va), xbe.data() + s.raw, s.rsize);
    }
    auto in_image = [&](uint32_t va) { return va >= kXbeBase && va < kXbeBase + h->image_size; };
    for (int k = 0; k < 2; k++) {
        *entry = h->entry ^ kEntryKeys[k];
        *thunk = h->kernel_thunk ^ kThunkKeys[k];
        if (in_image(*entry) && in_image(*thunk)) return;
    }
    Fatal("cannot decode entry point / kernel thunk");
}

void InstallLifted() {
    uint32_t count = 0;
    const LiftEntry* table = KtLiftTable(&count);
    uint32_t enabled = 0, replaced = 0;
    for (uint32_t i = 0; i < count; i++) {
        const LiftEntry& e = table[i];
        if (!e.fn) continue;
        g_lifted[e.va] = &e;
        if (HleReplaces(e.va)) {  // the host implementation takes the calls
            replaced++;
            continue;
        }
        *e.on = 1;
        enabled++;
    }
    Log("lift: %u functions, %u lifted functions enabled, %u replaced by the host", count, enabled, replaced);
}

// Title name from the XBE certificate (ASCII; other characters become '?').
std::string TitleName(const std::vector<uint8_t>& xbe) {
    auto* h = reinterpret_cast<const XbeHeader*>(xbe.data());
    if (xbe.size() < sizeof(XbeHeader) || h->cert_addr < h->base) return {};
    size_t cert = h->cert_addr - h->base;
    std::string name;
    for (size_t i = 0; i < 40 && cert + 12 + 2 * i + 1 < xbe.size(); i++) {
        uint16_t c = uint16_t(xbe[cert + 12 + 2 * i] | xbe[cert + 13 + 2 * i] << 8);
        if (!c) break;
        name += c < 0x80 ? char(c) : '?';
    }
    return name;
}

std::vector<std::filesystem::path> Args(int argc, char** argv) {
#if KT_WIN32_API
    (void)argc, (void)argv;  // wide arguments keep non-ASCII paths intact
    int n = 0;
    LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &n);
    std::vector<std::filesystem::path> out(w, w + n);
    LocalFree(w);
    return out;
#else
    return std::vector<std::filesystem::path>(argv, argv + argc);
#endif
}

}  // namespace

// usage: KakutoChojin <game dir | xbe file> <manifest.txt> [hdd dir]
int main(int argc, char** argv) {
    namespace fs = std::filesystem;
    auto args = Args(argc, argv);
    if (args.size() < 3) {
        fprintf(stderr, "usage: KakutoChojin <game dir containing default.xbe | xbe file> <manifest.txt> [hdd dir]\n");
        return 2;
    }
    fs::path game = args[1], manifest_path = args[2], xbe_path = game / "default.xbe";
    std::error_code ec;
    if (fs::is_regular_file(game, ec)) xbe_path = game, game = game.parent_path();
    fs::path hdd = args.size() > 3 ? args[3] : HostDir() / "hdd";
    LogInit(HostDir() / "KakutoChojin.log");
    Log("KakutoChojin-recomp (64-bit): game=%s manifest=%s hdd=%s", game.string().c_str(), manifest_path.string().c_str(),
        hdd.string().c_str());

    auto xbe = ReadFileBytes(xbe_path);
    if (!platform::RunLauncher(TitleName(xbe))) {
        Log("launcher: quit");
        return 0;
    }
    GuestMemoryInit();
    Manifest manifest = LoadManifest(manifest_path);
    uint32_t entry, thunk;
    LoadXbe(xbe, &entry, &thunk);
    Log("entry=%08x kernel_thunk=%08x", entry, thunk);

    IoSetGameRoot(game);
    IoSetHddRoot(hdd);
    CrashHandlerInstall();
    KpcrInit();
    KernelInstallThunks(thunk);
    HleInstall(manifest);
    InstallLifted();
    KernelStartTimers();

    auto* xh = reinterpret_cast<const XbeHeader*>(xbe.data());
    uint32_t tls_size = 4, tls_raw = 0, tls_start = 0;
    if (xh->tls_addr) {
        auto* tls_dir = G2H<const uint32_t>(xh->tls_addr);
        tls_start = tls_dir[0];
        tls_raw = tls_dir[1] - tls_dir[0];
        tls_size = ((tls_raw + tls_dir[4] + 15) & ~15u) + 4;
    }
    XThreadSetTitleTlsSize(tls_size);
    XThread* t = XThreadCreate(tls_size);
    if (tls_raw) memcpy(G2H<uint8_t>(t->tls_data) + 4, G2H(tls_start), tls_raw);
    XThreadBindCurrent(t);

    Log("calling entry point %08x", entry);
    CallGuest(entry, {});
    Log("entry point returned; waiting for title threads");
    for (;;) std::this_thread::sleep_for(std::chrono::hours(1));
}

#endif
