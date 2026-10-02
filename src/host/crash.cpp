// Crash reporting: first-chance hardware faults are logged (titles may handle
// some themselves via SEH); anything unhandled ends the run with a report.
#include "host.h"

#ifdef _WIN32

namespace {

constexpr uint32_t kImageLo = 0x10000, kImageHi = 0x01000000;

#if UINTPTR_MAX != 0xFFFFFFFFu
void Report(const char* what, EXCEPTION_POINTERS* ep) {
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    const CONTEXT* c = ep->ContextRecord;
    HMODULE m = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(c->Rip), &m))
        GetModuleFileNameA(m, name, MAX_PATH);
    Log("%s: code=%08lx rip=%p (%s+0x%llx)", what, er->ExceptionCode, reinterpret_cast<void*>(c->Rip), name,
        (unsigned long long)(c->Rip - uintptr_t(m)));
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
        uintptr_t a = er->ExceptionInformation[1];
        Log("  %s address %p (guest %08x)", er->ExceptionInformation[0] ? "write to" : "read from",
            reinterpret_cast<void*>(a), unsigned(a - uintptr_t(g_guest_base)));
    }
}
#else
void Report(const char* what, EXCEPTION_POINTERS* ep) {
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    const CONTEXT* c = ep->ContextRecord;
    uint32_t base = 0;
    const char* sym = HleSymbolAt(c->Eip, &base);
    if (c->Eip >= kImageHi) {
        HMODULE m = nullptr;
        char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(uintptr_t(c->Eip)), &m))
            GetModuleFileNameA(m, name, MAX_PATH);
        Log("%s in %s+0x%lx", what, name, c->Eip - DWORD(uintptr_t(m)));
    }
    Log("%s: code=%08lx eip=%08lx%s%s%s", what, er->ExceptionCode, c->Eip, sym ? " (" : "", sym ? sym : "",
        sym ? ")" : "");
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
        Log("  %s address %08lx", er->ExceptionInformation[0] ? "write to" : "read from",
            static_cast<unsigned long>(er->ExceptionInformation[1]));
    Log("  eax=%08lx ebx=%08lx ecx=%08lx edx=%08lx esi=%08lx edi=%08lx ebp=%08lx esp=%08lx", c->Eax, c->Ebx,
        c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
    LogTitleStack(reinterpret_cast<const void*>(uintptr_t(c->Esp)));
}
#endif

}  // namespace

// Logs values on the stack that look like return addresses into the title.
void LogTitleStack(const void* esp) {
    auto* sp = static_cast<const uint32_t*>(esp);
    char line[512];
    int n = 0, len = 0;
    for (int i = 0; i < 4096 && n < 24; i++) {
        uint32_t v;
        if (!ReadProcessMemory(GetCurrentProcess(), sp + i, &v, 4, nullptr)) break;
        if (v >= kImageLo && v < kImageHi) {
            len += snprintf(line + len, sizeof line - len, " %08x", v);
            n++;
        }
    }
    if (n) Log("  stack:%s", line);
}

// Frame-pointer backtrace (title code is compiled with EBP frames).
void LogBacktrace(const char* what) {
    void* frames[48];
    USHORT n = RtlCaptureStackBackTrace(0, 48, frames, nullptr);
    char line[1024];
    int len = 0;
    for (USHORT i = 0; i < n && len < 900; i++) {
        uintptr_t a = reinterpret_cast<uintptr_t>(frames[i]);
        if (a >= kImageLo && a < kImageHi) len += snprintf(line + len, sizeof line - len, " %08x", unsigned(a));
        else len += snprintf(line + len, sizeof line - len, " [host]");
    }
    Log("  backtrace (%s):%s", what, line);
}

namespace {

// Ring-0 cache maintenance the title issues after writing GPU-visible memory
// (wbinvd / invd) is meaningless here: skip the instruction.
bool SkipCacheInstruction(EXCEPTION_POINTERS* ep) {
#if UINTPTR_MAX != 0xFFFFFFFFu
    (void)ep;
    return false;
#else
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_PRIV_INSTRUCTION) return false;
    auto* ip = reinterpret_cast<const uint8_t*>(uintptr_t(ep->ContextRecord->Eip));
    if (ip[0] == 0x0F && (ip[1] == 0x09 || ip[1] == 0x08)) {
        ep->ContextRecord->Eip += 2;
        return true;
    }
    return false;
#endif
}

LONG CALLBACK FirstChance(EXCEPTION_POINTERS* ep) {
#if UINTPTR_MAX == 0xFFFFFFFFu
    if (LiftHandleExecFault(ep)) return EXCEPTION_CONTINUE_EXECUTION;
#endif
    if (SkipCacheInstruction(ep)) return EXCEPTION_CONTINUE_EXECUTION;
    switch (ep->ExceptionRecord->ExceptionCode) {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_PRIV_INSTRUCTION:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_STACK_OVERFLOW:
            Report("first-chance exception", ep);
            break;
        case 0x406D1388:  // thread naming
        case DBG_PRINTEXCEPTION_C:
            break;
        default: {
            static LONG logged = 0;
            if (os::AtomicIncrement(&logged) <= 20) Report("first-chance exception (other)", ep);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI Unhandled(EXCEPTION_POINTERS* ep) {
    Report("UNHANDLED exception", ep);
    ExitProcess(3);
}

}  // namespace

void CrashHandlerInstall() {
    AddVectoredExceptionHandler(1, FirstChance);
    SetUnhandledExceptionFilter(Unhandled);
}

#else  // POSIX: fatal signals are reported and end the run.

#include <signal.h>

#include <cstring>

namespace {

void OnSignal(int sig, siginfo_t* info, void*) {
    uintptr_t a = reinterpret_cast<uintptr_t>(info->si_addr);
    bool guest = g_guest_base && a >= uintptr_t(g_guest_base) && a - uintptr_t(g_guest_base) < (1ull << 32);
    Log("UNHANDLED signal %d (%s) at address %p%s", sig, strsignal(sig), info->si_addr, guest ? " (guest memory)" : "");
    if (guest) Log("  guest address %08x", unsigned(a - uintptr_t(g_guest_base)));
    os::Exit(3);
}

}  // namespace

void LogTitleStack(const void*) {}

void LogBacktrace(const char* what) { Log("  backtrace (%s): not available on this host", what); }

void CrashHandlerInstall() {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = OnSignal;
    sa.sa_flags = SA_SIGINFO;
    for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE}) sigaction(sig, &sa, nullptr);
}

#endif
