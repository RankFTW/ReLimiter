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
    TestMarkerLogExactLookupAndExpiry();
    TestMarkerLogConcurrentAccess();

    std::printf("\n=== Summary: %d tests, %d failures ===\n",
                g_test_count, g_fail_count);
    return g_fail_count == 0 ? 0 : 1;
}
