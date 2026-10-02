// xboxkrnl replacement: thunk installation plus memory, thread, sync, RTL and
// miscellaneous exports. File I/O lives in kernel_io.cpp.
#include "kernel.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "../generated/kernel_names.h"
#include "hle.h"
#include "ob.h"
#include "os.h"

#if UINTPTR_MAX == 0xFFFFFFFFu
#include <intrin.h>

#include "nt.h"

// The title's return address in a kernel export called by native code.
#define KT_TITLE_CALLER() (static_cast<uint32_t*>(_AddressOfReturnAddress())[0])

namespace nt {
#define KT_DEFINE(ret, name, args) ret(NTAPI* name) args;
KT_NT_FUNCS(KT_DEFINE)
#undef KT_DEFINE

void Init() {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
#define KT_RESOLVE(ret, name, args)                                              \
    name = reinterpret_cast<decltype(name)>(GetProcAddress(ntdll, #name));       \
    if (!name) Fatal("ntdll!%s not found", #name);
    KT_NT_FUNCS(KT_RESOLVE)
#undef KT_RESOLVE
}
}  // namespace nt
#else
#define KT_TITLE_CALLER() 0u  // unknown here (lifted code calls through HleInvoker)
#endif

namespace {

// ---- Data exports ------------------------------------------------------------

struct { USHORT Major, Minor, Build, Qfe; } x_XboxKrnlVersion = {1, 0, 5838, 1};
struct { ULONG Flags; UCHAR GpuRevision, McpRevision, Unknown3, Unknown4; } x_XboxHardwareInfo = {0, 0xD2, 0xD4, 0, 0};
volatile ULONG x_KeTickCount = 0;
uint32_t x_LaunchDataPage = 0;
ULONG x_HalBootSMCVideoMode = 1;
ULONG x_HalDiskCachePartitionCount = 3;
UCHAR x_XboxHDKey[16];
UCHAR x_XboxSignatureKey[16];
UCHAR x_XboxAlternateSignatureKeys[16][16];
UCHAR x_ExEventObjectType[0x40];
UCHAR x_PsThreadObjectType[0x40];
UCHAR x_IdexChannelObject[0x40];  // IDE channel object (no IDE hardware here)

#if UINTPTR_MAX == 0xFFFFFFFFu
// ---- Unimplemented-export stubs ----------------------------------------------

extern "C" __declspec(noreturn) void __stdcall KernelUnimplemented(uint32_t ordinal) {
    uint32_t caller = static_cast<uint32_t*>(_AddressOfReturnAddress())[2];
    uint32_t base = 0;
    const char* from = HleSymbolAt(caller, &base);
    Fatal("kernel export #%u %s is not implemented (called from %08x%s%s)", ordinal,
          ordinal < 379 ? kKernelNames[ordinal] : "?", caller, from ? " in " : "", from ? from : "");
}

void* MakeUnimplementedStub(uint32_t ordinal) {
    uint8_t* s = ExecAlloc(16);
    s[0] = 0x68;  // push ordinal
    memcpy(s + 1, &ordinal, 4);
    s[5] = 0xE8;  // call KernelUnimplemented
    uint32_t rel = uint32_t(reinterpret_cast<uintptr_t>(&KernelUnimplemented)) - uint32_t(reinterpret_cast<uintptr_t>(s + 10));
    memcpy(s + 6, &rel, 4);
    return s;
}
#endif


// ---- Memory ------------------------------------------------------------------

constexpr ULONG kXboxMemNoZero = 0x800000;
constexpr ULONG kXboxPageCacheBits = PAGE_NOCACHE | PAGE_WRITECOMBINE;

// Sizes and addresses are 32-bit guest values ([in, out] through guest memory).
NTSTATUS NTAPI x_NtAllocateVirtualMemory(uint32_t* base, uint32_t zero_bits, uint32_t* size, ULONG type,
                                         ULONG protect) {
#if UINTPTR_MAX == 0xFFFFFFFFu
    PVOID b = reinterpret_cast<PVOID>(uintptr_t(*base));
    SIZE_T sz = *size;
    NTSTATUS st = nt::NtAllocateVirtualMemory(GetCurrentProcess(), &b, zero_bits, &sz, type & ~kXboxMemNoZero,
                                              protect & ~kXboxPageCacheBits);
    *base = uint32_t(uintptr_t(b)), *size = uint32_t(sz);
    return st;
#else
    (void)zero_bits;
    ULONG p = protect & ~kXboxPageCacheBits;
    bool commit = (type & MEM_COMMIT) != 0;
    if (!*base) {
        void* mem = commit ? GuestAlloc(*size, p, 0x10000) : GuestReserve(*size);
        if (!mem) return STATUS_NO_MEMORY_X;
        *base = H2G(mem);
    } else if (commit) {
        if (!GuestCommit(*base, *size, p)) return STATUS_NO_MEMORY_X;
        *base &= ~0xFFFu;
    }
    *size = (*size + 0xFFF) & ~0xFFFu;
    return STATUS_SUCCESS;
#endif
}

NTSTATUS NTAPI x_NtFreeVirtualMemory(uint32_t* base, uint32_t* size, ULONG type) {
#if UINTPTR_MAX == 0xFFFFFFFFu
    PVOID b = reinterpret_cast<PVOID>(uintptr_t(*base));
    SIZE_T sz = *size;
    NTSTATUS st = nt::NtFreeVirtualMemory(GetCurrentProcess(), &b, &sz, type);
    *base = uint32_t(uintptr_t(b)), *size = uint32_t(sz);
    return st;
#else
    if (type & MEM_DECOMMIT)
        GuestDecommit(*base, *size);
    else
        GuestFree(G2H(*base));
    return STATUS_SUCCESS;
#endif
}

// MEMORY_BASIC_INFORMATION as the title sees it (pointers are 32-bit).
struct XMemoryBasicInformation {
    uint32_t BaseAddress, AllocationBase, AllocationProtect, RegionSize, State, Protect, Type;
};

NTSTATUS NTAPI x_NtQueryVirtualMemory(void* base, XMemoryBasicInformation* info) {
    GuestRegion r;
    if (!GuestQuery(H2G(base), &r)) return STATUS_INVALID_PARAMETER_X;
    constexpr uint32_t kMemPrivate = 0x20000;
    *info = {r.base, r.allocation_base, r.allocation_protect, r.size, r.state, r.protect,
             r.state == 0x10000 ? 0 : kMemPrivate};
    return STATUS_SUCCESS;
}

void* NTAPI x_MmAllocateContiguousMemoryEx(uint32_t size, uint32_t, uint32_t, uint32_t align, ULONG protect) {
    ULONG p = protect & ~kXboxPageCacheBits;
    return GuestAlloc(size, p ? p : PAGE_READWRITE, align);
}

void* NTAPI x_MmAllocateContiguousMemory(uint32_t size) { return GuestAlloc(size); }

VOID NTAPI x_MmFreeContiguousMemory(void* base) { GuestFree(base); }

VOID NTAPI x_MmPersistContiguousMemory(void*, uint32_t, BOOLEAN) {}

uint32_t NTAPI x_MmQueryAllocationSize(void* base) { return uint32_t(GuestAllocationSize(base)); }

ULONG NTAPI x_MmQueryAddressProtect(void* va) {
    GuestRegion r;
    return GuestQuery(H2G(va), &r) ? r.protect : 0;
}

VOID NTAPI x_MmSetAddressProtect(void* base, ULONG size, ULONG protect) {
    GuestProtect(H2G(base), size, protect & ~kXboxPageCacheBits);
}

// Pool blocks live in guest memory; a 16-byte header keeps the size.
void* NTAPI x_ExAllocatePoolWithTag(uint32_t size, ULONG) {
    auto* p = static_cast<uint8_t*>(GuestAlloc(size + 16, PAGE_READWRITE, 16));
    if (!p) return nullptr;
    memcpy(p, &size, 4);
    return p + 16;
}
void* NTAPI x_ExAllocatePool(uint32_t size) { return x_ExAllocatePoolWithTag(size, 0); }
VOID NTAPI x_ExFreePool(void* p) {
    if (p) GuestFree(static_cast<uint8_t*>(p) - 16);
}
ULONG NTAPI x_ExQueryPoolBlockSize(void* p) {
    uint32_t size = 0;
    if (p) memcpy(&size, static_cast<uint8_t*>(p) - 16, 4);
    return size;
}

// ---- Threads -----------------------------------------------------------------

NTSTATUS NTAPI x_PsCreateSystemThreadEx(GHandle* thread_handle, uint32_t, uint32_t stack_size, uint32_t tls_size,
                                        GHandle* thread_id, uint32_t start_routine, uint32_t start_context,
                                        BOOLEAN create_suspended, BOOLEAN, uint32_t system_routine) {
    XThread* t = XThreadCreate(tls_size);
    t->start_routine = start_routine;
    t->start_context = start_context;
    t->system_routine = system_routine;
    static std::atomic<uint32_t> next_id{0x100};
    t->tid = next_id += 4;
    size_t reserve = std::max<size_t>(size_t(stack_size) * 4, 1u << 20);
    auto th = ob::CreateThread(
        t,
        [t] {
            XThreadBindCurrent(t);  // binds the fake KPCR to this host thread
            if (t->system_routine)
                CallGuest(t->system_routine, {t->start_routine, t->start_context});
            else
                CallGuest(t->start_routine, {t->start_context});
        },
        create_suspended != 0, reserve);
    if (!th) {
        Log("PsCreateSystemThreadEx: thread creation failed, stack=%u tls=%u", stack_size, tls_size);
        return STATUS_NO_MEMORY_X;
    }
    t->thread = th.get();
    GHandle h = ob::Insert(th);
    if (thread_handle) *thread_handle = h;
    if (thread_id) *thread_id = t->tid;
    Log("PsCreateSystemThreadEx: start=%08x ctx=%08x sys=%08x tls=%u stack=%u -> tid %u%s", start_routine,
        start_context, system_routine, tls_size, stack_size, t->tid, create_suspended ? " (suspended)" : "");
    return STATUS_SUCCESS;
}

VOID NTAPI x_PsTerminateSystemThread(NTSTATUS status) { ob::ExitCurrentThread(status); }

NTSTATUS NTAPI x_NtResumeThread(GHandle h, PULONG prev) {
    auto t = ob::GetAs<ob::Thread>(h, ob::Type::Thread);
    if (!t) return STATUS_INVALID_HANDLE_X;
    if (prev) *prev = t->started ? 0 : 1;
    ob::Resume(t.get());
    return STATUS_SUCCESS;
}

LONG NTAPI x_KeSetBasePriorityThread(PVOID kthread, LONG increment) {
    XThread* t = XThreadFromKthread(kthread);
    LONG old = t->priority;
    t->priority = increment;
    if (t->thread) os::SetThreadPriority(t->thread->native, increment);
    return old;
}

NTSTATUS NTAPI x_ObReferenceObjectByHandle(GHandle h, uint32_t type, uint32_t* object) {
    if (auto t = ob::GetAs<ob::Thread>(h, ob::Type::Thread)) {
        if (t->x) {
            *object = H2G(t->x->kthread);
            return STATUS_SUCCESS;
        }
    }
    Log("ObReferenceObjectByHandle: unknown handle %08x (type %08x)", h, type);
    return STATUS_INVALID_HANDLE_X;
}

VOID __fastcall x_ObfDereferenceObject(uint32_t) {}

// ---- Time and dispatcher objects ------------------------------------------------

VOID NTAPI x_KeQuerySystemTime(PLARGE_INTEGER t) { t->QuadPart = LONGLONG(os::SystemTime100ns()); }

const int64_t* Timeout(PLARGE_INTEGER t) { return t ? reinterpret_cast<const int64_t*>(&t->QuadPart) : nullptr; }

NTSTATUS NTAPI x_KeDelayExecutionThread(CHAR, BOOLEAN alertable, PLARGE_INTEGER interval) {
    return ob::Delay(alertable != 0, Timeout(interval));
}

NTSTATUS NTAPI x_NtCreateEvent(GHandle* h, XOBJECT_ATTRIBUTES* oa, ULONG type, BOOLEAN initial) {
    if (oa && oa->ObjectName && oa->ObjectName->Length)
        Log("NtCreateEvent: named event '%.*s' is created unnamed", int(oa->ObjectName->Length), oa->ObjectName->Buffer.get());
    // EVENT_TYPE: 0 = NotificationEvent (manual reset), 1 = SynchronizationEvent
    *h = ob::Insert(std::make_shared<ob::Event>(type == 0, initial != 0));
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI x_NtSetEvent(GHandle h, PLONG prev) {
    auto e = ob::GetAs<ob::Event>(h, ob::Type::Event);
    if (!e) return STATUS_INVALID_HANDLE_X;
    LONG p = ob::SetEvent(e.get());
    if (prev) *prev = p;
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI x_NtClearEvent(GHandle h) {
    auto e = ob::GetAs<ob::Event>(h, ob::Type::Event);
    if (!e) return STATUS_INVALID_HANDLE_X;
    ob::ResetEvent(e.get());
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI x_NtPulseEvent(GHandle h, PLONG prev) {
    auto e = ob::GetAs<ob::Event>(h, ob::Type::Event);
    if (!e) return STATUS_INVALID_HANDLE_X;
    LONG p = ob::PulseEvent(e.get());
    if (prev) *prev = p;
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI x_NtCreateSemaphore(GHandle* h, XOBJECT_ATTRIBUTES*, LONG initial, LONG maximum) {
    *h = ob::Insert(std::make_shared<ob::Semaphore>(initial, maximum));
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI x_NtReleaseSemaphore(GHandle h, LONG count, PLONG prev) {
    auto s = ob::GetAs<ob::Semaphore>(h, ob::Type::Semaphore);
    if (!s) return STATUS_INVALID_HANDLE_X;
    int32_t p = 0;
    NTSTATUS st = ob::ReleaseSemaphore(s.get(), count, &p);
    if (prev) *prev = p;
    return st;
}

NTSTATUS NTAPI x_NtCreateMutant(GHandle* h, XOBJECT_ATTRIBUTES*, BOOLEAN initial_owner) {
    *h = ob::Insert(std::make_shared<ob::Mutant>(initial_owner ? XThreadCurrent() : nullptr));
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI x_NtReleaseMutant(GHandle h, PLONG prev) {
    auto m = ob::GetAs<ob::Mutant>(h, ob::Type::Mutant);
    if (!m) return STATUS_INVALID_HANDLE_X;
    int32_t p = 0;
    NTSTATUS st = ob::ReleaseMutant(m.get(), &p);
    if (prev) *prev = p;
    return st;
}

std::shared_ptr<ob::Waitable> WaitableOf(GHandle h) {
    std::shared_ptr<ob::Object> o = ob::Get(h);
    if (!o || o->type == ob::Type::File || o->type == ob::Type::SymbolicLink) return nullptr;
    return std::static_pointer_cast<ob::Waitable>(o);
}

NTSTATUS NTAPI x_NtWaitForSingleObjectEx(GHandle h, CHAR, BOOLEAN alertable, PLARGE_INTEGER timeout) {
    auto w = WaitableOf(h);
    if (!w) {
        Log("NtWaitForSingleObject: handle %08x is not waitable", h);
        return STATUS_INVALID_HANDLE_X;
    }
    return ob::Wait({w}, false, alertable != 0, Timeout(timeout));
}

NTSTATUS NTAPI x_NtWaitForSingleObject(GHandle h, BOOLEAN alertable, PLARGE_INTEGER timeout) {
    return x_NtWaitForSingleObjectEx(h, 1, alertable, timeout);
}

// WAIT_TYPE: 0 = WaitAll, 1 = WaitAny
NTSTATUS NTAPI x_NtWaitForMultipleObjectsEx(ULONG count, const GHandle* handles, ULONG wait_type, CHAR,
                                            BOOLEAN alertable, PLARGE_INTEGER timeout) {
    std::vector<std::shared_ptr<ob::Waitable>> objects;
    for (ULONG i = 0; i < count; i++) {
        auto w = WaitableOf(handles[i]);
        if (!w) return STATUS_INVALID_HANDLE_X;
        objects.push_back(w);
    }
    return ob::Wait(objects, wait_type == 0, alertable != 0, Timeout(timeout));
}

// Kernel-mode waits on dispatcher objects in title memory (XAPI uses a few).
NTSTATUS NTAPI x_KeWaitForSingleObject(void* object, ULONG, CHAR, BOOLEAN alertable, PLARGE_INTEGER timeout) {
    auto w = std::make_shared<ob::GuestDispatcher>(static_cast<uint8_t*>(object));
    return ob::Wait({w}, false, alertable != 0, Timeout(timeout));
}

LONG NTAPI x_KeSetEvent(void* event, LONG, BOOLEAN) { return ob::SetGuestSignal(static_cast<uint8_t*>(event), 1); }

LONG NTAPI x_KeResetEvent(void* event) { return ob::SetGuestSignal(static_cast<uint8_t*>(event), 0); }

VOID NTAPI x_KeInitializeEvent(void* event, ULONG type, BOOLEAN state) {
    auto* h = static_cast<uint8_t*>(event);
    memset(h, 0, 16);
    h[0] = UCHAR(type);  // NotificationEvent 0 / SynchronizationEvent 1
    h[2] = 4;            // Size in dwords
    *reinterpret_cast<int32_t*>(h + 4) = state ? 1 : 0;
    uint32_t list = H2G(h + 8);  // empty WaitListHead
    memcpy(h + 8, &list, 4), memcpy(h + 12, &list, 4);
}

NTSTATUS NTAPI x_NtYieldExecution() {
    std::this_thread::yield();
    return STATUS_SUCCESS;
}

// ---- IRQL ----------------------------------------------------------------------

UCHAR SwapIrql(UCHAR irql) {
    XThread* t = XThreadCurrent();
    if (!t) return 0;
    UCHAR old = t->kpcr[kpcr_off::kIrql];
    t->kpcr[kpcr_off::kIrql] = irql;
    return old;
}

UCHAR NTAPI x_KeRaiseIrqlToDpcLevel() { return SwapIrql(2); }
UCHAR __fastcall x_KfRaiseIrql(UCHAR irql) { return SwapIrql(irql); }
VOID __fastcall x_KfLowerIrql(UCHAR irql) { SwapIrql(irql); }

// ---- Critical sections -----------------------------------------------------------
// Owner is the fake KTHREAD pointer, matching what Xbox code expects to see.
// Contended waiters sleep on a hashed condition variable keyed by the lock.

LONG CurrentOwner() {
    XThread* t = XThreadCurrent();
    return t ? LONG(H2G(t->kthread)) : LONG(os::CurrentThreadId());
}

struct AddressWaiters {
    std::mutex mu;
    std::condition_variable cv;
};
AddressWaiters g_address_waiters[64];

AddressWaiters& WaitersOf(const volatile void* p) { return g_address_waiters[(uintptr_t(p) >> 4) % 64]; }

LONG Cas(volatile LONG* p, LONG exchange, LONG comparand) { return os::AtomicCompareExchange(p, exchange, comparand); }

VOID NTAPI x_RtlInitializeCriticalSection(XRTL_CRITICAL_SECTION* cs) {
    memset(cs, 0, sizeof *cs);
    cs->LockCount = -1;
}

VOID NTAPI x_RtlEnterCriticalSection(XRTL_CRITICAL_SECTION* cs) {
    LONG me = CurrentOwner();
    if (cs->OwningThread == me) {
        cs->RecursionCount++;
        return;
    }
    for (int spin = 0;; spin++) {
        if (Cas(&cs->OwningThread, me, 0) == 0) break;
        if (spin < 64) continue;
        AddressWaiters& w = WaitersOf(&cs->OwningThread);
        std::unique_lock<std::mutex> lk(w.mu);
        if (cs->OwningThread != 0) w.cv.wait_for(lk, std::chrono::milliseconds(5));
    }
    cs->RecursionCount = 1;
    os::AtomicIncrement(&cs->LockCount);
}

BOOLEAN NTAPI x_RtlTryEnterCriticalSection(XRTL_CRITICAL_SECTION* cs) {
    LONG me = CurrentOwner();
    if (cs->OwningThread == me) {
        cs->RecursionCount++;
        return TRUE;
    }
    if (Cas(&cs->OwningThread, me, 0) != 0) return FALSE;
    cs->RecursionCount = 1;
    os::AtomicIncrement(&cs->LockCount);
    return TRUE;
}

VOID NTAPI x_RtlLeaveCriticalSection(XRTL_CRITICAL_SECTION* cs) {
    if (--cs->RecursionCount > 0) return;
    os::AtomicDecrement(&cs->LockCount);
    os::AtomicExchange(&cs->OwningThread, LONG(0));
    AddressWaiters& w = WaitersOf(&cs->OwningThread);
    { std::lock_guard<std::mutex> lk(w.mu); }
    w.cv.notify_all();
}

// ---- RTL ---------------------------------------------------------------------------

VOID NTAPI x_RtlInitAnsiString(XANSI_STRING* s, const char* src) {
    s->Buffer = const_cast<char*>(src);  // a guest string: stored as its guest address
    s->Length = src ? USHORT(strlen(src)) : 0;
    s->MaximumLength = src ? s->Length + 1 : 0;
}

BOOLEAN NTAPI x_RtlEqualString(const XANSI_STRING* a, const XANSI_STRING* b, BOOLEAN case_insensitive) {
    if (a->Length != b->Length) return FALSE;
    const char *pa = a->Buffer.get(), *pb = b->Buffer.get();
    if (!case_insensitive) return memcmp(pa, pb, a->Length) == 0;
    for (USHORT i = 0; i < a->Length; i++)
        if (tolower((unsigned char)pa[i]) != tolower((unsigned char)pb[i])) return FALSE;
    return TRUE;
}

// Variadic: from lifted code only the format string is shown.
void DbgPrintFromLifted(KtCpu* c, HleConv) {
    const char* fmt = G2H<const char>(rd32(c->esp + 4));
    Log("DbgPrint: %s", fmt ? fmt : "(null)");
    c->eax = 0;
    c->eip = rd32(c->esp);
    c->esp += 4;
}

// ---- RTL helpers that the 32-bit build forwards to ntdll ------------------------------
// (64-bit ntdll uses 64-bit layouts, so these work on the title's structures.)

struct XUNICODE_STRING {
    USHORT Length, MaximumLength;
    GPtr<WCHAR> Buffer;
};

ULONG NTAPI x_RtlNtStatusToDosError(NTSTATUS status) { return StatusToDosError(status); }

ULONG NTAPI x_RtlCompareMemoryUlong(const void* src, ULONG length, ULONG pattern) {
    const auto* p = static_cast<const uint8_t*>(src);
    ULONG n = 0;
    for (; n + 4 <= length; n += 4) {
        ULONG v;
        memcpy(&v, p + n, 4);
        if (v != pattern) break;
    }
    return n;
}

void* NTAPI x_ExAllocatePool(uint32_t size);
VOID NTAPI x_ExFreePool(void* p);

NTSTATUS NTAPI x_RtlAnsiStringToUnicodeString(XUNICODE_STRING* dst, const XANSI_STRING* src, BOOLEAN allocate) {
    USHORT bytes = USHORT(src->Length * 2);
    if (allocate) {
        dst->Buffer = static_cast<WCHAR*>(x_ExAllocatePool(bytes + 2));
        dst->MaximumLength = USHORT(bytes + 2);
    } else if (dst->MaximumLength < bytes) {
        return STATUS_BUFFER_OVERFLOW_X;
    }
    for (USHORT i = 0; i < src->Length; i++) dst->Buffer[i] = WCHAR(uint8_t(src->Buffer[i]));
    dst->Length = bytes;
    if (dst->MaximumLength > bytes) dst->Buffer[src->Length] = 0;
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI x_RtlUnicodeStringToAnsiString(XANSI_STRING* dst, const XUNICODE_STRING* src, BOOLEAN allocate) {
    USHORT chars = USHORT(src->Length / 2);
    if (allocate) {
        dst->Buffer = static_cast<char*>(x_ExAllocatePool(chars + 1));
        dst->MaximumLength = USHORT(chars + 1);
    } else if (dst->MaximumLength < chars) {
        return STATUS_BUFFER_OVERFLOW_X;
    }
    for (USHORT i = 0; i < chars; i++) {
        WCHAR w = src->Buffer[i];
        dst->Buffer[i] = w < 0x80 ? char(w) : '?';
    }
    dst->Length = chars;
    if (dst->MaximumLength > chars) dst->Buffer[chars] = 0;
    return STATUS_SUCCESS;
}

// Exceptions raised by title code. The 32-bit build runs them on Win32 SEH;
// without native title code, resuming in an __except block is not supported.
VOID NTAPI x_RtlRaiseException(const uint32_t* record) {
    Fatal("RtlRaiseException(code %08x at %08x): structured exceptions are not supported without native title code",
          record ? record[0] : 0, record ? record[3] : 0);
}

VOID NTAPI x_RtlUnwind(uint32_t frame, uint32_t target_ip, uint32_t record, uint32_t value) {
    Fatal("RtlUnwind(frame %08x, target %08x, record %08x, value %08x): not supported without native title code",
          frame, target_ip, record, value);
}

ULONG __cdecl x_DbgPrint(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    size_t n = strlen(buf);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
    Log("DbgPrint: %s", buf);
    return 0;
}

VOID NTAPI x_KeBugCheck(ULONG code) {
    Log("KeBugCheck(0x%08lx) called from %08x", code, KT_TITLE_CALLER());
#if UINTPTR_MAX == 0xFFFFFFFFu
    LogTitleStack(_AddressOfReturnAddress());
#endif
    LogBacktrace("KeBugCheck");
    Fatal("KeBugCheck(0x%08lx)", code);
}

VOID NTAPI x_HalReturnToFirmware(ULONG routine) {
    Log("HalReturnToFirmware(%lu): title requested reboot/exit (called from %08x)", routine, KT_TITLE_CALLER());
#if UINTPTR_MAX == 0xFFFFFFFFu
    LogTitleStack(_AddressOfReturnAddress());
#endif
    os::Exit(0);
}

VOID NTAPI x_HalInitiateShutdown() {
    Log("HalInitiateShutdown");
    os::Exit(0);
}

BOOLEAN NTAPI x_HalIsResetOrShutdownPending() { return FALSE; }

// Shutdown notifications never run: the runtime has no orderly shutdown.
VOID NTAPI x_HalRegisterShutdownNotification(void*, BOOLEAN) {}

// ---- XBE sections and caches --------------------------------------------------------
// All sections are mapped up front, so loading only maintains the refcount.

struct XbeSectionHeader {
    ULONG flags, va, vsize, raw, rsize, name_addr;
    LONG ref_count;
};

NTSTATUS NTAPI x_XeLoadSection(XbeSectionHeader* s) {
    os::AtomicIncrement(&s->ref_count);
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI x_XeUnloadSection(XbeSectionHeader* s) {
    os::AtomicDecrement(&s->ref_count);
    return STATUS_SUCCESS;
}

ULONG NTAPI x_FscGetCacheSize() { return 16; }
NTSTATUS NTAPI x_FscSetCacheSize(ULONG) { return STATUS_SUCCESS; }

// ---- EEPROM settings -----------------------------------------------------------------

// The console's game region: one the title's certificate allows (XAPI sends
// the console to the dashboard otherwise). North America when allowed, else
// Japan, else the rest of the world. KT_REGION=na|jp|eu overrides.
ULONG GameRegion() {
    constexpr ULONG kNa = 1, kJapan = 2, kRestOfWorld = 4;
    if (const char* r = os::Env("KT_REGION")) {
        std::string s = r;
        return s == "jp" || s == "ja" ? kJapan : s == "eu" || s == "row" ? kRestOfWorld : kNa;
    }
    // XBE header at 0x10000 -> certificate -> GameRegion (+0xA0).
    const uint32_t cert = *G2H<uint32_t>(0x10118);
    const ULONG allowed = cert ? *G2H<uint32_t>(cert + 0xA0) : kNa;
    return (allowed & kNa) ? kNa : (allowed & kJapan) ? kJapan : (allowed & kRestOfWorld) ? kRestOfWorld : kNa;
}

// XC_LANGUAGE: KT_LANGUAGE=en|ja|<number>; by default Japanese on a Japanese console.
ULONG Language() {
    if (const char* l = os::Env("KT_LANGUAGE")) {
        std::string s = l;
        return s == "en" ? 1 : s == "ja" ? 2 : ULONG(atoi(l));
    }
    return GameRegion() == 2 ? 2 : 1;
}

NTSTATUS NTAPI x_ExQueryNonVolatileSetting(ULONG index, PULONG type, void* value, ULONG length, PULONG result_len) {
    ULONG v = 0;
    switch (index) {
        case 0x7: v = Language(); break;  // XC_LANGUAGE: 1 = English, 2 = Japanese
        case 0x8: v = 0; break;           // XC_VIDEO flags
        case 0x9: v = 0; break;           // XC_AUDIO flags
        case 0x103:                       // XC_FACTORY_AV_REGION: NTSC-J in Japan, else NTSC-M (60 Hz)
            v = GameRegion() == 2 ? 0x00400200 : 0x00400100;
            break;
        case 0x104: v = GameRegion(); break;  // XC_FACTORY_GAME_REGION
        default: Log("ExQueryNonVolatileSetting: index 0x%lx -> 0", index); break;
    }
    if (type) *type = 4;  // REG_DWORD
    if (value && length) {
        memset(value, 0, length);
        memcpy(value, &v, length < 4 ? length : 4);
    }
    if (result_len) *result_len = 4;
    return STATUS_SUCCESS;
}

// ---- SHA-1 (XcSHA*) -------------------------------------------------------------------

struct Sha1 {
    uint32_t h[5];
    uint64_t bytes;
    uint8_t buf[64];
};
static_assert(sizeof(Sha1) <= 116, "XcSHA context size");

uint32_t Rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

void Sha1Block(Sha1* s, const uint8_t* p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) w[i] = p[4 * i] << 24 | p[4 * i + 1] << 16 | p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; i++) w[i] = Rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) f = (b & c) | (~b & d), k = 0x5A827999;
        else if (i < 40) f = b ^ c ^ d, k = 0x6ED9EBA1;
        else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
        else f = b ^ c ^ d, k = 0xCA62C1D6;
        uint32_t t = Rol(a, 5) + f + e + k + w[i];
        e = d, d = c, c = Rol(b, 30), b = a, a = t;
    }
    s->h[0] += a, s->h[1] += b, s->h[2] += c, s->h[3] += d, s->h[4] += e;
}

VOID NTAPI x_XcSHAInit(Sha1* s) {
    *s = {{0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0}, 0, {}};
}

VOID NTAPI x_XcSHAUpdate(Sha1* s, const uint8_t* in, ULONG len) {
    while (len--) {
        s->buf[s->bytes++ % 64] = *in++;
        if (s->bytes % 64 == 0) Sha1Block(s, s->buf);
    }
}

VOID NTAPI x_XcSHAFinal(Sha1* s, uint8_t* digest) {
    uint64_t bits = s->bytes * 8;
    uint8_t pad = 0x80;
    x_XcSHAUpdate(s, &pad, 1);
    pad = 0;
    while (s->bytes % 64 != 56) x_XcSHAUpdate(s, &pad, 1);
    for (int i = 7; i >= 0; i--) {
        uint8_t b = uint8_t(bits >> (8 * i));
        x_XcSHAUpdate(s, &b, 1);
    }
    for (int i = 0; i < 20; i++) digest[i] = uint8_t(s->h[i / 4] >> (24 - 8 * (i % 4)));
}

// ---- Timer thread for KeTickCount -------------------------------------------------------

// The title reads KeTickCount through its import; on 64-bit hosts that is a
// copy in guest memory (KernelInstallThunks).
volatile ULONG* g_tick_count = &x_KeTickCount;

void TickThread() {
    os::HighResolutionTimers();
    auto start = std::chrono::steady_clock::now();
    for (;;) {
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
        *g_tick_count = ULONG(ms.count());
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

}  // namespace

std::vector<KExport> KernelCoreExports() {
#if UINTPTR_MAX == 0xFFFFFFFFu
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    // Exports whose x86 signature and semantics match ntdll exactly are
    // forwarded without a wrapper (RtlUnwind in particular must not get one).
    auto fwd = [&](const char* name) {
        return KExport{name, reinterpret_cast<void*>(GetProcAddress(ntdll, name)), nullptr, HleConv::Stdcall, false};
    };
#endif
    return {
        KXD(XboxKrnlVersion), KXD(XboxHardwareInfo), KXD(KeTickCount), KXD(LaunchDataPage),
        KXD(HalBootSMCVideoMode), KXD(HalDiskCachePartitionCount), KXD(XboxHDKey), KXD(XboxSignatureKey),
        KXD(XboxAlternateSignatureKeys), KXD(ExEventObjectType), KXD(PsThreadObjectType), KXD(IdexChannelObject),

        KX(NtAllocateVirtualMemory), KX(NtFreeVirtualMemory), KX(NtQueryVirtualMemory),
        KX(MmAllocateContiguousMemory), KX(MmAllocateContiguousMemoryEx), KX(MmFreeContiguousMemory),
        KX(MmPersistContiguousMemory), KX(MmQueryAllocationSize), KX(MmQueryAddressProtect),
        KX(MmSetAddressProtect), KX(ExAllocatePool), KX(ExAllocatePoolWithTag), KX(ExFreePool),
        KX(ExQueryPoolBlockSize),

        KX(PsCreateSystemThreadEx), KX(PsTerminateSystemThread), KX(NtResumeThread), KX(KeSetBasePriorityThread),
        KX(ObReferenceObjectByHandle), KXF(ObfDereferenceObject),

        KX(KeQuerySystemTime), KX(KeDelayExecutionThread), KX(NtCreateEvent), KX(NtSetEvent), KX(NtClearEvent),
        KX(NtPulseEvent), KX(NtCreateSemaphore), KX(NtReleaseSemaphore), KX(NtCreateMutant), KX(NtReleaseMutant),
        KX(NtWaitForSingleObject), KX(NtWaitForSingleObjectEx), KX(NtWaitForMultipleObjectsEx), KX(NtYieldExecution),
        KX(KeWaitForSingleObject), KX(KeSetEvent), KX(KeResetEvent), KX(KeInitializeEvent),

        KX(KeRaiseIrqlToDpcLevel), KXF(KfRaiseIrql), KXF(KfLowerIrql),

        KX(RtlInitializeCriticalSection), KX(RtlEnterCriticalSection), KX(RtlTryEnterCriticalSection),
        KX(RtlLeaveCriticalSection), KX(RtlInitAnsiString), KX(RtlEqualString),
#if UINTPTR_MAX == 0xFFFFFFFFu
        fwd("RtlUnwind"), fwd("RtlRaiseException"), fwd("RtlNtStatusToDosError"), fwd("RtlCompareMemoryUlong"),
        fwd("RtlAnsiStringToUnicodeString"), fwd("RtlUnicodeStringToAnsiString"),
#else
        KX(RtlUnwind), KX(RtlRaiseException), KX(RtlNtStatusToDosError), KX(RtlCompareMemoryUlong),
        KX(RtlAnsiStringToUnicodeString), KX(RtlUnicodeStringToAnsiString),
#endif

        KExport{"DbgPrint", (void*)&x_DbgPrint, &DbgPrintFromLifted, HleConv::Cdecl, false}, KX(KeBugCheck), KX(HalReturnToFirmware), KX(HalInitiateShutdown),
        KX(HalIsResetOrShutdownPending), KX(HalRegisterShutdownNotification), KX(XeLoadSection), KX(XeUnloadSection), KX(FscGetCacheSize),
        KX(FscSetCacheSize), KX(ExQueryNonVolatileSetting), KX(XcSHAInit), KX(XcSHAUpdate), KX(XcSHAFinal),
    };
}

namespace {

#if UINTPTR_MAX == 0xFFFFFFFFu
// With KT_TRACE set, every call to an implemented export is logged.
extern "C" void __stdcall KernelTrace(uint32_t ordinal) {
    // [ret][ordinal][8 x pushad][title return address]
    uint32_t caller = static_cast<uint32_t*>(_AddressOfReturnAddress())[10];
    Log("trace: %s (from %08x)", kKernelNames[ordinal], caller);
}

void* MakeTraceStub(uint32_t ordinal, void* impl) {
    uint8_t* s = ExecAlloc(32);
    uint8_t* c = s;
    auto rel = [&](const void* to) {
        uint32_t r = uint32_t(reinterpret_cast<uintptr_t>(to)) - uint32_t(reinterpret_cast<uintptr_t>(c + 4));
        memcpy(c, &r, 4);
        c += 4;
    };
    *c++ = 0x60;                                   // pushad
    *c++ = 0x68; memcpy(c, &ordinal, 4); c += 4;   // push ordinal
    *c++ = 0xE8; rel(reinterpret_cast<void*>(&KernelTrace));
    *c++ = 0x61;                                   // popad
    *c++ = 0xE9; rel(impl);
    return s;
}
#endif


}  // namespace

#if UINTPTR_MAX != 0xFFFFFFFFu
namespace {
std::unordered_map<uint32_t, std::string> g_unimplemented_imports;
}

const char* KernelImportName(uint32_t va) {
    auto it = g_unimplemented_imports.find(va);
    return it == g_unimplemented_imports.end() ? nullptr : it->second.c_str();
}

// Every import becomes a guest address: functions route to their invoker,
// data exports are copied into guest memory.
void KernelInstallThunks(uint32_t thunk_table) {
    std::unordered_map<std::string, KExport> impl;
    for (auto& list : {KernelCoreExports(), KernelIoExports(), KernelTimerExports()})
        for (const KExport& e : list) impl[e.name] = e;
    auto* thunks = G2H<uint32_t>(thunk_table);
    int total = 0, done = 0;
    for (; thunks[total]; total++) {
        uint32_t ordinal = thunks[total] & 0x7FFFFFFF;
        const char* name = ordinal < 379 ? kKernelNames[ordinal] : "";
        auto it = impl.find(name);
        if (it != impl.end() && it->second.data) {
            size_t size = std::string(name) == "XboxAlternateSignatureKeys" ? 256 : 64;
            void* copy = GuestAlloc(size);
            memcpy(copy, it->second.ptr, size);
            if (std::string(name) == "KeTickCount") g_tick_count = static_cast<volatile ULONG*>(copy);
            thunks[total] = H2G(copy);
            done++;
        } else if (it != impl.end() && it->second.invoke) {
            thunks[total] = HleGuestCallable(it->second.ptr, it->second.invoke, it->second.conv);
            done++;
        } else {
            uint32_t va = H2G(GuestAlloc(16, PAGE_READWRITE, 16));
            g_unimplemented_imports[va] = name;
            thunks[total] = va;
        }
    }
    Log("kernel: %d imports, %d implemented", total, done);
}
#else
const char* KernelImportName(uint32_t) { return nullptr; }

void KernelInstallThunks(uint32_t thunk_table) {
    nt::Init();
    const bool trace = GetEnvironmentVariableA("KT_TRACE", nullptr, 0) > 0;
    std::unordered_map<std::string, KExport> impl;
    for (auto& list : {KernelCoreExports(), KernelIoExports(), KernelTimerExports()})
        for (const KExport& e : list) {
            if (!e.ptr) Fatal("kernel export %s has no host address", e.name);
            impl[e.name] = e;
        }

    auto* thunks = reinterpret_cast<uint32_t*>(thunk_table);
    int total = 0, done = 0;
    for (; thunks[total]; total++) {
        uint32_t ordinal = thunks[total] & 0x7FFFFFFF;
        const char* name = ordinal < 379 ? kKernelNames[ordinal] : "";
        auto it = impl.find(name);
        if (it != impl.end()) {
            void* target = trace && !it->second.data ? MakeTraceStub(ordinal, it->second.ptr) : it->second.ptr;
            thunks[total] = uint32_t(reinterpret_cast<uintptr_t>(target));
            done++;
        } else {
            thunks[total] = uint32_t(reinterpret_cast<uintptr_t>(MakeUnimplementedStub(ordinal)));
        }
    }
    Log("kernel: %d imports, %d implemented", total, done);
}
#endif

void KernelStartTimers() { std::thread(TickThread).detach(); }

// The NT status -> Win32 error mapping XAPI relies on (GetLastError after a
// failed kernel call), for the codes this kernel returns.
ULONG StatusToDosError(NTSTATUS st) {
    switch (uint32_t(st)) {
        case 0x00000000: return 0;
        case 0x00000102: return 258;   // WAIT_TIMEOUT
        case 0x80000005: return 234;   // ERROR_MORE_DATA
        case 0x80000006: return 18;    // ERROR_NO_MORE_FILES
        case 0xC0000002: return 1;     // ERROR_INVALID_FUNCTION
        case 0xC0000008: return 6;     // ERROR_INVALID_HANDLE
        case 0xC000000D: return 87;    // ERROR_INVALID_PARAMETER
        case 0xC000000F: return 2;     // ERROR_FILE_NOT_FOUND
        case 0xC0000010: return 1;     // ERROR_INVALID_FUNCTION
        case 0xC0000011: return 38;    // ERROR_HANDLE_EOF
        case 0xC0000017: return 8;     // ERROR_NOT_ENOUGH_MEMORY
        case 0xC0000022: return 5;     // ERROR_ACCESS_DENIED
        case 0xC0000023: return 122;   // ERROR_INSUFFICIENT_BUFFER
        case 0xC0000034: return 2;     // ERROR_FILE_NOT_FOUND
        case 0xC0000035: return 183;   // ERROR_ALREADY_EXISTS
        case 0xC000003A: return 3;     // ERROR_PATH_NOT_FOUND
        case 0xC0000046: return 288;   // ERROR_NOT_OWNER
        case 0xC0000047: return 298;   // ERROR_TOO_MANY_POSTS
        case 0xC00000BA: return 5;     // ERROR_ACCESS_DENIED
        case 0xC0000101: return 145;   // ERROR_DIR_NOT_EMPTY
        case 0xC0000103: return 267;   // ERROR_DIRECTORY
        case 0xC0000185: return 1117;  // ERROR_IO_DEVICE
        default: return 317;           // ERROR_MR_MID_NOT_FOUND, as NT does
    }
}
