// Internal registry of implemented xboxkrnl exports.
#pragma once

#include <vector>

#include "hle_invoke.h"
#include "host.h"

struct KExport {
    const char* name;
    void* ptr;            // function, or the variable itself for data exports
    HleInvokeFn invoke;   // call from lifted code (null for data exports)
    HleConv conv;
    bool data;
};

// Each kernel_*.cpp contributes its exports.
std::vector<KExport> KernelCoreExports();
std::vector<KExport> KernelIoExports();
std::vector<KExport> KernelTimerExports();

// NT status -> Win32 error (RtlNtStatusToDosError) for the codes we return.
ULONG StatusToDosError(NTSTATUS st);

#define KX(fn) KExport{#fn, (void*)&x_##fn, &HleInvoker<&x_##fn>::Call, HleConv::Stdcall, false}
#define KXF(fn) KExport{#fn, (void*)&x_##fn, &HleInvoker<&x_##fn>::Call, HleConv::Fastcall, false}
#define KXD(var) KExport{#var, (void*)&x_##var, nullptr, HleConv::Stdcall, true}
