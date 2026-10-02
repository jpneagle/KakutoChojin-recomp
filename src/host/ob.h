// Xbox kernel objects and handles, implemented on the C++ standard library.
//
// Titles see 32-bit handles from one table. Dispatcher objects (events,
// semaphores, mutants, threads) share a single lock and condition variable:
// every state change wakes all waiters, which re-check their conditions.
// That is simple and fast enough for the handful of threads a title runs.
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "guest.h"

struct XThread;

namespace ob {

using NTSTATUS = int32_t;
constexpr NTSTATUS kSuccess = 0, kTimeout = 0x102, kUserApc = 0xC0, kAbandoned = 0x80;
constexpr NTSTATUS kInvalidHandle = int32_t(0xC0000008), kObjectTypeMismatch = int32_t(0xC0000024);
constexpr NTSTATUS kSemaphoreLimitExceeded = int32_t(0xC0000047), kMutantNotOwned = int32_t(0xC0000046);

// Pseudo handles (never in the table).
constexpr GHandle kCurrentProcess = 0xFFFFFFFF, kCurrentThread = 0xFFFFFFFE, kDosDevices = 0xFFFFFFFD;

enum class Type { Event, Semaphore, Mutant, Thread, File, SymbolicLink, GuestDispatcher };

struct Object {
    explicit Object(Type t) : type(t) {}
    virtual ~Object() = default;
    const Type type;
};

// ---- Dispatcher objects (state guarded by the dispatcher lock) ----

struct Waitable : Object {
    using Object::Object;
    virtual bool Signaled(XThread* waiter) const = 0;
    virtual void Acquire(XThread* waiter) = 0;  // a satisfied wait consumes the signal
};

struct Event : Waitable {
    Event(bool manual_reset, bool initial) : Waitable(Type::Event), manual(manual_reset), state(initial) {}
    bool manual, state;
    bool Signaled(XThread*) const override { return state; }
    void Acquire(XThread*) override {
        if (!manual) state = false;
    }
};

struct Semaphore : Waitable {
    Semaphore(int32_t initial, int32_t maximum) : Waitable(Type::Semaphore), count(initial), limit(maximum) {}
    int32_t count, limit;
    bool Signaled(XThread*) const override { return count > 0; }
    void Acquire(XThread*) override { count--; }
};

struct Mutant : Waitable {
    explicit Mutant(XThread* initial_owner) : Waitable(Type::Mutant), owner(initial_owner), depth(initial_owner ? 1 : 0) {}
    XThread* owner;
    int32_t depth;
    bool Signaled(XThread* w) const override { return !owner || owner == w; }
    void Acquire(XThread* w) override { owner = w, depth++; }
};

struct Thread : Waitable {
    Thread() : Waitable(Type::Thread) {}
    XThread* x = nullptr;
    uintptr_t native = 0;  // os::CurrentThreadNative()
    bool exited = false, started = false;
    int32_t exit_status = 0;
    std::deque<std::function<void()>> apcs;  // user APCs (run in alertable waits)
    bool Signaled(XThread*) const override { return exited; }
    void Acquire(XThread*) override {}
};

// A kernel dispatcher object that lives in title memory (KEVENT, KSEMAPHORE,
// KTIMER: a DISPATCHER_HEADER the title passes to Ke* functions directly).
// Its SignalState in guest memory is the state, guarded by the dispatcher lock.
struct GuestDispatcher : Waitable {
    explicit GuestDispatcher(uint8_t* header) : Waitable(Type::GuestDispatcher), h(header) {}
    uint8_t* h;  // DISPATCHER_HEADER: Type, Absolute, Size, Inserted, LONG SignalState, ...
    int32_t& signal() const { return *reinterpret_cast<int32_t*>(h + 4); }
    bool Signaled(XThread*) const override { return signal() > 0; }
    void Acquire(XThread*) override {
        switch (h[0]) {
            case 1: case 9: signal() = 0; break;  // SynchronizationEvent / SynchronizationTimer
            case 5: signal()--; break;            // Semaphore
            default: break;                       // notification objects stay signalled
        }
    }
};

// Sets SignalState of a guest dispatcher object (KeSetEvent / timers); returns the previous value.
int32_t SetGuestSignal(uint8_t* header, int32_t value);

// ---- Handle table ----

GHandle Insert(std::shared_ptr<Object> o);
std::shared_ptr<Object> Get(GHandle h);  // resolves kCurrentThread too
bool Close(GHandle h);

template <typename T>
std::shared_ptr<T> GetAs(GHandle h, Type t) {
    std::shared_ptr<Object> o = Get(h);
    return o && o->type == t ? std::static_pointer_cast<T>(o) : nullptr;
}

// ---- Dispatcher operations ----

// NT timeouts: null = infinite, negative = relative, positive = absolute
// system time (both in 100 ns units).
NTSTATUS Wait(const std::vector<std::shared_ptr<Waitable>>& objects, bool wait_all, bool alertable,
              const int64_t* timeout);
NTSTATUS Delay(bool alertable, const int64_t* interval);

int32_t SetEvent(Event* e);   // returns the previous state
int32_t ResetEvent(Event* e);
int32_t PulseEvent(Event* e);
NTSTATUS ReleaseSemaphore(Semaphore* s, int32_t count, int32_t* previous);
NTSTATUS ReleaseMutant(Mutant* m, int32_t* previous);

// Sets an event the title gave us by handle (completion events). Returns false
// if the handle is not an event.
bool SetEventHandle(GHandle h);

// ---- Threads ----

// The ob::Thread of the calling thread (created on first use for host threads).
std::shared_ptr<Thread> CurrentThread();
// Runs `body` on a new host thread. With `suspended`, it starts at Resume().
std::shared_ptr<Thread> CreateThread(XThread* x, std::function<void()> body, bool suspended, size_t stack_reserve);
void Resume(Thread* t);
// Marks the calling thread exited (waking joiners) and ends it.
[[noreturn]] void ExitCurrentThread(int32_t status);
void QueueApc(Thread* t, std::function<void()> fn);

}  // namespace ob
