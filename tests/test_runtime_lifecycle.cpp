// Production tests of the callback rundown (src/runtime_lifecycle.cpp, linked as is, no stubs): per-thread slots, a locked
// update on the thread's own line, and the Dekker handshake with `s_stopping`. They cover: Initialize refuses while a
// callback is admitted; WaitForCallbacks is false while any admitted callback (including one admitted after shutdown
// began, which only forwards) is inside and true when all left; threads entering and leaving while another thread runs
// BeginShutdown + WaitForCallbacks (a working guard is never inside when the wait returns true, and none is admitted
// afterwards); the overflow slot (more threads than the table) counting exactly; thread exit releasing slots (thousands of
// threads, no exhaustion); a slot reused while a guard of its former owner is still inside; the EnterCallback/LeaveCallback
// pair; a guard destroyed on another thread than it was constructed on. `--bench` prints the cost of a guard pair (not a
// test). What only a game shows: the time the hooks spend in the guard.

#include "runtime_lifecycle.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #value); std::fflush(stderr); std::abort(); } } while (false)

namespace {

void SleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// A thread that stays inside a guard (or an Enter/Leave pair) until released.
struct Holder {
    std::atomic<bool> inside{ false };
    std::atomic<bool> release{ false };
    std::atomic<bool> was_stopping{ false };
    std::thread thread;
};

void StartGuardHolder(Holder& holder, int nested = 0)
{
    holder.thread = std::thread([&holder, nested] {
        RuntimeCallbackGuard guard;
        holder.was_stopping.store(guard.IsStopping());
        // Nested admissions and leaves on top of the held one (the slot's count must stay exact).
        for (int i = 0; i < nested; ++i) { RuntimeCallbackGuard inner; }
        holder.inside.store(true);
        while (!holder.release.load()) SleepMs(1);
    });
}

void WaitInside(Holder& holder)
{
    while (!holder.inside.load()) SleepMs(1);
}

void Release(Holder& holder)
{
    holder.release.store(true);
    holder.thread.join();
}

// ── Initialize / WaitForCallbacks with deterministic holders ──
void TestInitializeAndWait()
{
    CHECK(RuntimeLifecycle_Initialize());
    CHECK(!RuntimeLifecycle_IsStopping());
    CHECK(RuntimeLifecycle_GetStopEvent() != nullptr);
    CHECK(RuntimeLifecycle_WaitForCallbacks(0)); // nothing admitted

    {
        RuntimeCallbackGuard guard;
        CHECK(!guard.IsStopping());
        CHECK(!RuntimeLifecycle_Initialize()); // a callback is admitted (this thread's slot)
        CHECK(!RuntimeLifecycle_WaitForCallbacks(20));
    }
    CHECK(RuntimeLifecycle_WaitForCallbacks(0));

    // A guard of another thread, admitted before shutdown began: shutdown does not drain while it is inside.
    Holder working;
    StartGuardHolder(working, 50);
    WaitInside(working);
    CHECK(!working.was_stopping.load());
    CHECK(!RuntimeLifecycle_Initialize());
    RuntimeLifecycle_BeginShutdown();
    CHECK(RuntimeLifecycle_IsStopping());
    CHECK(!RuntimeLifecycle_WaitForCallbacks(30));
    CHECK(!RuntimeLifecycle_WaitForCallbacks(30)); // and again: the examination does not consume anything
    CHECK(!RuntimeLifecycle_Initialize());

    // A guard admitted after shutdown began only forwards and still counts until it leaves.
    Holder forwarder;
    StartGuardHolder(forwarder, 7);
    WaitInside(forwarder);
    CHECK(forwarder.was_stopping.load());
    Release(working);
    CHECK(!RuntimeLifecycle_WaitForCallbacks(30)); // the forwarder is still inside
    CHECK(!RuntimeLifecycle_Initialize());
    Release(forwarder);
    CHECK(RuntimeLifecycle_WaitForCallbacks(5000));
    {
        RuntimeCallbackGuard guard; // this thread, after shutdown began
        CHECK(guard.IsStopping());
        CHECK(!RuntimeLifecycle_WaitForCallbacks(20));
    }
    CHECK(RuntimeLifecycle_WaitForCallbacks(0));
    CHECK(RuntimeLifecycle_Initialize());
    CHECK(!RuntimeLifecycle_IsStopping());
}

// The EnterCallback/LeaveCallback pair (the Streamline detours' bracket) counts like a guard.
void TestEnterLeavePair()
{
    CHECK(RuntimeLifecycle_Initialize());
    std::atomic<bool> in{ false }, go{ false };
    std::thread t([&] {
        const bool stopping = RuntimeLifecycle_EnterCallback();
        CHECK(!stopping);
        in.store(true);
        while (!go.load()) SleepMs(1);
        RuntimeLifecycle_LeaveCallback();
    });
    while (!in.load()) SleepMs(1);
    CHECK(!RuntimeLifecycle_Initialize());
    RuntimeLifecycle_BeginShutdown();
    CHECK(!RuntimeLifecycle_WaitForCallbacks(20));
    go.store(true);
    t.join();
    CHECK(RuntimeLifecycle_WaitForCallbacks(5000));
    CHECK(RuntimeLifecycle_Initialize());
}

// ── Threads entering and leaving while another one runs the shutdown pass ──
// A working guard (admitted before shutdown began) bumps `in_body` while it is inside. When WaitForCallbacks returns true
// `in_body` is 0 and, from then on, no guard that sees `!IsStopping()` is admitted (`violations`).
void TestShutdownAgainstHammering()
{
    const unsigned workers = std::max(4u, std::min(16u, std::thread::hardware_concurrency()));
    constexpr int kRounds = 250;
    for (int round = 0; round < kRounds; ++round) {
        CHECK(RuntimeLifecycle_Initialize());
        std::atomic<int> in_body{ 0 };
        std::atomic<bool> drained{ false };
        std::atomic<int> violations{ 0 };
        std::atomic<unsigned> started{ 0 };
        std::atomic<uint64_t> working_guards{ 0 };
        std::vector<std::thread> threads;
        for (unsigned i = 0; i < workers; ++i) {
            threads.emplace_back([&] {
                started.fetch_add(1);
                for (;;) {
                    RuntimeCallbackGuard guard;
                    if (guard.IsStopping()) {
                        // A forwarder: admitted after shutdown began. Leave the loop (it may be inside during the pass).
                        break;
                    }
                    in_body.fetch_add(1);
                    if (drained.load()) violations.fetch_add(1);
                    working_guards.fetch_add(1, std::memory_order_relaxed);
                    // A few nested admissions: slot counts above one.
                    { RuntimeCallbackGuard inner; if (inner.IsStopping()) { /* also fine */ } }
                    in_body.fetch_sub(1);
                }
            });
        }
        while (started.load() < workers) std::this_thread::yield();
        // 0 .. ~300 microseconds of hammering before the shutdown.
        const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds((round * 37) % 300);
        while (std::chrono::steady_clock::now() < until) std::this_thread::yield();

        RuntimeLifecycle_BeginShutdown();
        CHECK(RuntimeLifecycle_WaitForCallbacks(20000));
        CHECK(in_body.load() == 0);
        drained.store(true);
        for (auto& t : threads) t.join();
        CHECK(violations.load() == 0);
        CHECK(in_body.load() == 0);
        CHECK(RuntimeLifecycle_WaitForCallbacks(0));
    }
    CHECK(RuntimeLifecycle_Initialize());
}

// Forwarders keep entering and leaving while the rundown passes run, with working guards held by blocked threads: Wait is
// false for as long as one of them is inside, however many passes run; true once they left and the hammering stopped.
void TestHeldGuardsAgainstForwarders()
{
    CHECK(RuntimeLifecycle_Initialize());
    constexpr int kHolders = 6;
    Holder holders[kHolders];
    for (auto& h : holders) StartGuardHolder(h, 100);
    for (auto& h : holders) WaitInside(h);

    std::atomic<bool> stop{ false };
    std::atomic<uint64_t> forwarded{ 0 };
    std::vector<std::thread> hammer;
    for (int i = 0; i < 6; ++i) {
        hammer.emplace_back([&] {
            while (!stop.load()) {
                RuntimeCallbackGuard guard;
                if (guard.IsStopping()) forwarded.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    RuntimeLifecycle_BeginShutdown();
    for (int i = 0; i < kHolders; ++i) {
        // Passes while a working guard is inside and forwarders are entering and leaving: never "drained".
        for (int pass = 0; pass < 200; ++pass) {
            CHECK(!RuntimeLifecycle_WaitForCallbacks(0));
            CHECK(!RuntimeLifecycle_Initialize());
        }
        Release(holders[i]);
    }
    stop.store(true);
    for (auto& t : hammer) t.join();
    CHECK(forwarded.load() > 0);
    CHECK(RuntimeLifecycle_WaitForCallbacks(5000));
    CHECK(RuntimeLifecycle_Initialize());
}

// ── Overflow: more threads than slots, each inside a guard ──
void TestOverflow()
{
    CHECK(RuntimeLifecycle_Initialize());
    const RuntimeLifecycleSlotStats before = RuntimeLifecycle_SlotStats();
    const unsigned total = before.capacity + 64;
    std::vector<std::unique_ptr<Holder>> holders;
    for (unsigned i = 0; i < total; ++i) {
        holders.push_back(std::make_unique<Holder>());
        StartGuardHolder(*holders.back(), 20);
        WaitInside(*holders.back()); // one at a time: the first threads take the table, the rest the overflow slot
    }
    const RuntimeLifecycleSlotStats full = RuntimeLifecycle_SlotStats();
    CHECK(full.owned_now == full.capacity); // the table is full: later threads are on the overflow slot
    RuntimeLifecycle_BeginShutdown();
    // Release all but one, in an order that mixes table slots and overflow users; the count stays exact: not drained
    // until the last one leaves.
    std::vector<unsigned> order;
    for (unsigned i = 0; i < total; ++i) order.push_back(i);
    for (unsigned i = 0; i < total; ++i) std::swap(order[i], order[(i * 31u + 7u) % total]);
    for (unsigned n = 0; n + 1 < total; ++n) {
        Release(*holders[order[n]]);
        if (n % 16 == 0) CHECK(!RuntimeLifecycle_WaitForCallbacks(0));
    }
    CHECK(!RuntimeLifecycle_WaitForCallbacks(30));
    Release(*holders[order[total - 1]]);
    CHECK(RuntimeLifecycle_WaitForCallbacks(5000));
    CHECK(RuntimeLifecycle_Initialize());
    // The table's slots came back.
    CHECK(RuntimeLifecycle_SlotStats().owned_now == before.owned_now);

    // Many threads on the overflow slot at once, entering and leaving (locked updates): exact after they are done.
    std::vector<std::thread> threads;
    std::atomic<bool> go{ false };
    for (unsigned i = 0; i < total; ++i) {
        threads.emplace_back([&] {
            while (!go.load()) std::this_thread::yield();
            for (int k = 0; k < 2000; ++k) { RuntimeCallbackGuard g; }
        });
    }
    go.store(true);
    for (auto& t : threads) t.join();
    RuntimeLifecycle_BeginShutdown();
    CHECK(RuntimeLifecycle_WaitForCallbacks(5000));
    CHECK(RuntimeLifecycle_Initialize());
}

// ── Thread exit releases the slot; slots are reused ──
void TestSlotReuse()
{
    CHECK(RuntimeLifecycle_Initialize());
    { RuntimeCallbackGuard g; } // this thread owns a slot from now on
    const RuntimeLifecycleSlotStats before = RuntimeLifecycle_SlotStats();

    // Thousands of threads, one at a time and in batches: far more than the table, never exhausted.
    for (int i = 0; i < 3000; ++i) {
        std::thread([] { RuntimeCallbackGuard g; CHECK(!g.IsStopping()); }).join();
    }
    CHECK(RuntimeLifecycle_SlotStats().owned_now == before.owned_now);
    CHECK(RuntimeLifecycle_SlotStats().high_water <= before.high_water + 2);
    for (int batch = 0; batch < 200; ++batch) {
        std::vector<std::thread> threads;
        for (int i = 0; i < 24; ++i) threads.emplace_back([] { for (int k = 0; k < 10; ++k) { RuntimeCallbackGuard g; } });
        for (auto& t : threads) t.join();
    }
    const RuntimeLifecycleSlotStats after = RuntimeLifecycle_SlotStats();
    CHECK(after.owned_now == before.owned_now);
    CHECK(after.high_water <= before.high_water + 2 * 24); // the batches' own concurrent slots, not thousands
    // Every guard left: the rundown is clean, slots that were reused carried a zero count.
    RuntimeLifecycle_BeginShutdown();
    CHECK(RuntimeLifecycle_WaitForCallbacks(0));
    CHECK(RuntimeLifecycle_Initialize());
}

// Shutdown while a table slot is released and reclaimed repeatedly (a pass reads the slot words while owners change).
void TestReleaseAgainstPass()
{
    CHECK(RuntimeLifecycle_Initialize());
    std::atomic<bool> stop{ false };
    std::thread churn([&] {
        while (!stop.load()) {
            std::thread([] { for (int k = 0; k < 20; ++k) { RuntimeCallbackGuard g; } }).join();
        }
    });
    for (int i = 0; i < 2000; ++i) (void)RuntimeLifecycle_WaitForCallbacks(0); // no crash, no hang; the result varies
    stop.store(true);
    churn.join();
    RuntimeLifecycle_BeginShutdown();
    CHECK(RuntimeLifecycle_WaitForCallbacks(5000));
    CHECK(RuntimeLifecycle_Initialize());
}

// ── A guard destroyed on another thread than it was constructed on keeps the count exact, also when its slot was reused ──
void TestGuardLeftOnAnotherThread()
{
    CHECK(RuntimeLifecycle_Initialize());
    std::atomic<RuntimeCallbackGuard*> guard{ nullptr };
    std::thread a([&] { guard.store(new RuntimeCallbackGuard); });
    a.join(); // the thread is gone: its slot was released with the guard still counted in it
    CHECK(!RuntimeLifecycle_WaitForCallbacks(20));
    // The slot goes to another thread, which counts on top of it.
    std::thread c([] { for (int k = 0; k < 1000; ++k) { RuntimeCallbackGuard g; } });
    c.join();
    CHECK(!RuntimeLifecycle_WaitForCallbacks(20));
    RuntimeLifecycle_BeginShutdown();
    CHECK(!RuntimeLifecycle_WaitForCallbacks(20));
    std::thread b([&] { delete guard.load(); });
    b.join();
    CHECK(RuntimeLifecycle_WaitForCallbacks(5000));
    CHECK(RuntimeLifecycle_Initialize());
}

// ── Benchmark (--bench): ns per guard pair, single thread and four threads ──
template <class F>
double NsPerPair(unsigned threads, F&& body)
{
    constexpr int kIterations = 4000000;
    std::atomic<unsigned> ready{ 0 };
    std::atomic<bool> go{ false };
    std::vector<std::thread> pool;
    std::vector<double> times(threads);
    for (unsigned t = 0; t < threads; ++t) {
        pool.emplace_back([&, t] {
            ready.fetch_add(1);
            while (!go.load()) std::this_thread::yield();
            const auto begin = std::chrono::steady_clock::now();
            body(t, kIterations);
            const auto end = std::chrono::steady_clock::now();
            times[t] = std::chrono::duration<double, std::nano>(end - begin).count() / kIterations;
        });
    }
    while (ready.load() < threads) std::this_thread::yield();
    go.store(true);
    for (auto& th : pool) th.join();
    double sum = 0;
    for (double v : times) sum += v;
    return sum / threads;
}

struct alignas(64) PaddedCounter { std::atomic<uint32_t> value{ 0 }; };

void Bench()
{
    CHECK(RuntimeLifecycle_Initialize());
    static std::atomic<bool> stopping{ false };
    static std::atomic<uint32_t> shared{ 0 };
    static PaddedCounter per_thread[16];
    static volatile uint32_t sink = 0;
    for (unsigned threads : { 1u, 4u }) {
        const double production = NsPerPair(threads, [](unsigned, int n) {
            uint32_t s = 0;
            for (int i = 0; i < n; ++i) { RuntimeCallbackGuard g; s += g.IsStopping(); }
            sink = s;
        });
        const double shared_locked = NsPerPair(threads, [](unsigned, int n) {
            uint32_t s = 0;
            for (int i = 0; i < n; ++i) {
                shared.fetch_add(1, std::memory_order_acq_rel);
                s += stopping.load(std::memory_order_acquire);
                shared.fetch_sub(1, std::memory_order_release);
            }
            sink = s;
        });
        const double slot_locked = NsPerPair(threads, [](unsigned t, int n) {
            uint32_t s = 0;
            std::atomic<uint32_t>& c = per_thread[t].value;
            for (int i = 0; i < n; ++i) {
                c.fetch_add(1, std::memory_order_acq_rel);
                s += stopping.load(std::memory_order_acquire);
                c.fetch_sub(1, std::memory_order_release);
            }
            sink = s;
        });
        std::printf("threads=%u  production guard (out of line) %.2f ns/pair | inline models: shared locked counter %.2f, per-thread locked slot %.2f\n",
            threads, production, shared_locked, slot_locked);
    }
}

} // namespace

int main(int argc, char** argv)
{
    // A hung rundown must fail the test, not stall the suite.
    std::thread([] { SleepMs(240000); std::fprintf(stderr, "FAIL: timeout\n"); std::fflush(stderr); std::_Exit(3); }).detach();

    if (argc > 1 && std::strcmp(argv[1], "--bench") == 0) { Bench(); return 0; }

    TestInitializeAndWait();
    TestEnterLeavePair();
    TestSlotReuse(); // before the tests that fill the table: the high-water mark is still small
    TestHeldGuardsAgainstForwarders();
    TestShutdownAgainstHammering();
    TestOverflow();
    TestReleaseAgainstPass();
    TestGuardLeftOnAnotherThread();
    std::printf("runtime_lifecycle: ok\n");
    return 0;
}
