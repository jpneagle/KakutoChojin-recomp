// os.h for Windows and POSIX.
#include "os.h"

#include <cctype>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")
#else
#include <pthread.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace os {

// ---- Portable parts ----------------------------------------------------------------

uint64_t SystemTime100ns() {
    // 1601-01-01 -> 1970-01-01
    constexpr uint64_t kEpochDelta = 116444736000000000ull;
    auto since_1970 = std::chrono::system_clock::now().time_since_epoch();
    return kEpochDelta + uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(since_1970).count() / 100);
}

const char* Env(const char* name) {
    const char* v = getenv(name);
    return v && *v ? v : nullptr;
}

void Exit(int code) { std::_Exit(code); }

namespace {

std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) a++;
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

std::string Lower(std::string s) {
    for (char& c : s) c = char(tolower((unsigned char)c));
    return s;
}

std::mutex g_ini_mu;

// KakutoChojin.ini, read once: "section.key" -> value. ';' and '#' start comments.
std::map<std::string, std::string>& Ini() {
    static std::map<std::string, std::string> values;
    static std::once_flag once;
    std::call_once(once, [] {
        FILE* f = OpenFile(HostDir() / "KakutoChojin.ini", "r");
        if (!f) return;
        char line[512];
        std::string section;
        while (fgets(line, sizeof line, f)) {
            std::string s = line;
            size_t c = s.find_first_of(";#");
            if (c != std::string::npos) s.resize(c);
            s = Trim(s);
            if (s.empty()) continue;
            if (s.front() == '[' && s.back() == ']') {
                section = Lower(Trim(s.substr(1, s.size() - 2)));
                continue;
            }
            size_t eq = s.find('=');
            if (eq == std::string::npos) continue;
            values[section + "." + Lower(Trim(s.substr(0, eq)))] = Trim(s.substr(eq + 1));
        }
        fclose(f);
    });
    return values;
}

}  // namespace

std::string IniString(const char* section, const char* key, const std::string& def) {
    std::lock_guard<std::mutex> lk(g_ini_mu);
    auto it = Ini().find(Lower(section) + "." + Lower(key));
    return it == Ini().end() ? def : it->second;
}

void IniSet(const char* section, const char* key, const std::string& value) {
    std::lock_guard<std::mutex> lk(g_ini_mu);
    Ini()[Lower(section) + "." + Lower(key)] = value;
    // Rewrite the file: replace the key's line in its section, or add it.
    std::vector<std::string> lines;
    if (FILE* f = OpenFile(HostDir() / "KakutoChojin.ini", "r")) {
        char buf[512];
        while (fgets(buf, sizeof buf, f)) {
            std::string l = buf;
            while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
            lines.push_back(l);
        }
        fclose(f);
    }
    std::string want = Lower(section), cur;
    int section_end = -1, found = -1;
    for (int i = 0; i < int(lines.size()); i++) {
        std::string t = Trim(lines[i]);
        if (!t.empty() && t.front() == '[' && t.back() == ']') {
            cur = Lower(Trim(t.substr(1, t.size() - 2)));
            continue;
        }
        if (cur != want) continue;
        section_end = i + 1;
        size_t eq = t.find('=');
        if (eq != std::string::npos && t[0] != ';' && t[0] != '#' && Lower(Trim(t.substr(0, eq))) == Lower(key)) found = i;
    }
    std::string line = std::string(key) + "=" + value;
    if (found >= 0) {
        lines[found] = line;
    } else if (section_end >= 0) {
        lines.insert(lines.begin() + section_end, line);
    } else {
        lines.push_back("[" + std::string(section) + "]");
        lines.push_back(line);
    }
    if (FILE* f = OpenFile(HostDir() / "KakutoChojin.ini", "w")) {
        for (const std::string& l : lines) fprintf(f, "%s\n", l.c_str());
        fclose(f);
    }
}

int IniInt(const char* section, const char* key, int def) {
    std::string v = IniString(section, key, "");
    return v.empty() ? def : atoi(v.c_str());
}

#ifdef _WIN32

// ---- Windows -------------------------------------------------------------------------------

namespace {
DWORD WINAPI ThreadMain(void* arg) {
    auto* fn = static_cast<std::function<void()>*>(arg);
    (*fn)();
    delete fn;
    return 0;
}

DWORD Protection(uint32_t page_protect) { return page_protect & ~uint32_t(PAGE_NOCACHE | PAGE_WRITECOMBINE); }
}  // namespace

bool StartThread(std::function<void()> fn, size_t stack_reserve) {
    auto* heap = new std::function<void()>(std::move(fn));
    HANDLE h = ::CreateThread(nullptr, stack_reserve, ThreadMain, heap, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!h) {
        delete heap;
        return false;
    }
    CloseHandle(h);
    return true;
}

uintptr_t CurrentThreadNative() {
    HANDLE h = nullptr;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h, 0, FALSE, DUPLICATE_SAME_ACCESS);
    return reinterpret_cast<uintptr_t>(h);
}

uint32_t CurrentThreadId() { return GetCurrentThreadId(); }

void SetThreadPriority(uintptr_t native, int increment) {
    int prio = increment >= 16 ? THREAD_PRIORITY_TIME_CRITICAL
             : increment <= -16 ? THREAD_PRIORITY_IDLE
             : increment > 2 ? THREAD_PRIORITY_HIGHEST
             : increment < -2 ? THREAD_PRIORITY_LOWEST
             : increment;
    if (native) ::SetThreadPriority(reinterpret_cast<HANDLE>(native), prio);
}

void ExitThread() { ::ExitThread(0); }

void HighResolutionTimers() { timeBeginPeriod(1); }

fs::path HostDir() {
    static const fs::path dir = [] {
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&HostDir), &self);
        wchar_t buf[MAX_PATH];
        GetModuleFileNameW(self, buf, MAX_PATH);
        return fs::path(buf).parent_path();
    }();
    return dir;
}

FILE* OpenFile(const fs::path& p, const char* mode) {
    return _wfopen(p.c_str(), std::wstring(mode, mode + strlen(mode)).c_str());
}

void* MemReserve(void* at, size_t size) {
    void* p = VirtualAlloc(at, size, MEM_RESERVE, PAGE_NOACCESS);
    if (p && p != at) VirtualFree(p, 0, MEM_RELEASE), p = nullptr;
    return p;
}

bool MemCommit(void* p, size_t size, uint32_t page_protect) {
    return VirtualAlloc(p, size, MEM_COMMIT, Protection(page_protect)) != nullptr;
}

void MemDecommit(void* p, size_t size) { VirtualFree(p, size, MEM_DECOMMIT); }

bool MemProtect(void* p, size_t size, uint32_t page_protect) {
    DWORD old;
    return VirtualProtect(p, size, Protection(page_protect), &old) != 0;
}

#else

// ---- POSIX ---------------------------------------------------------------------------------

namespace {
void* ThreadMain(void* arg) {
    auto* fn = static_cast<std::function<void()>*>(arg);
    (*fn)();
    delete fn;
    return nullptr;
}

int Protection(uint32_t page_protect) {
    switch (page_protect & 0xFF) {
        case 0x02: return PROT_READ;                            // PAGE_READONLY
        case 0x04: case 0x08: return PROT_READ | PROT_WRITE;   // PAGE_READWRITE / WRITECOPY
        case 0x10: return PROT_EXEC;                            // PAGE_EXECUTE
        case 0x20: return PROT_READ | PROT_EXEC;                // PAGE_EXECUTE_READ
        case 0x40: case 0x80: return PROT_READ | PROT_WRITE | PROT_EXEC;
        default: return PROT_NONE;                              // PAGE_NOACCESS
    }
}
}  // namespace

bool StartThread(std::function<void()> fn, size_t stack_reserve) {
    auto* heap = new std::function<void()>(std::move(fn));
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    // Lifted code nests host frames deeply; keep at least the usual 8 MB.
    pthread_attr_setstacksize(&attr, stack_reserve < (8u << 20) ? (8u << 20) : stack_reserve);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t t;
    int err = pthread_create(&t, &attr, ThreadMain, heap);
    pthread_attr_destroy(&attr);
    if (err) {
        delete heap;
        return false;
    }
    return true;
}

uintptr_t CurrentThreadNative() { return uintptr_t(pthread_self()); }

uint32_t CurrentThreadId() { return uint32_t(syscall(SYS_gettid)); }

// Raising priorities needs privileges on most systems; titles work without.
void SetThreadPriority(uintptr_t, int) {}

void ExitThread() { pthread_exit(nullptr); }

void HighResolutionTimers() {}

fs::path HostDir() {
    static const fs::path dir = [] {
        std::error_code ec;
        fs::path exe = fs::read_symlink("/proc/self/exe", ec);
        return ec ? fs::current_path() : exe.parent_path();
    }();
    return dir;
}

FILE* OpenFile(const fs::path& p, const char* mode) { return fopen(p.c_str(), mode); }

void* MemReserve(void* at, size_t size) {
    int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;
#ifdef MAP_FIXED_NOREPLACE
    flags |= MAP_FIXED_NOREPLACE;
#endif
    void* p = mmap(at, size, PROT_NONE, flags, -1, 0);
    if (p == MAP_FAILED) return nullptr;
    if (p != at) {
        munmap(p, size);
        return nullptr;
    }
    return p;
}

bool MemCommit(void* p, size_t size, uint32_t page_protect) { return mprotect(p, size, Protection(page_protect)) == 0; }

// Dropped pages read back as zero once committed again, as on Windows.
void MemDecommit(void* p, size_t size) {
    madvise(p, size, MADV_DONTNEED);
    mprotect(p, size, PROT_NONE);
}

bool MemProtect(void* p, size_t size, uint32_t page_protect) { return mprotect(p, size, Protection(page_protect)) == 0; }

#endif

}  // namespace os
