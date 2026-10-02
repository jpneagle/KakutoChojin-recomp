// Guest address space management (see guest.h).
#include "guest.h"

#include <map>
#include <mutex>
#include <vector>

#include "host.h"

#if UINTPTR_MAX == 0xFFFFFFFFu
// 32-bit: guest addresses are host addresses.
extern "C" uint8_t* g_guest_base = nullptr;

void GuestMemoryInit() {}

void* GuestCommit(uint32_t addr, size_t size, uint32_t protect) {
    return VirtualAlloc(reinterpret_cast<void*>(uintptr_t(addr)), size, MEM_COMMIT | MEM_RESERVE, protect);
}

// Small blocks come from the process heap (VirtualAlloc reserves 64 KB per
// call, which would exhaust the 32-bit address space).
namespace {
std::mutex g_small_mu;
std::map<const void*, size_t> g_small;
constexpr size_t kSmall = 0x8000;
}  // namespace

void* GuestAlloc(size_t size, uint32_t protect, size_t align) {
    if (size < kSmall && align <= 16 && protect == PAGE_READWRITE) {
        void* p = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size ? size : 1);
        std::lock_guard<std::mutex> lk(g_small_mu);
        if (p) g_small[p] = size;
        return p;
    }
    return VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, protect);
}

void* GuestReserve(size_t size, size_t) { return VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_READWRITE); }

void GuestDecommit(uint32_t addr, size_t size) {
    VirtualFree(reinterpret_cast<void*>(uintptr_t(addr)), size, MEM_DECOMMIT);
}

void GuestFree(void* p) {
    {
        std::lock_guard<std::mutex> lk(g_small_mu);
        if (g_small.erase(p)) {
            HeapFree(GetProcessHeap(), 0, p);
            return;
        }
    }
    VirtualFree(p, 0, MEM_RELEASE);
}

size_t GuestAllocationSize(const void* p) {
    {
        std::lock_guard<std::mutex> lk(g_small_mu);
        auto it = g_small.find(p);
        if (it != g_small.end()) return it->second;
    }
    MEMORY_BASIC_INFORMATION mbi;
    size_t total = 0;
    for (auto* q = static_cast<const uint8_t*>(p);
         VirtualQuery(q, &mbi, sizeof mbi) && mbi.AllocationBase == p && mbi.State == MEM_COMMIT; q += mbi.RegionSize)
        total += mbi.RegionSize;
    return total;
}

bool GuestQuery(uint32_t addr, GuestRegion* r) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(reinterpret_cast<void*>(uintptr_t(addr)), &mbi, sizeof mbi)) return false;
    *r = {H2G(mbi.BaseAddress), H2G(mbi.AllocationBase), uint32_t(mbi.RegionSize), mbi.State, mbi.Protect,
          mbi.AllocationProtect};
    return true;
}

bool GuestProtect(uint32_t addr, size_t size, uint32_t protect) {
    DWORD old;
    return VirtualProtect(reinterpret_cast<void*>(uintptr_t(addr)), size, protect, &old) != 0;
}

bool GuestValid(uint32_t addr, size_t size) {
    MEMORY_BASIC_INFORMATION mbi;
    return VirtualQuery(reinterpret_cast<void*>(uintptr_t(addr)), &mbi, sizeof mbi) && mbi.State == MEM_COMMIT &&
           uintptr_t(addr) + size <= uintptr_t(mbi.BaseAddress) + mbi.RegionSize;
}

#else
// 64-bit: one reserved 4 GB region, aligned so that the low 32 bits of a
// host pointer into it are the guest address. Allocations are tracked as
// blocks (for placement and sizes) and pages (commit state and protection).
extern "C" uint8_t* g_guest_base = nullptr;

namespace {

constexpr uint64_t kSpace = 1ull << 32;
constexpr uint32_t kPage = 0x1000;
// Dynamic allocations come from here (the XBE image sits below).
constexpr uint32_t kHeapLo = 0x01000000, kHeapHi = 0xF0000000;

std::mutex g_mu;
std::map<uint32_t, uint32_t> g_used;   // block start -> size (page multiples), includes fixed commits
std::vector<uint16_t> g_page_protect;  // per page: PAGE_* protection, 0 = not committed

uint32_t RoundUp(size_t v, uint32_t a) { return uint32_t((v + a - 1) & ~size_t(a - 1)); }

void SetPages(uint32_t lo, uint32_t size, uint16_t protect) {
    for (uint32_t pg = lo / kPage, n = size / kPage; n; pg++, n--) g_page_protect[pg] = protect;
}

// Commits pages (g_mu held); newly committed pages are zero.
bool CommitLocked(uint32_t lo, uint32_t size, uint32_t protect) {
    if (!os::MemCommit(g_guest_base + lo, size, protect)) return false;
    SetPages(lo, size, uint16_t(protect ? protect : PAGE_READWRITE));
    return true;
}

}  // namespace

void GuestMemoryInit() {
    for (uint64_t k = 1; k < 1024 && !g_guest_base; k++)
        g_guest_base = static_cast<uint8_t*>(os::MemReserve(reinterpret_cast<void*>(k * kSpace), kSpace));
    if (!g_guest_base) Fatal("cannot reserve the 4 GB guest address space");
    g_page_protect.assign(kSpace / kPage, 0);
    Log("guest memory: 4 GB reserved at %p", g_guest_base);
}

// Is [lo, lo + sz) inside a recorded block? (g_mu held)
bool CoveredLocked(uint32_t lo, uint32_t sz) {
    auto it = g_used.upper_bound(lo);
    if (it == g_used.begin()) return false;
    --it;
    return uint64_t(lo) + sz <= uint64_t(it->first) + it->second;
}

void* GuestCommit(uint32_t addr, size_t size, uint32_t protect) {
    std::lock_guard<std::mutex> lk(g_mu);
    uint32_t lo = addr & ~(kPage - 1), sz = RoundUp(addr + size, kPage) - lo;
    if (!CommitLocked(lo, sz, protect)) return nullptr;
    if (!CoveredLocked(lo, sz)) g_used[lo] = sz;  // fixed-address region of its own
    return g_guest_base + addr;
}

// First fit in the heap range; records the block (g_mu held).
uint32_t PlaceLocked(uint32_t sz, uint32_t a) {
    uint32_t cand = kHeapLo;
    for (auto it = g_used.begin(); it != g_used.end(); ++it) {
        uint32_t start = it->first, end = start + it->second;
        if (end <= cand) continue;
        if (start >= RoundUp(cand, a) + uint64_t(sz)) break;
        cand = end;
    }
    cand = RoundUp(cand, a);
    if (uint64_t(cand) + sz > kHeapHi) return 0;
    g_used[cand] = sz;
    return cand;
}

void* GuestReserve(size_t size, size_t align) {
    std::lock_guard<std::mutex> lk(g_mu);
    uint32_t at = PlaceLocked(RoundUp(size ? size : 1, kPage), uint32_t(align < kPage ? kPage : align));
    return at ? g_guest_base + at : nullptr;
}

void GuestDecommit(uint32_t addr, size_t size) {
    std::lock_guard<std::mutex> lk(g_mu);
    uint32_t lo = addr & ~(kPage - 1), sz = RoundUp(addr + size, kPage) - lo;
    os::MemDecommit(g_guest_base + lo, sz);
    SetPages(lo, sz, 0);
}

void* GuestAlloc(size_t size, uint32_t protect, size_t align) {
    std::lock_guard<std::mutex> lk(g_mu);
    uint32_t sz = RoundUp(size ? size : 1, kPage);
    uint32_t cand = PlaceLocked(sz, uint32_t(align < kPage ? kPage : align));
    if (!cand) return nullptr;
    if (!CommitLocked(cand, sz, protect)) {
        g_used.erase(cand);
        return nullptr;
    }
    memset(g_guest_base + cand, 0, sz);
    return g_guest_base + cand;
}

void GuestFree(void* p) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_used.find(H2G(p));
    if (it == g_used.end()) return;
    os::MemDecommit(g_guest_base + it->first, it->second);
    SetPages(it->first, it->second, 0);
    g_used.erase(it);
}

size_t GuestAllocationSize(const void* p) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_used.find(H2G(p));
    return it == g_used.end() ? 0 : it->second;
}

bool GuestValid(uint32_t addr, size_t size) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!size || uint64_t(addr) + size > kSpace) return false;
    for (uint64_t pg = addr / kPage; pg <= (uint64_t(addr) + size - 1) / kPage; pg++)
        if (!g_page_protect[pg]) return false;
    return true;
}

bool GuestQuery(uint32_t addr, GuestRegion* r) {
    std::lock_guard<std::mutex> lk(g_mu);
    uint32_t page = addr & ~(kPage - 1);
    *r = {};
    r->base = page;
    auto it = g_used.upper_bound(page);
    bool in_block = false;
    uint32_t block_end = 0;
    if (it != g_used.begin()) {
        auto prev = std::prev(it);
        if (uint64_t(page) < uint64_t(prev->first) + prev->second) {
            in_block = true;
            r->allocation_base = prev->first;
            block_end = prev->first + prev->second;
        }
    }
    if (!in_block) {  // free up to the next block
        r->state = 0x10000;  // MEM_FREE
        r->size = (it == g_used.end() ? uint32_t(kSpace - kPage) : it->first) - page;
        return true;
    }
    uint16_t prot = g_page_protect[page / kPage];
    uint32_t end = page;
    while (end < block_end && g_page_protect[end / kPage] == prot) end += kPage;
    r->size = end - page;
    r->state = prot ? MEM_COMMIT : MEM_RESERVE;
    r->protect = prot;
    r->allocation_protect = PAGE_READWRITE;
    return true;
}

bool GuestProtect(uint32_t addr, size_t size, uint32_t protect) {
    std::lock_guard<std::mutex> lk(g_mu);
    uint32_t lo = addr & ~(kPage - 1), sz = RoundUp(addr + size, kPage) - lo;
    if (!os::MemProtect(g_guest_base + lo, sz, protect)) return false;
    SetPages(lo, sz, uint16_t(protect));
    return true;
}
#endif
