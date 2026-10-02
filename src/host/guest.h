// The Xbox (guest) address space as seen from host code.
//
// Title code works with 32-bit addresses. In the 32-bit Windows build the
// XBE is mapped at its real addresses, so a guest address is a host pointer
// (g_guest_base == 0). In 64-bit builds the 4 GB guest space is a reserved
// host region starting at g_guest_base, and every guest address that host
// code dereferences goes through G2H; every host object handed to the title
// must live inside that region (GuestAlloc) and is passed back with H2G.
//
// Structures shared with the title therefore never contain host pointers:
// they use GPtr<T> (a 4-byte guest pointer) and GHandle (a 32-bit handle).
#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>

extern "C" uint8_t* g_guest_base;

inline void* G2H(uint32_t addr) { return addr ? g_guest_base + addr : nullptr; }
template <typename T>
inline T* G2H(uint32_t addr) {
    return static_cast<T*>(G2H(addr));
}
inline uint32_t H2G(const void* p) {
    return p ? uint32_t(static_cast<const uint8_t*>(p) - g_guest_base) : 0;
}

// A pointer stored in guest memory (4 bytes on every host).
template <typename T>
struct GPtr {
    uint32_t addr;

    T* get() const { return G2H<T>(addr); }
    T* operator->() const { return get(); }
    T& operator*() const { return *get(); }
    T& operator[](size_t i) const { return get()[i]; }
    explicit operator bool() const { return addr != 0; }
    GPtr& operator=(T* p) {
        addr = H2G(p);
        return *this;
    }
};
static_assert(sizeof(GPtr<int>) == 4, "GPtr must match the guest pointer size");

// Guest handles are 32 bits; Win32 handles fit in 32 bits (sign-extended).
using GHandle = uint32_t;
inline void* HandleFromGuest(GHandle h) { return reinterpret_cast<void*>(intptr_t(int32_t(h))); }
inline GHandle HandleToGuest(void* h) { return GHandle(uint32_t(reinterpret_cast<uintptr_t>(h))); }

// Calls title code at `fn` (stdcall) with 32-bit arguments on the calling
// thread and returns EAX: natively in the 32-bit build, through the lifted
// code otherwise (guest_call.cpp).
uint32_t CallGuest(uint32_t fn, std::initializer_list<uint32_t> args);

// ---- Guest memory (guest_mem.cpp) --------------------------------------------

// Reserves the guest address space (64-bit builds; a no-op on 32-bit).
void GuestMemoryInit();
// Commits [addr, addr + size) at a fixed guest address (XBE sections).
void* GuestCommit(uint32_t addr, size_t size, uint32_t protect);
// Allocates committed, zeroed guest memory anywhere (title allocations and
// host objects the title reads). Returns a host pointer inside the region.
void* GuestAlloc(size_t size, uint32_t protect = 0x04 /* PAGE_READWRITE */, size_t align = 0x1000);
void GuestFree(void* p);
// Reserves guest address space without committing it (NtAllocateVirtualMemory
// with MEM_RESERVE only); pages are committed later with GuestCommit.
void* GuestReserve(size_t size, size_t align = 0x10000);
// Decommits pages inside an allocation (MEM_DECOMMIT); the range stays reserved.
void GuestDecommit(uint32_t addr, size_t size);
// Size of the allocation that starts at p (0 if p is not an allocation start).
size_t GuestAllocationSize(const void* p);
// Is [addr, addr + size) inside committed guest memory?
bool GuestValid(uint32_t addr, size_t size);

// A run of pages with the same state (NtQueryVirtualMemory).
struct GuestRegion {
    uint32_t base, allocation_base, size;
    uint32_t state;    // MEM_COMMIT / MEM_RESERVE / MEM_FREE
    uint32_t protect;  // PAGE_* (0 if not committed)
    uint32_t allocation_protect;
};
bool GuestQuery(uint32_t addr, GuestRegion* out);
bool GuestProtect(uint32_t addr, size_t size, uint32_t protect);
