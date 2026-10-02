// Library-level HLE: every function XbSymbolDatabase found in a hardware
// library is redirected. Functions with a host implementation jump to it;
// "ignored" ones return 0; the rest stop the run and report who called them,
// which drives what to implement next.
#include "hle.h"

#include <intrin.h>

#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <string>

#include "host.h"
#include "manifest.h"

namespace {

std::vector<HleEntry> g_syms;  // sorted by va

// Function-local statics so registration from other translation units'
// static initializers works regardless of initialization order.
struct Impl {
    void* fn;
    HleInvokeFn invoke;
};
std::map<std::string, Impl>& Impls() {
    static std::map<std::string, Impl> m;
    return m;
}

// Title address -> how lifted code calls the replacement there.
struct Target {
    HleInvokeFn invoke;  // null: ignored (returns 0)
    HleConv conv;
    uint32_t stack_bytes;
    uint32_t index;
    bool unimplemented = false;  // mandatory library function without a host version
};
std::map<uint32_t, Target> g_targets;

HleConv ConvOf(const std::string& c) {
    return c == "fastcall" ? HleConv::Fastcall : c == "cdecl" ? HleConv::Cdecl : HleConv::Stdcall;
}
std::set<std::string>& Ignored() {
    static std::set<std::string> s;
    return s;
}

#if UINTPTR_MAX == 0xFFFFFFFFu
void WriteJump(uint32_t from, const void* to) {
    auto* p = reinterpret_cast<uint8_t*>(from);
    p[0] = 0xE9;
    uint32_t rel = uint32_t(reinterpret_cast<uintptr_t>(to)) - (from + 5);
    memcpy(p + 1, &rel, 4);
}

void EmitCall(uint8_t* at, const void* to) {
    at[0] = 0xE8;
    uint32_t rel = uint32_t(reinterpret_cast<uintptr_t>(to)) - uint32_t(reinterpret_cast<uintptr_t>(at + 5));
    memcpy(at + 1, &rel, 4);
}
#endif


}  // namespace

void HleRegister(const char* lib, const char* name, void* fn, HleInvokeFn invoke) {
    Impls()[std::string(lib) + "!" + name] = Impl{fn, invoke};
}
void HleRegisterIgnored(const char* lib, const char* name) { Ignored().insert(std::string(lib) + "!" + name); }

#if UINTPTR_MAX == 0xFFFFFFFFu
// Reached via `push index; call HleUnimplemented` from a per-symbol stub, so
// the stack holds [stub return][index][title return address].
extern "C" __declspec(noreturn) void __stdcall HleUnimplemented(uint32_t index) {
    uint32_t caller = static_cast<uint32_t*>(_AddressOfReturnAddress())[2];
    const HleEntry& e = g_syms[index];
    uint32_t base = 0;
    const char* from = HleSymbolAt(caller, &base);
    Fatal("HLE unimplemented: %s!%s (va %08x), called from %08x%s%s", e.lib.c_str(), e.name.c_str(), e.va, caller,
          from ? " in " : "", from ? from : "");
}
#endif


// Called (preserving registers via pushad) the first time an ignored function runs.
extern "C" void __stdcall HleIgnoredNotice(uint32_t index) {
    static std::mutex mu;
    static std::set<uint32_t> seen;
    std::lock_guard<std::mutex> lk(mu);
    if (seen.insert(index).second) Log("HLE ignored: %s!%s", g_syms[index].lib.c_str(), g_syms[index].name.c_str());
}

// Libraries that drive NV2A/APU hardware: every function must be replaced.
// Elsewhere (XAPI, XGRAPHICS...) only functions with a host implementation are.
bool MandatoryLibrary(const std::string& lib) { return lib == "D3D8" || lib == "D3D8LTCG" || lib == "DSOUND"; }

void HleInstall(const Manifest& m) {
    g_syms = m.hle;
    std::sort(g_syms.begin(), g_syms.end(), [](auto& a, auto& b) { return a.va < b.va; });
    size_t implemented = 0, ignored = 0;
    for (uint32_t i = 0; i < g_syms.size(); i++) {
        const HleEntry& e = g_syms[i];
        std::string key = e.lib + "!" + e.name;
        auto it = Impls().find(key);
        if (it != Impls().end()) {
#if UINTPTR_MAX == 0xFFFFFFFFu
            WriteJump(e.va, it->second.fn);
#endif
            g_targets[e.va] = Target{it->second.invoke, ConvOf(e.callconv), e.stack_bytes, i};
            implemented++;
            continue;
        }
        bool is_ignored = Ignored().count(key) > 0;
        if (!is_ignored && !MandatoryLibrary(e.lib)) continue;
        g_targets[e.va] = Target{nullptr, ConvOf(e.callconv), e.stack_bytes, i, !is_ignored};
#if UINTPTR_MAX != 0xFFFFFFFFu
        ignored += is_ignored;  // handled at call time (HleCallFromLifted)
#else
        uint8_t* stub = ExecAlloc(32);
        if (is_ignored) {
            // pushad; push i; call HleIgnoredNotice; popad; xor eax,eax; ret N
            uint8_t* c = stub;
            *c++ = 0x60;
            *c++ = 0x68; memcpy(c, &i, 4); c += 4;
            EmitCall(c, reinterpret_cast<void*>(&HleIgnoredNotice)); c += 5;
            *c++ = 0x61;
            *c++ = 0x31; *c++ = 0xC0;
            *c++ = 0xC2; uint16_t n = uint16_t(e.stack_bytes); memcpy(c, &n, 2);
            ignored++;
        } else {
            stub[0] = 0x68;  // push imm32
            memcpy(stub + 1, &i, 4);
            EmitCall(stub + 5, reinterpret_cast<void*>(&HleUnimplemented));
        }
        WriteJump(e.va, stub);
#endif
    }
    for (auto& [key, fn] : Impls()) {
        bool found = std::any_of(g_syms.begin(), g_syms.end(), [&](auto& e) { return e.lib + "!" + e.name == key; });
        if (!found) Log("HLE: %s is implemented but not present in this title", key.c_str());
    }
    Log("HLE: %zu symbols hooked, %zu implemented, %zu ignored", g_syms.size(), implemented, ignored);
}

// Nearest known symbol at or below va (only within the HLE libraries).
const char* HleSymbolAt(uint32_t va, uint32_t* base) {
    auto it = std::upper_bound(g_syms.begin(), g_syms.end(), va, [](uint32_t v, auto& e) { return v < e.va; });
    if (it == g_syms.begin()) return nullptr;
    --it;
    if (va - it->va > 0x2000) return nullptr;
    *base = it->va;
    return it->name.c_str();
}

bool HleReplaces(uint32_t va) { return g_targets.count(va) != 0; }

bool HleCallFromLifted(KtCpu* c, uint32_t va) {
    auto it = g_targets.find(va);
    if (it == g_targets.end()) return false;
    const Target& t = it->second;
    if (t.unimplemented)
        Fatal("HLE unimplemented: %s!%s (va %08x), called from %08x", g_syms[t.index].lib.c_str(),
              g_syms[t.index].name.c_str(), va, rd32(c->esp));
    if (t.invoke) {
        t.invoke(c, t.conv);
        return true;
    }
    HleIgnoredNotice(t.index);  // ignored: return 0, pop the arguments
    c->eax = 0;
    c->eip = rd32(c->esp);
    c->esp += 4 + (t.conv == HleConv::Cdecl ? 0 : t.stack_bytes);
    return true;
}

uint32_t HleGuestCallable(void* fn, HleInvokeFn invoke, HleConv conv) {
#if UINTPTR_MAX == 0xFFFFFFFFu
    (void)invoke, (void)conv;
    return uint32_t(reinterpret_cast<uintptr_t>(fn));
#else
    (void)fn;
    static std::mutex mu;
    std::lock_guard<std::mutex> lk(mu);
    uint32_t va = H2G(GuestAlloc(16, PAGE_READWRITE, 16));  // unique address; never executed
    g_targets[va] = Target{invoke, conv, 0, 0};
    return va;
#endif
}
