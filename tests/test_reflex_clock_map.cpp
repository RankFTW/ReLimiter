// Direct production tests for src/reflex_clock_map.cpp (C++17, src include
// path), not a copied model. Covers the PRESENT_START keyed ring and the
// offset estimator. The NvAPI hook placement, the report ingestion call site
// and the real driver clock relationship still need live validation.
#include "reflex_clock_map.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>

namespace {

int failures = 0;

#define CHECK(condition) do { \
    if (!(condition)) { \
        std::printf("FAIL line %d: %s\n", __LINE__, #condition); \
        ++failures; \
    } \
} while (false)

bool Near(double actual, double expected, double tolerance)
{
    return std::fabs(actual - expected) <= tolerance;
}

// ── PRESENT_START ring ──

void TestExactKeyTake()
{
    int64_t qpc = -1;
    ReflexClockMap_RecordPresentStart(7, 1000, 123456);
    CHECK(ReflexClockMap_TakePresentStart(7, 1000, &qpc));
    CHECK(qpc == 123456);
    // Take consumes: a replayed report for the same frame is unmatched.
    CHECK(!ReflexClockMap_TakePresentStart(7, 1000, &qpc));
    CHECK(qpc == 0);

    ReflexClockMap_RecordPresentStart(7, 1001, 555);
    CHECK(!ReflexClockMap_TakePresentStart(7, 1002, &qpc)); // other slot, never written
    CHECK(!ReflexClockMap_TakePresentStart(7, 1001 + 256, &qpc)); // same slot, other frame
    CHECK(ReflexClockMap_TakePresentStart(7, 1001, &qpc));
    CHECK(qpc == 555);
}

void TestOverwriteAndWrap()
{
    int64_t qpc = 0;
    ReflexClockMap_RecordPresentStart(7, 2000, 111);
    ReflexClockMap_RecordPresentStart(7, 2000 + 256, 222); // replaces the slot
    CHECK(!ReflexClockMap_TakePresentStart(7, 2000, &qpc));
    CHECK(qpc == 0);
    CHECK(ReflexClockMap_TakePresentStart(7, 2000 + 256, &qpc));
    CHECK(qpc == 222);

    // A full lap of distinct frames keeps only the newest lap.
    for (uint64_t frame = 5000; frame < 5000 + 512; ++frame)
        ReflexClockMap_RecordPresentStart(7, frame, static_cast<int64_t>(frame * 10));
    CHECK(!ReflexClockMap_TakePresentStart(7, 5000, &qpc));
    CHECK(!ReflexClockMap_TakePresentStart(7, 5255, &qpc));
    CHECK(ReflexClockMap_TakePresentStart(7, 5256, &qpc));
    CHECK(qpc == 52560);
    CHECK(ReflexClockMap_TakePresentStart(7, 5511, &qpc));
    CHECK(qpc == 55110);

    // Frame IDs above 32 bits index by their low bits and still match exactly.
    const uint64_t large_frame = (uint64_t{1} << 40) + 3;
    ReflexClockMap_RecordPresentStart(7, large_frame, 999);
    CHECK(!ReflexClockMap_TakePresentStart(7, 3, &qpc));
    CHECK(ReflexClockMap_TakePresentStart(7, large_frame, &qpc));
    CHECK(qpc == 999);
}

void TestEpochMismatch()
{
    int64_t qpc = 0;
    ReflexClockMap_RecordPresentStart(8, 3000, 777);
    CHECK(!ReflexClockMap_TakePresentStart(9, 3000, &qpc));
    CHECK(qpc == 0);
    CHECK(ReflexClockMap_TakePresentStart(8, 3000, &qpc));
    CHECK(qpc == 777);

    // A new producer epoch reusing a frame ID replaces the old epoch's entry.
    ReflexClockMap_RecordPresentStart(8, 3001, 1);
    ReflexClockMap_RecordPresentStart(9, 3001, 2);
    CHECK(!ReflexClockMap_TakePresentStart(8, 3001, &qpc));
    CHECK(ReflexClockMap_TakePresentStart(9, 3001, &qpc));
    CHECK(qpc == 2);
}

void TestInvalidInputs()
{
    int64_t qpc = 0;
    ReflexClockMap_RecordPresentStart(0, 4000, 10);
    CHECK(!ReflexClockMap_TakePresentStart(0, 4000, &qpc));
    ReflexClockMap_RecordPresentStart(10, 0, 10);
    CHECK(!ReflexClockMap_TakePresentStart(10, 0, &qpc));
    ReflexClockMap_RecordPresentStart(10, 4001, 0);
    CHECK(!ReflexClockMap_TakePresentStart(10, 4001, &qpc));
    ReflexClockMap_RecordPresentStart(10, 4002, -5);
    CHECK(!ReflexClockMap_TakePresentStart(10, 4002, &qpc));
    ReflexClockMap_RecordPresentStart(10, 4003, 33);
    CHECK(!ReflexClockMap_TakePresentStart(10, 4003, nullptr)); // no output: not consumed
    CHECK(ReflexClockMap_TakePresentStart(10, 4003, &qpc));
    CHECK(qpc == 33);
}

// One writer rewrites a few slots while a reader takes the newest keys. Every
// accepted copy must carry the QPC written for exactly that key; a torn or
// replaced payload would break the encoding. Interleaving is nondeterministic;
// the invariant is checked on every success and the counts are printed.
void TestConcurrentTakeNeverTorn()
{
    constexpr uint32_t kEpoch = 21;
    constexpr uint64_t kFirstFrame = 1ull << 20;
    constexpr uint64_t kWrites = 400000;
    const auto encode = [](uint64_t frame) { return static_cast<int64_t>(frame * 3 + 1); };
    std::atomic<uint64_t> published{0};
    std::atomic<bool> done{false};
    std::thread writer([&] {
        for (uint64_t index = 0; index < kWrites; ++index) {
            // Cycle 4 slots so the reader races active overwrites.
            const uint64_t frame = kFirstFrame + (index / 4) * 256 + (index % 4);
            ReflexClockMap_RecordPresentStart(kEpoch, frame, encode(frame));
            published.store(frame, std::memory_order_release);
        }
        done.store(true, std::memory_order_release);
    });
    uint64_t successes = 0, attempts = 0, mismatches = 0;
    while (!done.load(std::memory_order_acquire)) {
        const uint64_t frame = published.load(std::memory_order_acquire);
        if (frame == 0) continue;
        ++attempts;
        int64_t qpc = 0;
        if (!ReflexClockMap_TakePresentStart(kEpoch, frame, &qpc)) continue;
        ++successes;
        if (qpc != encode(frame)) {
            if (mismatches++ == 0)
                std::printf("FAIL torn take: frame=%llu qpc=%lld expected=%lld\n",
                    static_cast<unsigned long long>(frame), static_cast<long long>(qpc),
                    static_cast<long long>(encode(frame)));
        }
    }
    writer.join();
    CHECK(mismatches == 0);
    std::printf("concurrent take: attempts=%llu successes=%llu mismatches=%llu\n",
        static_cast<unsigned long long>(attempts), static_cast<unsigned long long>(successes),
        static_cast<unsigned long long>(mismatches));
}

// ── Offset conversion ──

void TestOffsetConversion()
{
    double offset = 1.0;
    // 10 MHz QPC after ~14 days: the naive double product would round.
    CHECK(ReflexClockMap_OffsetUs(12345678901237, 10000000, 1234567890135, &offset));
    CHECK(Near(offset, -11.3, 1e-6));
    CHECK(ReflexClockMap_OffsetUs(10000000, 10000000, 1000000, &offset));
    CHECK(offset == 0.0);
    CHECK(ReflexClockMap_OffsetUs(10000000, 10000000, 999988, &offset));
    CHECK(offset == 12.0);

    CHECK(!ReflexClockMap_OffsetUs(0, 10000000, 1, &offset));
    CHECK(offset == 0.0);
    CHECK(!ReflexClockMap_OffsetUs(10, 0, 1, &offset));
    CHECK(!ReflexClockMap_OffsetUs(10, 10000000, 0, &offset));
    CHECK(!ReflexClockMap_OffsetUs(10, 10000000, UINT64_MAX, &offset));
    CHECK(!ReflexClockMap_OffsetUs(10, 10000000, 1, nullptr));
}

void TestStatusText()
{
    CHECK(std::strcmp(ReflexClockMap_StatusText(PacingClockMapStatus::Unknown), "unknown") == 0);
    CHECK(std::strcmp(ReflexClockMap_StatusText(PacingClockMapStatus::Warming), "warming") == 0);
    CHECK(std::strcmp(ReflexClockMap_StatusText(PacingClockMapStatus::Stable), "stable") == 0);
    CHECK(std::strcmp(ReflexClockMap_StatusText(PacingClockMapStatus::Unstable), "unstable") == 0);
    CHECK(std::strcmp(ReflexClockMap_StatusText(static_cast<PacingClockMapStatus>(200)), "unknown") == 0);
}

// ── Estimator ──

constexpr int64_t kFrequency = 10000000; // 10 MHz: 10 ticks per microsecond
constexpr int64_t kBaseQpc = 5000000000000; // ~5.8 days
constexpr int64_t kFrameTicks = 166667;

// Local PRESENT_START for frame index on a whole-microsecond tick.
int64_t LocalQpc(int index)
{
    return kBaseQpc + static_cast<int64_t>(index) * kFrameTicks / 10 * 10;
}

// Driver timestamp that makes local_us - driver_us equal offset_us.
uint64_t DriverUs(int index, double offset_us)
{
    return static_cast<uint64_t>(LocalQpc(index) / 10 - static_cast<int64_t>(offset_us));
}

void Feed(ReflexClockMapEstimator* estimator, uint32_t epoch, int first, int count, double offset_us)
{
    for (int index = first; index < first + count; ++index)
        CHECK(estimator->Update(epoch, kFrequency, LocalQpc(index), DriverUs(index, offset_us)));
}

void TestWarmingStableUnstable()
{
    ReflexClockMapEstimator estimator;
    CHECK(estimator.Status() == PacingClockMapStatus::Unknown);
    CHECK(estimator.OffsetUs() == 0.0);

    const int minimum = static_cast<int>(ReflexClockMapEstimator::kMinimumSamples);
    Feed(&estimator, 3, 0, minimum - 1, -12.0);
    CHECK(estimator.Status() == PacingClockMapStatus::Warming);
    CHECK(estimator.OffsetUs() == -12.0);
    CHECK(estimator.SpreadUs() == 0.0);
    Feed(&estimator, 3, minimum - 1, 1, -12.0);
    CHECK(estimator.SampleCount() == ReflexClockMapEstimator::kMinimumSamples);
    CHECK(estimator.Status() == PacingClockMapStatus::Stable);
    CHECK(estimator.OffsetUs() == -12.0);

    // Unmatched reports in the same epoch neither add samples nor reset.
    CHECK(!estimator.Update(3, kFrequency, 0, DriverUs(0, -12.0)));
    CHECK(!estimator.Update(3, kFrequency, LocalQpc(0), 0));
    CHECK(estimator.SampleCount() == ReflexClockMapEstimator::kMinimumSamples);
    CHECK(estimator.Status() == PacingClockMapStatus::Stable);

    // Alternating 0/500 us offsets: every trimmed range is 500 us.
    ReflexClockMapEstimator bimodal;
    for (int index = 0; index < minimum; ++index)
        Feed(&bimodal, 4, index, 1, (index % 2) ? 500.0 : 0.0);
    CHECK(bimodal.Status() == PacingClockMapStatus::Unstable);
    CHECK(Near(bimodal.SpreadUs(), 500.0, 1e-9));

    // Spread exactly at the threshold is not below it.
    ReflexClockMapEstimator edge;
    for (int index = 0; index < minimum; ++index)
        Feed(&edge, 4, index, 1, (index % 2) ? ReflexClockMapEstimator::kStableSpreadUs : 0.0);
    CHECK(edge.Status() == PacingClockMapStatus::Unstable);
}

void TestResetOnEpochAndFrequency()
{
    ReflexClockMapEstimator estimator;
    Feed(&estimator, 5, 0, 40, 20.0);
    CHECK(estimator.Status() == PacingClockMapStatus::Stable);

    // A new producer epoch starts a new window even for an unmatched report.
    CHECK(!estimator.Update(6, kFrequency, 0, 0));
    CHECK(estimator.SampleCount() == 0);
    CHECK(estimator.Status() == PacingClockMapStatus::Unknown);
    CHECK(estimator.OffsetUs() == 0.0);

    Feed(&estimator, 6, 0, 40, -30.0);
    CHECK(estimator.Status() == PacingClockMapStatus::Stable);
    CHECK(estimator.OffsetUs() == -30.0);

    // A QPC frequency change also resets; the new sample starts warming.
    CHECK(estimator.Update(6, kFrequency * 2, LocalQpc(0), 1));
    CHECK(estimator.SampleCount() == 1);
    CHECK(estimator.Status() == PacingClockMapStatus::Warming);

    estimator.Reset();
    CHECK(estimator.SampleCount() == 0);
    CHECK(estimator.Status() == PacingClockMapStatus::Unknown);
}

void TestConstantOffsetRecovery()
{
    // Non-decimal QPC frequency: driver microseconds truncate, so per-frame
    // offsets fall in (-250, -249]. The median must stay inside that interval.
    constexpr int64_t kOddFrequency = 3579545;
    ReflexClockMapEstimator estimator;
    for (int index = 0; index < 200; ++index) {
        const int64_t local_qpc = 7000000000000 + static_cast<int64_t>(index) * 59659;
        const int64_t seconds = local_qpc / kOddFrequency;
        const int64_t remainder = local_qpc % kOddFrequency;
        const uint64_t local_whole_us = static_cast<uint64_t>(
            seconds * 1000000 + remainder * 1000000 / kOddFrequency);
        CHECK(estimator.Update(9, kOddFrequency, local_qpc, local_whole_us + 250));
    }
    CHECK(estimator.SampleCount() == ReflexClockMapEstimator::kWindowCapacity);
    CHECK(estimator.Status() == PacingClockMapStatus::Stable);
    CHECK(estimator.OffsetUs() > -250.0 - 1e-6 && estimator.OffsetUs() <= -249.0 + 1e-6);
    CHECK(estimator.SpreadUs() < 1.0);
}

void TestOutlierRobustnessAndRollover()
{
    const int window = static_cast<int>(ReflexClockMapEstimator::kWindowCapacity);
    // Three isolated spikes in a full window (two late driver stamps, one early).
    ReflexClockMapEstimator estimator;
    for (int index = 0; index < window; ++index) {
        double offset = -12.0;
        if (index == 17) offset = -900.0;
        if (index == 64) offset = -1500.0;
        if (index == 100) offset = 400.0;
        Feed(&estimator, 11, index, 1, offset);
    }
    CHECK(estimator.Status() == PacingClockMapStatus::Stable);
    CHECK(estimator.OffsetUs() == -12.0);
    CHECK(estimator.SpreadUs() == 0.0);

    // Ten percent of late stamps is not an isolated spike: Unstable, median intact.
    ReflexClockMapEstimator noisy;
    for (int index = 0; index < window; ++index)
        Feed(&noisy, 11, index, 1, (index % 10 == 0) ? -900.0 : -12.0);
    CHECK(noisy.Status() == PacingClockMapStatus::Unstable);
    CHECK(noisy.OffsetUs() == -12.0);

    // The window rolls: an offset step stays Unstable while more old samples
    // remain than the trim discards, turns Stable once the rest are trimmed,
    // and is fully replaced after a whole window. A wrong eviction would leave
    // old values in the sorted view and keep the spread at 1000 us.
    const int trim = window / static_cast<int>(ReflexClockMapEstimator::kTrimDivisor);
    ReflexClockMapEstimator step;
    Feed(&step, 12, 0, 64, 0.0);
    CHECK(step.Status() == PacingClockMapStatus::Stable);
    Feed(&step, 12, 64, window - (trim + 1), 1000.0); // trim + 1 old samples remain
    CHECK(step.SampleCount() == ReflexClockMapEstimator::kWindowCapacity);
    CHECK(step.Status() == PacingClockMapStatus::Unstable);
    CHECK(Near(step.SpreadUs(), 1000.0, 1e-9));
    Feed(&step, 12, 64 + window - (trim + 1), 1, 1000.0); // trim old samples remain
    CHECK(step.Status() == PacingClockMapStatus::Stable);
    CHECK(step.OffsetUs() == 1000.0);
    Feed(&step, 12, 64 + window - trim, trim, 1000.0); // every old sample evicted
    CHECK(step.Status() == PacingClockMapStatus::Stable);
    CHECK(step.OffsetUs() == 1000.0);
    CHECK(step.SpreadUs() == 0.0);
    Feed(&step, 12, 64 + window, trim + 1, 0.0); // new outliers beyond the trim
    CHECK(step.Status() == PacingClockMapStatus::Unstable);
}

} // namespace

int main()
{
    TestExactKeyTake();
    TestOverwriteAndWrap();
    TestEpochMismatch();
    TestInvalidInputs();
    TestConcurrentTakeNeverTorn();
    TestOffsetConversion();
    TestStatusText();
    TestWarmingStableUnstable();
    TestResetOnEpochAndFrequency();
    TestConstantOffsetRecovery();
    TestOutlierRobustnessAndRollover();
    std::printf("reflex clock map: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
