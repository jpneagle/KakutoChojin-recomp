// Host -> title calls (see CallGuest in guest.h).
#include "guest.h"
#include "host.h"

#if UINTPTR_MAX == 0xFFFFFFFFu
// 32-bit: title code is called natively (lifted functions are entered
// through their patched entries).
uint32_t CallGuest(uint32_t fn, std::initializer_list<uint32_t> args) {
    const uint32_t* a = args.begin();
    switch (args.size()) {
        case 0: return reinterpret_cast<uint32_t(__stdcall*)()>(fn)();
        case 1: return reinterpret_cast<uint32_t(__stdcall*)(uint32_t)>(fn)(a[0]);
        case 2: return reinterpret_cast<uint32_t(__stdcall*)(uint32_t, uint32_t)>(fn)(a[0], a[1]);
        case 3: return reinterpret_cast<uint32_t(__stdcall*)(uint32_t, uint32_t, uint32_t)>(fn)(a[0], a[1], a[2]);
        case 4:
            return reinterpret_cast<uint32_t(__stdcall*)(uint32_t, uint32_t, uint32_t, uint32_t)>(fn)(a[0], a[1], a[2],
                                                                                                    a[3]);
        default: Fatal("CallGuest: %zu arguments not supported", args.size());
    }
}
#endif
