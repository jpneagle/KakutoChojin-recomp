// Calling host implementations (kernel / HLE exports) from lifted code.
//
// In the 32-bit build title code calls x_* functions natively. Without native
// title code (64-bit builds), lifted code reaches them through
// HleInvoker<&x_Name>::Call, which reads the arguments from the guest stack
// (and ECX/EDX for __fastcall), converts them to the C++ parameter types,
// stores the result in EAX (EDX:EAX, or st(0) for floating point) and pops
// the return address and, for callee-cleanup conventions, the arguments.
//
// Parameter types decide the conversion: T* is a guest pointer (G2H),
// GPtr<T> / GHandle / integers are taken as they are, float takes one slot,
// double and 64-bit integers take two.
#pragma once

#include <cstring>
#include <tuple>
#include <type_traits>

#include "../lift/kt_lift.h"
#include "guest.h"
#include "host.h"

enum class HleConv { Stdcall, Cdecl, Fastcall };

namespace hle {

struct ArgReader {
    KtCpu* c;
    uint32_t sp;  // next stack slot
    int regs;     // fastcall register arguments left (ECX, EDX)

    uint32_t Slot() {
        if (regs == 2) return regs--, c->ecx;
        if (regs == 1) return regs--, c->edx;
        uint32_t v = rd32(sp);
        sp += 4;
        return v;
    }
    uint64_t Slot64() {
        regs = 0;  // 64-bit values always go on the stack
        uint64_t lo = rd32(sp), hi = rd32(sp + 4);
        sp += 8;
        return lo | hi << 32;
    }
};

template <typename T, typename = void>
struct Arg {
    static T Get(ArgReader& r) {
        static_assert(sizeof(T) <= 4 && (std::is_integral<T>::value || std::is_enum<T>::value),
                      "unsupported HLE parameter type");
        return static_cast<T>(r.Slot());
    }
};
template <typename T>
struct Arg<T*> {
    static T* Get(ArgReader& r) { return static_cast<T*>(G2H(r.Slot())); }
};
template <typename T>
struct Arg<GPtr<T>> {
    static GPtr<T> Get(ArgReader& r) { return GPtr<T>{r.Slot()}; }
};
template <>
struct Arg<float> {
    static float Get(ArgReader& r) {
        uint32_t v = r.Slot();
        float f;
        memcpy(&f, &v, 4);
        return f;
    }
};
template <>
struct Arg<double> {
    static double Get(ArgReader& r) {
        uint64_t v = r.Slot64();
        double d;
        memcpy(&d, &v, 8);
        return d;
    }
};
template <typename T>
struct Arg<T, typename std::enable_if<std::is_integral<T>::value && sizeof(T) == 8>::type> {
    static T Get(ArgReader& r) { return static_cast<T>(r.Slot64()); }
};
template <>
struct Arg<LARGE_INTEGER> {
    static LARGE_INTEGER Get(ArgReader& r) {
        LARGE_INTEGER v;
        v.QuadPart = LONGLONG(r.Slot64());
        return v;
    }
};

template <typename R, typename = void>
struct Ret {
    static void Set(KtCpu* c, R v) {
        static_assert(sizeof(R) <= 4, "unsupported HLE return type");
        c->eax = uint32_t(v);
    }
};
template <typename T>
struct Ret<T*> {
    static void Set(KtCpu* c, T* v) { c->eax = H2G(v); }
};
template <typename R>
struct Ret<R, typename std::enable_if<std::is_integral<R>::value && sizeof(R) == 8>::type> {
    static void Set(KtCpu* c, R v) {
        c->eax = uint32_t(uint64_t(v));
        c->edx = uint32_t(uint64_t(v) >> 32);
    }
};
template <>
struct Ret<float> {
    static void Set(KtCpu* c, float v) { kt_fpush(c, v); }
};
template <>
struct Ret<double> {
    static void Set(KtCpu* c, double v) { kt_fpush(c, v); }
};

// Calls fn with arguments read left to right (a braced list fixes the order).
template <typename R, typename... A>
struct Call {
    template <typename F>
    static void Do(F fn, KtCpu* c, ArgReader& r) {
        std::tuple<A...> args{Arg<A>::Get(r)...};
        if constexpr (std::is_void<R>::value) {
            std::apply(fn, args);
        } else {
            Ret<R>::Set(c, std::apply(fn, args));
        }
    }
};

template <typename F>
struct Signature;
template <typename R, typename... A>
struct Signature<R (*)(A...)> : Call<R, A...> {};
#if UINTPTR_MAX == 0xFFFFFFFFu  // calling conventions are distinct types only on x86
template <typename R, typename... A>
struct Signature<R(__stdcall*)(A...)> : Call<R, A...> {};
template <typename R, typename... A>
struct Signature<R(__fastcall*)(A...)> : Call<R, A...> {};
#endif

}  // namespace hle

using HleInvokeFn = void (*)(KtCpu* c, HleConv conv);

// Performs a call to fn as lifted code would: [esp] = return address.
template <auto Fn>
struct HleInvoker {
    static void Call(KtCpu* c, HleConv conv) {
        uint32_t ret = rd32(c->esp);
        hle::ArgReader r{c, c->esp + 4, conv == HleConv::Fastcall ? 2 : 0};
        hle::Signature<decltype(Fn)>::Do(Fn, c, r);
        c->eip = ret;
        c->esp = conv == HleConv::Cdecl ? c->esp + 4 : r.sp;
    }
};
