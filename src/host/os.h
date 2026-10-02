// Small host OS helpers (threads, time, memory, settings) with Windows and
// POSIX versions. Portable host code goes through these instead of Win32.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <string>

namespace os {

// ---- Time and threads ----

// Wall-clock time in 100 ns units since 1601-01-01 (Windows FILETIME / Xbox time).
uint64_t SystemTime100ns();

// Starts a detached thread with at least `stack_reserve` bytes of stack.
bool StartThread(std::function<void()> fn, size_t stack_reserve);
// A handle to the calling thread usable from other threads (for priorities).
uintptr_t CurrentThreadNative();
uint32_t CurrentThreadId();
// Xbox priority increment (-16 .. 16) -> host priority, where supported.
void SetThreadPriority(uintptr_t native, int increment);
[[noreturn]] void ExitThread();
// Ends the process at once (no static destructors: other threads still run).
[[noreturn]] void Exit(int code);

// Requests 1 ms timer/sleep resolution for the process (Windows).
void HighResolutionTimers();

// ---- Environment and settings ----

// Environment variable, or null.
const char* Env(const char* name);
inline bool EnvSet(const char* name) { return Env(name) != nullptr; }
inline int EnvInt(const char* name, int def) {
    const char* v = Env(name);
    return v ? atoi(v) : def;
}

// Directory of the runtime binary (kt_host.dll / KakutoChojin.exe).
std::filesystem::path HostDir();
// [section] key=value from KakutoChojin.ini next to the runtime.
std::string IniString(const char* section, const char* key, const std::string& def);
int IniInt(const char* section, const char* key, int def);
// Sets [section] key=value in KakutoChojin.ini (other lines and comments are kept).
void IniSet(const char* section, const char* key, const std::string& value);

// fopen with a path that may contain non-ASCII characters.
FILE* OpenFile(const std::filesystem::path& p, const char* mode);

// ---- Virtual memory (protections are Windows PAGE_* values) ----

// Reserves `size` bytes at exactly `at` (no access); null on failure.
void* MemReserve(void* at, size_t size);
bool MemCommit(void* p, size_t size, uint32_t page_protect);  // zero-filled when newly committed
void MemDecommit(void* p, size_t size);
bool MemProtect(void* p, size_t size, uint32_t page_protect);

// ---- Atomics on 32-bit values shared with title code ----

template <typename T>
T AtomicIncrement(volatile T* p);
template <typename T>
T AtomicDecrement(volatile T* p);
template <typename T>
T AtomicExchange(volatile T* p, T v);
template <typename T>
T AtomicCompareExchange(volatile T* p, T exchange, T comparand);
template <typename T>
T AtomicOr(volatile T* p, T bits);  // returns the previous value

}  // namespace os

#ifdef _MSC_VER
#include <intrin.h>
namespace os {
template <typename T>
T AtomicIncrement(volatile T* p) {
    static_assert(sizeof(T) == 4, "32-bit only");
    return T(_InterlockedIncrement(reinterpret_cast<volatile long*>(p)));
}
template <typename T>
T AtomicDecrement(volatile T* p) {
    static_assert(sizeof(T) == 4, "32-bit only");
    return T(_InterlockedDecrement(reinterpret_cast<volatile long*>(p)));
}
template <typename T>
T AtomicExchange(volatile T* p, T v) {
    static_assert(sizeof(T) == 4, "32-bit only");
    return T(_InterlockedExchange(reinterpret_cast<volatile long*>(p), long(v)));
}
template <typename T>
T AtomicCompareExchange(volatile T* p, T exchange, T comparand) {
    static_assert(sizeof(T) == 4, "32-bit only");
    return T(_InterlockedCompareExchange(reinterpret_cast<volatile long*>(p), long(exchange), long(comparand)));
}
template <typename T>
T AtomicOr(volatile T* p, T bits) {
    static_assert(sizeof(T) == 4, "32-bit only");
    return T(_InterlockedOr(reinterpret_cast<volatile long*>(p), long(bits)));
}
}  // namespace os
#else
namespace os {
template <typename T>
T AtomicIncrement(volatile T* p) {
    return __atomic_add_fetch(p, T(1), __ATOMIC_SEQ_CST);
}
template <typename T>
T AtomicDecrement(volatile T* p) {
    return __atomic_sub_fetch(p, T(1), __ATOMIC_SEQ_CST);
}
template <typename T>
T AtomicExchange(volatile T* p, T v) {
    return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST);
}
template <typename T>
T AtomicCompareExchange(volatile T* p, T exchange, T comparand) {
    __atomic_compare_exchange_n(p, &comparand, exchange, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return comparand;
}
template <typename T>
T AtomicOr(volatile T* p, T bits) {
    return __atomic_fetch_or(p, bits, __ATOMIC_SEQ_CST);
}
}  // namespace os
#endif
