// Registry of host implementations for XDK library functions (D3D8, DSOUND...).
#pragma once

#include <cstdint>

#include "hle_invoke.h"

void HleRegister(const char* lib, const char* name, void* fn, HleInvokeFn invoke);
// Functions that may safely do nothing: they return 0 and log once.
void HleRegisterIgnored(const char* lib, const char* name);

struct HleRegistrar {
    HleRegistrar(const char* lib, const char* name, void* fn, HleInvokeFn invoke) {
        HleRegister(lib, name, fn, invoke);
    }
    HleRegistrar(const char* lib, const char* name) { HleRegisterIgnored(lib, name); }
};

// Registers x_<name> as the implementation of lib!name.
#define HLE_EXPORT(lib, name) \
    static HleRegistrar hle_reg_##name(lib, #name, (void*)&x_##name, &HleInvoker<&x_##name>::Call)
#define HLE_IGNORE(lib, name) static HleRegistrar hle_ign_##name(lib, #name)

// Lifted code calling a title address that the host replaces (64-bit
// builds, or strict mode): runs the implementation as a call from `c`.
// Returns false if `va` is not a replaced function.
struct KtCpu;
bool HleCallFromLifted(KtCpu* c, uint32_t va);
// Does the host replace the title function at va?
bool HleReplaces(uint32_t va);

// A guest address that title code can call (through a vtable or callback
// pointer) to run host function `fn`: the function itself in the 32-bit
// build, a reserved guest address routed to `invoke` otherwise.
uint32_t HleGuestCallable(void* fn, HleInvokeFn invoke, HleConv conv);
