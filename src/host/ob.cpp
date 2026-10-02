// Xbox kernel objects: handle table, dispatcher objects, waits and threads.
#include "ob.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <unordered_map>

#include "os.h"

XThread* XThreadCurrent();

namespace ob {

namespace {

using Clock = std::chrono::steady_clock;

std::mutex g_table_mu;
std::unordered_map<GHandle, std::shared_ptr<Object>> g_table;
GHandle g_next = 0x1000;

// The dispatcher lock: guards all Waitable state and APC queues.
std::mutex g_mu;
std::condition_variable g_cv;

thread_local std::shared_ptr<Thread> t_self;

// NT timeout -> deadline (null for infinite).
bool Deadline(const int64_t* timeout, Clock::time_point* out) {
    if (!timeout) return false;
    int64_t t = *timeout;
    int64_t rel100ns = t <= 0 ? -t : t - int64_t(os::SystemTime100ns());
    if (rel100ns < 0) rel100ns = 0;
    *out = Clock::now() + std::chrono::microseconds(rel100ns / 10);
    return true;
}

// Takes the calling thread's APCs (g_mu held) to run them unlocked.
std::deque<std::function<void()>> TakeApcs(Thread* self) {
    std::deque<std::function<void()>> out;
    out.swap(self->apcs);
    return out;
}

}  // namespace

GHandle Insert(std::shared_ptr<Object> o) {
    std::lock_guard<std::mutex> lk(g_table_mu);
    GHandle h = g_next;
    g_next += 4;
    g_table[h] = std::move(o);
    return h;
}

std::shared_ptr<Object> Get(GHandle h) {
    if (h == kCurrentThread) return CurrentThread();
    std::lock_guard<std::mutex> lk(g_table_mu);
    auto it = g_table.find(h);
    return it == g_table.end() ? nullptr : it->second;
}

bool Close(GHandle h) {
    std::lock_guard<std::mutex> lk(g_table_mu);
    return g_table.erase(h) != 0;
}

NTSTATUS Wait(const std::vector<std::shared_ptr<Waitable>>& objects, bool wait_all, bool alertable,
              const int64_t* timeout) {
    std::shared_ptr<Thread> self = CurrentThread();
    XThread* me = XThreadCurrent();
    Clock::time_point deadline;
    bool timed = Deadline(timeout, &deadline);
    std::unique_lock<std::mutex> lk(g_mu);
    for (;;) {
        if (alertable && !self->apcs.empty()) {
            auto apcs = TakeApcs(self.get());
            lk.unlock();
            for (auto& fn : apcs) fn();
            return kUserApc;
        }
        if (wait_all) {
            bool all = !objects.empty() &&
                       std::all_of(objects.begin(), objects.end(), [&](const auto& o) { return o->Signaled(me); });
            if (all) {
                for (const auto& o : objects) o->Acquire(me);
                g_cv.notify_all();
                return kSuccess;
            }
        } else {
            for (size_t i = 0; i < objects.size(); i++)
                if (objects[i]->Signaled(me)) {
                    objects[i]->Acquire(me);
                    g_cv.notify_all();
                    return kSuccess + NTSTATUS(i);
                }
        }
        if (timed) {
            if (Clock::now() >= deadline) return kTimeout;
            g_cv.wait_until(lk, deadline);
        } else {
            g_cv.wait(lk);
        }
    }
}

NTSTATUS Delay(bool alertable, const int64_t* interval) {
    Clock::time_point deadline;
    bool timed = Deadline(interval, &deadline);
    if (!alertable) {
        if (interval && *interval == 0)
            std::this_thread::yield();
        else if (timed)
            std::this_thread::sleep_until(deadline);
        else
            for (;;) std::this_thread::sleep_for(std::chrono::hours(1));
        return kSuccess;
    }
    std::shared_ptr<Thread> self = CurrentThread();
    std::unique_lock<std::mutex> lk(g_mu);
    for (;;) {
        if (!self->apcs.empty()) {
            auto apcs = TakeApcs(self.get());
            lk.unlock();
            for (auto& fn : apcs) fn();
            return kUserApc;
        }
        if (!timed) {
            g_cv.wait(lk);
            continue;
        }
        if (Clock::now() >= deadline) return kSuccess;
        g_cv.wait_until(lk, deadline);
    }
}

int32_t SetEvent(Event* e) {
    std::lock_guard<std::mutex> lk(g_mu);
    int32_t prev = e->state;
    e->state = true;
    g_cv.notify_all();
    return prev;
}

int32_t ResetEvent(Event* e) {
    std::lock_guard<std::mutex> lk(g_mu);
    int32_t prev = e->state;
    e->state = false;
    return prev;
}

// Releases the current waiters and leaves the event reset. Waiters re-check
// under the lock, so a pulse is only seen by threads that are already waiting
// when they wake before the reset; approximated by a short signalled window.
int32_t PulseEvent(Event* e) {
    int32_t prev = SetEvent(e);
    std::this_thread::yield();
    ResetEvent(e);
    return prev;
}

NTSTATUS ReleaseSemaphore(Semaphore* s, int32_t count, int32_t* previous) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (previous) *previous = s->count;
    if (count <= 0 || s->count + count > s->limit) return kSemaphoreLimitExceeded;
    s->count += count;
    g_cv.notify_all();
    return kSuccess;
}

NTSTATUS ReleaseMutant(Mutant* m, int32_t* previous) {
    XThread* me = XThreadCurrent();
    std::lock_guard<std::mutex> lk(g_mu);
    if (previous) *previous = m->owner ? 1 - m->depth : 1;
    if (m->owner != me) return kMutantNotOwned;
    if (--m->depth == 0) m->owner = nullptr;
    g_cv.notify_all();
    return kSuccess;
}

int32_t SetGuestSignal(uint8_t* header, int32_t value) {
    std::lock_guard<std::mutex> lk(g_mu);
    int32_t* s = reinterpret_cast<int32_t*>(header + 4);
    int32_t prev = *s;
    *s = value;
    g_cv.notify_all();
    return prev;
}

bool SetEventHandle(GHandle h) {
    std::shared_ptr<Event> e = GetAs<Event>(h, Type::Event);
    if (!e) return false;
    SetEvent(e.get());
    return true;
}

std::shared_ptr<Thread> CurrentThread() {
    if (!t_self) {
        t_self = std::make_shared<Thread>();
        t_self->x = XThreadCurrent();
        t_self->native = os::CurrentThreadNative();
        t_self->started = true;
    }
    return t_self;
}

std::shared_ptr<Thread> CreateThread(XThread* x, std::function<void()> body, bool suspended, size_t stack_reserve) {
    auto t = std::make_shared<Thread>();
    t->x = x;
    t->started = !suspended;
    bool ok = os::StartThread(
        [t, body] {
            t_self = t;
            t->native = os::CurrentThreadNative();
            {
                std::unique_lock<std::mutex> lk(g_mu);
                g_cv.wait(lk, [&] { return t->started; });
            }
            body();
            std::lock_guard<std::mutex> lk(g_mu);
            t->exited = true;
            g_cv.notify_all();
        },
        stack_reserve);
    return ok ? t : nullptr;
}

void Resume(Thread* t) {
    std::lock_guard<std::mutex> lk(g_mu);
    t->started = true;
    g_cv.notify_all();
}

void ExitCurrentThread(int32_t status) {
    std::shared_ptr<Thread> self = CurrentThread();
    {
        std::lock_guard<std::mutex> lk(g_mu);
        self->exit_status = status;
        self->exited = true;
        g_cv.notify_all();
    }
    os::ExitThread();
}

void QueueApc(Thread* t, std::function<void()> fn) {
    std::lock_guard<std::mutex> lk(g_mu);
    t->apcs.push_back(std::move(fn));
    g_cv.notify_all();
}

}  // namespace ob
