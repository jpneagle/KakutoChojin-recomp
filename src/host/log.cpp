#include <cstdarg>
#include <cstdio>
#include <mutex>

#include "host.h"

namespace {
FILE* g_log = nullptr;
std::mutex g_mu;

void VLog(const char* prefix, const char* fmt, va_list ap) {
    char buf[32768];
    vsnprintf(buf, sizeof buf, fmt, ap);
    std::lock_guard<std::mutex> lk(g_mu);
    unsigned tid = os::CurrentThreadId();
    fprintf(stderr, "[%5u] %s%s\n", tid, prefix, buf);
    if (g_log) {
        fprintf(g_log, "[%5u] %s%s\n", tid, prefix, buf);
        fflush(g_log);
    }
}
}  // namespace

void LogInit(const std::filesystem::path& path) { g_log = os::OpenFile(path, "w"); }

void Log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    VLog("", fmt, ap);
    va_end(ap);
}

void Fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    VLog("FATAL: ", fmt, ap);
    va_end(ap);
    os::Exit(1);
}
