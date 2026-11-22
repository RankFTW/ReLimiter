#include "marker_log.h"
#include "scheduler.h"

#include <Windows.h>

#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

namespace {
int g_test_count = 0;
int g_fail_count = 0;

#define ASSERT_TRUE(expr, msg) do { \
    ++g_test_count; \
    if (!(expr)) { \
        std::printf("  FAIL: %s (line %d)\n", msg, __LINE__); \
        ++g_fail_count; \
    } \
} while (0)

int64_t QpcNow()
{
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

void TestOwnerGuardSequentialAndNested()
{
    std::printf("Test: scheduler owner guard rejects nested admission\n");
    {
        SchedulerOwnerGuard owner;
        ASSERT_TRUE(static_cast<bool>(owner), "first owner acquisition succeeds");
        SchedulerOwnerGuard nested;
        ASSERT_TRUE(!static_cast<bool>(nested), "nested owner acquisition fails open");
    }

    SchedulerOwnerGuard next;
    ASSERT_TRUE(static_cast<bool>(next), "owner releases at scope exit");
}

void TestOwnerGuardContention()
{
    std::printf("Test: scheduler owner guard admits one concurrent producer\n");
    constexpr int kThreadCount = 12;
    std::atomic<bool> start{false};
    std::atomic<int> attempted{0};
    std::atomic<int> acquired{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreadCount);

    for (int index = 0; index < kThreadCount; ++index) {
        threads.emplace_back([&] {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            SchedulerOwnerGuard owner;
            attempted.fetch_add(1, std::memory_order_acq_rel);
            if (!owner) return;
            acquired.fetch_add(1, std::memory_order_relaxed);
            while (attempted.load(std::memory_order_acquire) != kThreadCount)
                std::this_thread::yield();
        });
    }

    start.store(true, std::memory_order_release);
    for (std::thread& thread : threads)
        thread.join();

    ASSERT_TRUE(attempted.load(std::memory_order_relaxed) == kThreadCount,
                "all producers attempted admission");
    ASSERT_TRUE(acquired.load(std::memory_order_relaxed) == 1,
                "exactly one concurrent producer acquired ownership");
}

std::atomic<int> g_owner_wakes{0};
HANDLE g_owner_released = nullptr;

void TestOwnerAwait()
{
    std::printf("Test: the release of an awaited scheduler owner calls the waker\n");
    g_owner_wakes.store(0);
    SchedulerOwner_SetWaker([]() noexcept { g_owner_wakes.fetch_add(1, std::memory_order_relaxed); });

    ASSERT_TRUE(!SchedulerOwner_Await(), "a free owner is not awaited");
    {
        SchedulerOwnerGuard owner;
        ASSERT_TRUE(static_cast<bool>(owner), "the owner is still free after that");
    }
    ASSERT_TRUE(g_owner_wakes.load() == 0, "a release nobody awaited wakes nobody");
    {
        SchedulerOwnerGuard owner;
        ASSERT_TRUE(SchedulerOwner_Await() && SchedulerOwner_Await(), "a held owner is awaited");
        SchedulerOwnerGuard nested;
        ASSERT_TRUE(!static_cast<bool>(nested), "an awaited owner still rejects admission");
    }
    ASSERT_TRUE(g_owner_wakes.load() == 1, "the release of an awaited owner wakes once");
    {
        SchedulerOwnerGuard owner;
        ASSERT_TRUE(static_cast<bool>(owner), "the owner is free after the awaited release");
    }
    ASSERT_TRUE(g_owner_wakes.load() == 1, "the release after it was not awaited");
    SchedulerOwner_SetWaker(nullptr);
}

// The Sleep hook's side of the handoff, without its timer: try, else wait for the release.
bool AcquireAfterRelease(HANDLE released, int* critical)
{
    for (;;) {
        {
            SchedulerOwnerGuard owner;
            if (owner) {
                ++*critical;
                return true;
            }
        }
        ResetEvent(released);
        if (!SchedulerOwner_Await()) continue;
        if (WaitForSingleObject(released, 10'000) != WAIT_OBJECT_0) return false; // a lost wake
    }
}

void TestOwnerHandoff()
{
    std::printf("Test: every release of the scheduler owner reaches its waiter\n");
    constexpr int kRounds = 20000;
    g_owner_wakes.store(0);
    g_owner_released = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    SchedulerOwner_SetWaker([]() noexcept {
        g_owner_wakes.fetch_add(1, std::memory_order_relaxed);
        SetEvent(g_owner_released);
    });
    std::atomic<int> held_round{0}; // the holder has the owner for this round
    std::atomic<int> done_round{0}; // the waiter had it in this round
    std::atomic<bool> lost{false};
    int critical = 0;               // written only under the owner
    int holder_failures = 0;

    // Each round the holder releases once and then leaves the owner alone until the waiter had it:
    // a wake lost in the race between the release and the waiter's mark leaves the waiter parked.
    std::thread waiter([&] {
        for (int round = 1; round <= kRounds; ++round) {
            while (held_round.load(std::memory_order_acquire) != round)
                std::this_thread::yield();
            const bool acquired = AcquireAfterRelease(g_owner_released, &critical);
            if (!acquired) lost.store(true, std::memory_order_release);
            done_round.store(round, std::memory_order_release);
            if (!acquired) return;
        }
    });
    for (int round = 1; round <= kRounds && !lost.load(std::memory_order_acquire); ++round) {
        {
            SchedulerOwnerGuard owner;
            if (owner) ++critical;
            else ++holder_failures;
            held_round.store(round, std::memory_order_release);
            // The release meets the waiter's first attempt at a different moment each round.
            for (int spin = round % 4; spin > 0; --spin)
                std::this_thread::yield();
        }
        while (done_round.load(std::memory_order_acquire) != round)
            std::this_thread::yield();
    }
    waiter.join();

    ASSERT_TRUE(!lost.load(), "no release was missed by the waiter");
    ASSERT_TRUE(holder_failures == 0, "the holder finds the owner free at each round");
    ASSERT_TRUE(critical == 2 * kRounds, "holder and waiter each held the owner once per round");
    ASSERT_TRUE(g_owner_wakes.load() > 0, "releases were awaited");
    SchedulerOwner_SetWaker(nullptr);
    CloseHandle(g_owner_released);
    g_owner_released = nullptr;
}

void TestMarkerLogExactLookupAndExpiry()
{
    std::printf("Test: fixed MarkerLog exact lookup, update, and expiry\n");
    MarkerLog log;
    const int64_t first_qpc = QpcNow();
    log.Record(3, first_qpc, 42);
    ASSERT_TRUE(log.Get(42, 3) == first_qpc, "exact marker key resolves");
    ASSERT_TRUE(log.Get(42, 4) == 0, "different marker type does not alias");
    ASSERT_TRUE(log.Get(43, 3) == 0, "different frame ID does not alias");

    const int64_t updated_qpc = QpcNow();
    log.Record(3, updated_qpc, 42);
    ASSERT_TRUE(log.Get(42, 3) == updated_qpc, "exact marker key updates in place");

    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    log.Record(7, QpcNow() - frequency.QuadPart * 3, 99);
    ASSERT_TRUE(log.Get(99, 7) == 0, "markers older than two seconds expire");
}

void TestMarkerLogConcurrentAccess()
{
    std::printf("Test: fixed MarkerLog supports concurrent producers/readers\n");
    MarkerLog log;
    constexpr int kThreadCount = 8;
    constexpr int kIterations = 4000;
    std::atomic<bool> start{false};
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreadCount);

    for (int index = 0; index < kThreadCount; ++index) {
        threads.emplace_back([&] {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            for (int iteration = 0; iteration < kIterations; ++iteration) {
                const int64_t qpc = QpcNow();
                log.Record(5, qpc, 1234);
                if (log.Get(1234, 5) <= 0)
                    failures.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (std::thread& thread : threads)
        thread.join();

    ASSERT_TRUE(failures.load(std::memory_order_relaxed) == 0,
                "concurrent exact-key access remains valid");
}
}

int main()
{
    std::printf("=== Scheduler CTD-Safety Tests ===\n\n");
    TestOwnerGuardSequentialAndNested();
    TestOwnerGuardContention();
    TestOwnerAwait();
    TestOwnerHandoff();
    TestMarkerLogExactLookupAndExpiry();
    TestMarkerLogConcurrentAccess();

    std::printf("\n=== Summary: %d tests, %d failures ===\n",
                g_test_count, g_fail_count);
    return g_fail_count == 0 ? 0 : 1;
}
