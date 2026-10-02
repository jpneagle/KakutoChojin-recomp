// Per-thread fake KPCR/KTHREAD and the fs: access redirection.
//
// Windows owns fs: (it points at the TEB), so title instructions such as
// `mov eax, fs:[0x28]` are rewritten to jump to a trampoline that loads the
// thread's XThread from a Win32 TLS slot (read straight from the TEB) and
// fetches the field from the fake KPCR instead.
#include <cstddef>

#include "host.h"
#include "manifest.h"
#include "ob.h"
#include "os.h"

namespace {

#if UINTPTR_MAX == 0xFFFFFFFFu
// Patched fs: accesses read the XThread's KPCR from this Win32 TLS slot.
DWORD g_slot = TLS_OUT_OF_INDEXES;
constexpr unsigned kTebTlsSlots = 0xE10;  // TEB.TlsSlots[64] on x86
#else
thread_local XThread* t_current = nullptr;
#endif

}  // namespace

void KpcrInit() {
#if UINTPTR_MAX == 0xFFFFFFFFu
    g_slot = TlsAlloc();
    if (g_slot >= 64) Fatal("TLS slot %lu is outside TEB.TlsSlots", g_slot);
#endif
}

// Xbox keeps TLS at the top of the thread's stack: NtTib.StackBase (fs:[4])
// points just past the block and compiled TLS accesses use a negative index
// (-tls_size/4) from it. KTHREAD.TlsData points at the block's start, whose
// first dword is the "TLS array" slot pointing at the data that follows.
// The structures live in guest memory: title code follows the pointers in them.
XThread* XThreadCreate(size_t tls_size) {
    auto* t = static_cast<XThread*>(GuestAlloc(sizeof(XThread)));
    auto* tls = static_cast<uint8_t*>(GuestAlloc(tls_size + 16));
    if (!t || !tls) Fatal("XThreadCreate: out of memory");
    auto put = [](uint8_t* base, unsigned off, const void* v) {
        uint32_t g = H2G(v);
        memcpy(base + off, &g, 4);
    };
    put(tls, 0, tls + 4);
    t->tls_data = H2G(tls);
    put(t->kpcr, kpcr_off::kTlsPointer, tls + tls_size);
    put(t->kpcr, kpcr_off::kSelfPcr, t->kpcr);
    put(t->kpcr, kpcr_off::kPrcb, t->kpcr + kpcr_off::kCurrentThread);
    t->kpcr[kpcr_off::kIrql] = 0;  // PASSIVE_LEVEL
    put(t->kpcr, kpcr_off::kCurrentThread, t->kthread);
    put(t->kthread, kKthreadTlsData, tls);
    return t;
}

void XThreadBindCurrent(XThread* t) {
#if UINTPTR_MAX == 0xFFFFFFFFu
    TlsSetValue(g_slot, t->kpcr);
#else
    t_current = t;
#endif
    std::shared_ptr<ob::Thread> self = ob::CurrentThread();
    self->x = t;
    t->thread = self.get();
    if (!t->tid) t->tid = os::CurrentThreadId();
}

namespace {
size_t g_title_tls_size = 4;
}

void XThreadSetTitleTlsSize(size_t size) { g_title_tls_size = size; }

XThread* XThreadAdoptCurrent() {
    XThread* t = XThreadCreate(g_title_tls_size);
    XThreadBindCurrent(t);
    return t;
}

XThread* XThreadCurrent() {
#if UINTPTR_MAX == 0xFFFFFFFFu
    auto* kpcr = static_cast<uint8_t*>(TlsGetValue(g_slot));
    return kpcr ? reinterpret_cast<XThread*>(kpcr - offsetof(XThread, kpcr)) : nullptr;
#else
    return t_current;
#endif
}

XThread* XThreadFromKthread(void* kthread) {
    return reinterpret_cast<XThread*>(static_cast<uint8_t*>(kthread) - offsetof(XThread, kthread));
}

#if UINTPTR_MAX == 0xFFFFFFFFu
void ApplyFsPatches(const Manifest& m) {
    const uint32_t slot_disp = kTebTlsSlots + 4 * g_slot;
    for (const FsPatch& p : m.fs) {
        if (p.size < 5) Fatal("fs patch at %08x too short", p.va);
        if (p.reg == 4) Fatal("fs patch at %08x targets esp", p.va);
        uint8_t* site = reinterpret_cast<uint8_t*>(p.va);
        // Sanity check: the site must still be the fs: instruction we expect.
        if (site[0] != 0x64) Fatal("fs patch at %08x: unexpected bytes", p.va);

        uint8_t* tr = ExecAlloc(32);
        uint8_t* c = tr;
        auto emit32 = [&](uint32_t v) { memcpy(c, &v, 4); c += 4; };
        // mov reg, fs:[TlsSlots + 4*slot]   -> fake KPCR base
        *c++ = 0x64; *c++ = 0x8B; *c++ = uint8_t(0x05 | (p.reg << 3)); emit32(slot_disp);
        if (p.movzx_byte) {  // movzx reg, byte [reg + off]
            *c++ = 0x0F; *c++ = 0xB6;
        } else {             // mov reg, [reg + off]
            *c++ = 0x8B;
        }
        *c++ = uint8_t(0x80 | (p.reg << 3) | p.reg); emit32(p.offset);
        *c++ = 0xE9; emit32(uint32_t(p.va + p.size) - uint32_t(reinterpret_cast<uintptr_t>(c) + 4));

        site[0] = 0xE9;
        uint32_t rel = uint32_t(reinterpret_cast<uintptr_t>(tr)) - (p.va + 5);
        memcpy(site + 1, &rel, 4);
        for (unsigned i = 5; i < p.size; i++) site[i] = 0x90;
    }
    Log("applied %zu fs patches (TLS slot %lu)", m.fs.size(), g_slot);
}

#endif
