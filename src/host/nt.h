// Host ntdll entry points used by the 32-bit build (title memory is host
// memory there). Resolved at runtime so we don't depend on undocumented
// import-library contents.
#pragma once

#include "host.h"

namespace nt {

#define KT_NT_FUNCS(X)                                                                        \
    X(NTSTATUS, NtAllocateVirtualMemory, (HANDLE, PVOID*, ULONG_PTR, PSIZE_T, ULONG, ULONG)) \
    X(NTSTATUS, NtFreeVirtualMemory, (HANDLE, PVOID*, PSIZE_T, ULONG))

#define KT_DECLARE(ret, name, args) extern ret(NTAPI* name) args;
KT_NT_FUNCS(KT_DECLARE)
#undef KT_DECLARE

void Init();

}  // namespace nt
