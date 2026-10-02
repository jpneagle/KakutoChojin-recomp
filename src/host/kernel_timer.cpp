// KTIMER / KDPC exports. Expired timers and queued DPCs are run by a single
// scheduler thread that plays the role of the Xbox's DISPATCH_LEVEL.
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

#include "kernel.h"
#include "ob.h"
#include "os.h"

namespace {

// Xbox object layouts (x86; pointers are guest addresses).
struct XLIST_ENTRY {
    uint32_t Flink, Blink;
};

struct XKDPC {
    SHORT Type;  // DpcObject = 0x13
    UCHAR Inserted, Padding;
    XLIST_ENTRY DpcListEntry;
    uint32_t DeferredRoutine;  // void NTAPI (KDPC*, context, arg1, arg2)
    uint32_t DeferredContext, SystemArgument1, SystemArgument2;
};
static_assert(offsetof(XKDPC, DeferredRoutine) == 0x0C, "KDPC layout");

struct XKTIMER {
    UCHAR Type, Absolute, Size, Inserted;
    LONG SignalState;
    XLIST_ENTRY WaitListHead;
    ULARGE_INTEGER DueTime;
    XLIST_ENTRY TimerListEntry;
    GPtr<XKDPC> Dpc;
    LONG Period;
};
static_assert(sizeof(XKTIMER) == 0x28, "KTIMER layout");

using Clock = std::chrono::steady_clock;

std::mutex g_mu;
std::condition_variable g_cv;
std::multimap<Clock::time_point, XKTIMER*> g_timers;
std::deque<XKDPC*> g_dpcs;
bool g_started = false;

void RemoveTimerLocked(XKTIMER* t) {
    for (auto it = g_timers.begin(); it != g_timers.end(); ++it)
        if (it->second == t) {
            g_timers.erase(it);
            break;
        }
    t->Inserted = FALSE;
}

void QueueDpcLocked(XKDPC* d, uint32_t a1, uint32_t a2) {
    d->SystemArgument1 = a1;
    d->SystemArgument2 = a2;
    d->Inserted = TRUE;
    g_dpcs.push_back(d);
    g_cv.notify_all();
}

void Scheduler() {
    XThread* x = XThreadAdoptCurrent();
    x->kpcr[kpcr_off::kIrql] = 2;  // DISPATCH_LEVEL

    std::unique_lock<std::mutex> lk(g_mu);
    for (;;) {
        auto now = Clock::now();
        while (!g_timers.empty() && g_timers.begin()->first <= now) {
            XKTIMER* t = g_timers.begin()->second;
            g_timers.erase(g_timers.begin());
            ob::SetGuestSignal(reinterpret_cast<uint8_t*>(t), 1);  // wakes KeWaitForSingleObject
            t->Inserted = FALSE;
            if (t->Period > 0) {
                g_timers.emplace(now + std::chrono::milliseconds(t->Period), t);
                t->Inserted = TRUE;
            }
            if (t->Dpc && !t->Dpc->Inserted) QueueDpcLocked(t->Dpc.get(), 0, 0);
        }
        while (!g_dpcs.empty()) {
            XKDPC* d = g_dpcs.front();
            g_dpcs.pop_front();
            d->Inserted = FALSE;
            lk.unlock();
            CallGuest(d->DeferredRoutine, {H2G(d), d->DeferredContext, d->SystemArgument1, d->SystemArgument2});
            lk.lock();
        }
        if (g_timers.empty())
            g_cv.wait(lk);
        else
            g_cv.wait_until(lk, g_timers.begin()->first);
    }
}

void EnsureStartedLocked() {
    if (!g_started) {
        g_started = true;
        std::thread(Scheduler).detach();
    }
}

// DueTime < 0: relative, in 100 ns units. DueTime > 0: absolute system time.
Clock::time_point DueToTimePoint(LARGE_INTEGER due) {
    LONGLONG rel100ns = due.QuadPart;
    if (rel100ns > 0) rel100ns = -(due.QuadPart - LONGLONG(os::SystemTime100ns()));
    return Clock::now() + std::chrono::microseconds(-rel100ns / 10);
}

VOID NTAPI x_KeInitializeDpc(XKDPC* d, uint32_t routine, uint32_t ctx) {
    memset(d, 0, sizeof *d);
    d->Type = 0x13;
    d->DeferredRoutine = routine;
    d->DeferredContext = ctx;
}

VOID NTAPI x_KeInitializeTimerEx(XKTIMER* t, ULONG type) {
    memset(t, 0, sizeof *t);
    t->Type = UCHAR(8 + type);  // TimerNotificationObject / TimerSynchronizationObject
    t->Size = sizeof *t / 4;
    t->WaitListHead.Flink = t->WaitListHead.Blink = H2G(&t->WaitListHead);
}

BOOLEAN NTAPI x_KeSetTimerEx(XKTIMER* t, LARGE_INTEGER due, LONG period, XKDPC* dpc) {
    std::lock_guard<std::mutex> lk(g_mu);
    EnsureStartedLocked();
    BOOLEAN was = t->Inserted;
    if (was) RemoveTimerLocked(t);
    t->Dpc = dpc;
    t->Period = period;
    t->SignalState = 0;
    t->DueTime.QuadPart = ULONGLONG(due.QuadPart);
    t->Inserted = TRUE;
    g_timers.emplace(DueToTimePoint(due), t);
    g_cv.notify_all();
    return was;
}

BOOLEAN NTAPI x_KeSetTimer(XKTIMER* t, LARGE_INTEGER due, XKDPC* dpc) { return x_KeSetTimerEx(t, due, 0, dpc); }

BOOLEAN NTAPI x_KeCancelTimer(XKTIMER* t) {
    std::lock_guard<std::mutex> lk(g_mu);
    BOOLEAN was = t->Inserted;
    if (was) RemoveTimerLocked(t);
    return was;
}

BOOLEAN NTAPI x_KeInsertQueueDpc(XKDPC* d, uint32_t a1, uint32_t a2) {
    std::lock_guard<std::mutex> lk(g_mu);
    EnsureStartedLocked();
    if (d->Inserted) return FALSE;
    QueueDpcLocked(d, a1, a2);
    return TRUE;
}

BOOLEAN NTAPI x_KeRemoveQueueDpc(XKDPC* d) {
    std::lock_guard<std::mutex> lk(g_mu);
    for (auto it = g_dpcs.begin(); it != g_dpcs.end(); ++it)
        if (*it == d) {
            g_dpcs.erase(it);
            d->Inserted = FALSE;
            return TRUE;
        }
    return FALSE;
}

}  // namespace

std::vector<KExport> KernelTimerExports() {
    return {KX(KeInitializeDpc), KX(KeInitializeTimerEx), KX(KeSetTimer), KX(KeSetTimerEx), KX(KeCancelTimer),
            KX(KeInsertQueueDpc), KX(KeRemoveQueueDpc)};
}
