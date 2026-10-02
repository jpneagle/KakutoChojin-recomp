// Shared declarations for the runtime that replaces the Xbox kernel and
// hardware libraries (kt_host.dll in the 32-bit build, KakutoChojin.exe otherwise).
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "guest.h"
#include "os.h"
#include "xtypes.h"

typedef LONG NTSTATUS_X;  // Xbox NTSTATUS; same values as NT

#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS ((NTSTATUS)0x00000000L)
#endif
#define STATUS_NOT_IMPLEMENTED_X ((NTSTATUS)0xC0000002L)
#define STATUS_INVALID_HANDLE_X ((NTSTATUS)0xC0000008L)
#define STATUS_INVALID_PARAMETER_X ((NTSTATUS)0xC000000DL)
#define STATUS_INVALID_DEVICE_REQUEST_X ((NTSTATUS)0xC0000010L)
#define STATUS_NO_MEMORY_X ((NTSTATUS)0xC0000017L)
#define STATUS_OBJECT_NAME_NOT_FOUND_X ((NTSTATUS)0xC0000034L)
#define STATUS_BUFFER_TOO_SMALL_X ((NTSTATUS)0xC0000023L)
#define STATUS_BUFFER_OVERFLOW_X ((NTSTATUS)0x80000005L)

// ---- Xbox kernel structures (x86 layouts) ----------------------------------

struct XANSI_STRING {
    USHORT Length;
    USHORT MaximumLength;
    GPtr<char> Buffer;
};
static_assert(sizeof(XANSI_STRING) == 8, "Xbox ANSI_STRING layout");

struct XOBJECT_ATTRIBUTES {
    GHandle RootDirectory;
    GPtr<XANSI_STRING> ObjectName;
    ULONG Attributes;
};
static_assert(sizeof(XOBJECT_ATTRIBUTES) == 12, "Xbox OBJECT_ATTRIBUTES layout");

// IO_STATUS_BLOCK as the title sees it (16 bytes on a 64-bit host).
struct XIO_STATUS_BLOCK {
    LONG Status;
    ULONG Information;
};

// Xbox RTL_CRITICAL_SECTION: a DISPATCHER_HEADER followed by the lock state.
struct XRTL_CRITICAL_SECTION {
    UCHAR Header[16];
    LONG LockCount;
    LONG RecursionCount;
    volatile LONG OwningThread;  // guest address of the owner's KTHREAD
};
static_assert(sizeof(XRTL_CRITICAL_SECTION) == 0x1C, "Xbox CRITICAL_SECTION layout");

// ---- Fake KPCR / KTHREAD ----------------------------------------------------
// Title code reads a few KPCR fields through fs:. Those reads are patched to
// load from the XThread of the calling thread (see kpcr.cpp).

namespace kpcr_off {
constexpr unsigned kTlsPointer = 0x04;     // NtTib.StackBase holds TlsData on Xbox
constexpr unsigned kSelfPcr = 0x1C;
constexpr unsigned kPrcb = 0x20;
constexpr unsigned kIrql = 0x24;
constexpr unsigned kCurrentThread = 0x28;  // Prcb (at 0x28) -> CurrentThread
}  // namespace kpcr_off
constexpr unsigned kKthreadTlsData = 0x28;

namespace ob {
struct Thread;
}

struct XThread {
    alignas(16) uint8_t kpcr[0x300];
    alignas(16) uint8_t kthread[0x200];
    uint32_t tls_data;       // guest address of the TLS block
    uint32_t tid;            // thread id reported to the title
    ob::Thread* thread;      // kernel thread object (host only)
    LONG priority;
    uint32_t start_routine;  // guest addresses
    uint32_t start_context;
    uint32_t system_routine;
};

XThread* XThreadCreate(size_t tls_size);
XThread* XThreadCurrent();
// Makes `t` the calling thread's XThread (its fake KPCR for fs: accesses).
void XThreadBindCurrent(XThread* t);
// Gives a host-created thread a fake KPCR (and title-sized TLS) so it can
// call into title code.
XThread* XThreadAdoptCurrent();
void XThreadSetTitleTlsSize(size_t size);
XThread* XThreadFromKthread(void* kthread);
void KpcrInit();

// ---- Subsystems -------------------------------------------------------------

struct Manifest;
void LogInit(const std::filesystem::path& path);
void Log(const char* fmt, ...);
[[noreturn]] void Fatal(const char* fmt, ...);

void KernelInstallThunks(uint32_t thunk_table);
// Name of an unimplemented kernel import at a guest address (64-bit hosts).
const char* KernelImportName(uint32_t va);
void KernelStartTimers();
void IoSetGameRoot(const std::filesystem::path& root);
void IoSetHddRoot(const std::filesystem::path& root);

void HleInstall(const Manifest& m);
const char* HleSymbolAt(uint32_t va, uint32_t* base);
void CrashHandlerInstall();
void LogTitleStack(const void* esp);
void LogBacktrace(const char* what);

#if defined(_WIN32) && UINTPTR_MAX == 0xFFFFFFFFu
// Switches functions selected by KT_LIFT to kt_lifted.dll (lift.cpp).
void LiftInstall(const Manifest& m);
// Strict lift mode: redirects execute faults in title code (lift.cpp).
bool LiftHandleExecFault(EXCEPTION_POINTERS* ep);

// Executable scratch memory for trampolines (main.cpp).
uint8_t* ExecAlloc(size_t n);
#endif

// Directory of the runtime binary.
inline std::filesystem::path HostDir() { return os::HostDir(); }
