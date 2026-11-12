// Directly tests the production pure law of the unified pacer
// (src/unified_pacer.cpp): interval composition and binding bits, the exact
// start target and the no-catch-up commit, pairs, staleness and I_gpu, the
// hard limit against a scripted environment, start resolution through the
// mailbox, resets, the Sleep-mode tuple, the mailbox packing and the lead
// anchor. The Sleep hook, the owner, the executors and pacing quality still
// need live validation; the scripted environment is a model.

#include "unified_pacer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <vector>

namespace
{
    int s_failures = 0;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,        \
                         #condition);                                           \
            ++s_failures;                                                       \
        }                                                                       \
    } while (false)

#define CHECKF(condition, ...)                                                  \
    do {                                                                        \
        if (!(condition)) {                                                     \
            std::fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__,        \
                         #condition);                                           \
            std::fprintf(stderr, __VA_ARGS__);                                  \
            std::fprintf(stderr, "\n");                                         \
            ++s_failures;                                                       \
        }                                                                       \
    } while (false)

    constexpr int64_t kFreq = 10000000; // 10 MHz QPC
    constexpr int64_t kStart = 123456789012345LL;
    constexpr uint32_t kSeed = 20260101u; // the generated scenarios replay from it
    constexpr double kEarlierTargetUs = 4000.0; // the earlier law's queue target (every_frame_service = false)

    bool Near(double a, double b, double eps = 1e-6)
    {
        return std::fabs(a - b) <= eps;
    }

    int64_t Ticks(double us)
    {
        return static_cast<int64_t>(std::llround(us * static_cast<double>(kFreq) / 1e6));
    }

    double TicksD(double us)
    {
        return us * static_cast<double>(kFreq) / 1e6;
    }

    PacingPlanKey Key(uint64_t frame_id, uint32_t epoch = 1, uint32_t generation = 1)
    {
        PacingPlanKey key{};
        key.identity_domain = PacingIdentityDomain::NvApiFrame;
        key.producer_epoch = epoch;
        key.controller_generation = generation;
        key.frame_id = frame_id;
        return key;
    }

    UnifiedIntervalTerms CapFps(double fps)
    {
        UnifiedIntervalTerms terms{};
        terms.cap_us = 1e6 / fps;
        return terms;
    }

    // Deterministic LCG; every scenario that uses it replays from kSeed.
    struct Lcg {
        uint32_t state;
        explicit Lcg(uint32_t seed) : state(seed) {}
        uint32_t Next()
        {
            state = state * 1664525u + 1013904223u;
            return state >> 8;
        }
    };

    // One frame through the Sleep model: Reserve, the Sleep's start wait, the
    // mailbox publication, Resolve and CommitRelease, then OnPlan.
    struct FrameSpec {
        int64_t arrival = 0;
        int64_t overshoot_ticks = 0;     // timer overshoot after the whole-tick target
        int64_t after_target_ticks = 0;  // hard-limit wait: release no earlier than target + this
        bool floor_release = false;      // release at T.base (a fractional target is never overshot)
        uint64_t frame_id = 0;           // 0: 1000 + grid index
        uint32_t epoch = 1;
        uint32_t key_generation = 1;
    };

    struct FrameResult {
        uint64_t grid = 0;
        PacingPlanKey key{};
        UnifiedStart start{};
        UnifiedResolution resolution{};
        int64_t release = 0;
        uint32_t commit_flags = 0;
        bool committed = false;
        bool reserved = false;
    };

    struct Driver {
        UnifiedPacer law;
        UnifiedIntervalTerms terms;
        uint32_t generation = 1;
        uint64_t grid = 1;
        int64_t last_release = kStart;

        // The tests of the rules of the saturated-pair estimator and of the 4 ms
        // target run the earlier law explicitly (every_frame_service = false,
        // target 4000); `shipped_defaults` runs the law with its default options.
        explicit Driver(const UnifiedIntervalTerms& t = CapFps(120.0), bool shipped_defaults = false) : terms(t)
        {
            law.Reset(1, 1, kFreq);
            if (!shipped_defaults) {
                UnifiedOptions earlier{};
                earlier.every_frame_service = false;
                earlier.queue_target_us = kEarlierTargetUs;
                law.SetOptions(earlier);
            }
        }

        int64_t DefaultArrival() const { return last_release + 100; }

        FrameResult Frame(FrameSpec spec)
        {
            FrameResult out{};
            out.grid = grid;
            if (spec.arrival == 0)
                spec.arrival = DefaultArrival();
            const uint64_t frame_id = spec.frame_id != 0 ? spec.frame_id : 1000 + grid;
            out.key = Key(frame_id, spec.epoch, spec.key_generation);

            out.reserved = law.Reserve(grid, generation, spec.arrival, terms, &out.start) ==
                UnifiedReserveStatus::Reserved;
            CHECK(out.reserved);
            if (!out.reserved)
                return out;

            int64_t release = spec.arrival;
            UnifiedSleepOutcome outcome = UnifiedSleepOutcome::Late;
            if (!out.start.release_now) {
                release = spec.floor_release ? out.start.target.base
                                             : UnifiedPacer_TargetTicks(out.start.target);
                release += spec.overshoot_ticks;
                outcome = UnifiedSleepOutcome::Completed;
            }
            if (spec.after_target_ticks > 0) {
                release = std::max(release,
                    UnifiedPacer_TargetTicks(out.start.target) + spec.after_target_ticks);
                outcome = UnifiedSleepOutcome::Completed;
            }
            release = std::max(release, spec.arrival);

            const uint64_t word = UnifiedMailbox_Pack(out.start.token, outcome, release - spec.arrival);
            out.resolution = law.Resolve(grid, generation, word, spec.arrival, terms);
            CHECK(out.resolution.kind == UnifiedResolutionKind::Consumed);
            out.release = out.resolution.release_qpc;
            out.committed = law.CommitRelease(grid, out.release, &out.commit_flags);
            CHECK(out.committed);
            law.OnPlan(grid, out.key, out.release);
            last_release = out.release;
            ++grid;
            return out;
        }

        FrameResult Frame() { return Frame(FrameSpec{}); }

        // Completion, output and report of a frame (the evidence of 3 to 5
        // frames later; the law only needs it before it is asked).
        void Evidence(const FrameResult& frame, int64_t ready_qpc, double hold_us, double z_raw_us,
            bool output = true)
        {
            law.OnReady(frame.grid, frame.key, ready_qpc);
            if (output)
                law.OnOutput(frame.grid, frame.key, ready_qpc + Ticks(hold_us), hold_us > 0.0);
            law.OnReport(frame.key, z_raw_us);
        }
    };

    constexpr int64_t kReadyBase = 5000000000LL;

    // Feeds `count` frames; frame n completes at kReadyBase + n * G, with no
    // hold and z_raw from z_of(n).
    void FeedFrames(Driver* driver, int count, double service_us,
        const std::function<double(uint64_t)>& z_of, const std::function<double(uint64_t)>& hold_of = nullptr)
    {
        for (int i = 0; i < count; ++i) {
            const FrameResult frame = driver->Frame();
            const double hold = hold_of ? hold_of(frame.grid) : 0.0;
            driver->Evidence(frame,
                kReadyBase + static_cast<int64_t>(frame.grid) * Ticks(service_us), hold,
                z_of(frame.grid));
        }
    }

    // ── 1. Fractional I over 10^6 on-time frames ──

    void TestFractionalIntervalDrift()
    {
        // I = 1e6/119.88 µs = 83416.7... ticks. The timer releases at the whole
        // tick below the target, so the origin keeps the fraction: the target
        // follows n × I exactly (an interval rounded to ticks would drift
        // 0.3 tick per frame).
        Driver driver(CapFps(119.88));
        const double interval_ticks = TicksD(1e6 / 119.88);
        FrameSpec spec{};
        spec.floor_release = true;

        FrameResult first = driver.Frame(spec);
        const int64_t base0 = first.release;
        double worst = 0.0;
        uint64_t n = 1;
        for (int i = 0; i < 1000000; ++i) {
            FrameSpec s{};
            s.floor_release = true;
            s.arrival = driver.last_release + 1; // early: the game waits for the target
            const FrameResult frame = driver.Frame(s);
            ++n;
            // T(n) is exact: base + frac; frame 1 released at its arrival.
            const double ideal = static_cast<double>(n - 1) * interval_ticks;
            const double actual = static_cast<double>(frame.start.target.base - base0) + frame.start.target.frac;
            worst = std::max(worst, std::fabs(actual - ideal));
        }
        CHECKF(worst <= 1.0, "worst drift %.6f ticks", worst);

        // A whole-tick release (ceil) loses at most one tick of the fraction per
        // frame and never lets two releases come closer than I − 1 tick.
        Driver ceil_driver(CapFps(119.88));
        FrameResult previous = ceil_driver.Frame();
        for (int i = 0; i < 20000; ++i) {
            FrameSpec s{};
            s.arrival = ceil_driver.last_release + 1;
            const FrameResult frame = ceil_driver.Frame(s);
            const double spacing = static_cast<double>(frame.release - previous.release);
            CHECKF(spacing >= interval_ticks - 1.0 && spacing <= interval_ticks + 1.0,
                "spacing %.3f at frame %d", spacing, i);
            previous = frame;
        }
    }

    // ── 2. No catch-up ──

    void TestNoCatchUp()
    {
        // Late arrivals, hard-limit waits ending at T + 2I and timer
        // overshoot, with the cap changing along the way.
        Lcg rng(kSeed);
        Driver driver(CapFps(120.0));
        FrameResult previous{};
        bool have_previous = false;
        const double caps[] = {120.0, 60.0, 144.0, 90.0};
        for (int i = 0; i < 4000; ++i) {
            if (i % 500 == 0)
                driver.terms = CapFps(caps[(i / 500) % 4]);

            FrameSpec spec{};
            const uint32_t kind = rng.Next() % 11;
            if (kind == 0 && have_previous)
                spec.arrival = driver.last_release + static_cast<int64_t>(3.0 * TicksD(driver.terms.cap_us)); // late arrival
            else if (kind == 1)
                spec.after_target_ticks = static_cast<int64_t>(2.0 * TicksD(driver.terms.cap_us)); // hard-limit wait to T + 2I
            else
                spec.overshoot_ticks = static_cast<int64_t>(rng.Next() % 300); // timer overshoot
            const FrameResult frame = driver.Frame(spec);
            if (have_previous && !(frame.start.flags & kUpReset)) {
                const double interval_ticks = TicksD(frame.start.interval_us);
                const double gap_from_release = static_cast<double>(frame.start.target.base - previous.release) +
                    frame.start.target.frac;
                CHECKF(gap_from_release >= interval_ticks - 1e-6,
                    "T(n+1) < R(n) + I at frame %d seed %u: %.3f < %.3f", i, kSeed, gap_from_release,
                    interval_ticks);
                const double spacing = static_cast<double>(frame.release - previous.release);
                CHECKF(spacing >= interval_ticks - 1.0,
                    "starts closer than I at frame %d seed %u: %.3f < %.3f", i, kSeed, spacing,
                    interval_ticks);
            }
            previous = frame;
            have_previous = true;
        }

        // A late release is flagged and the next target follows the release.
        Driver late;
        late.Frame();
        FrameSpec spec{};
        spec.after_target_ticks = Ticks(2.0 * late.terms.cap_us);
        const FrameResult delayed = late.Frame(spec);
        CHECK((delayed.commit_flags & kUpLate) != 0);
        const FrameResult next = late.Frame();
        CHECK(next.start.target.base - delayed.release >= Ticks(late.terms.cap_us) - 1);
        CHECK((next.commit_flags & kUpLate) == 0);
    }

    // ── 3. Interval terms, binding, clamp and median ──

    void TestBindingTerms()
    {
        auto reserve_flags = [](const UnifiedIntervalTerms& terms, double* interval_out) {
            UnifiedPacer law;
            law.Reset(1, 1, kFreq);
            UnifiedStart start{};
            CHECK(law.Reserve(1, 1, kStart, terms, &start) == UnifiedReserveStatus::Reserved);
            *interval_out = start.interval_us;
            return start.flags & (kUpBoundCap | kUpBoundVrr | kUpBoundGame | kUpBoundGpu);
        };
        double interval = 0.0;

        UnifiedIntervalTerms terms{};
        terms.cap_us = 8333.33;
        CHECK(reserve_flags(terms, &interval) == kUpBoundCap);
        CHECK(Near(interval, 8333.33));

        terms.vrr_us = 1e6 / UnifiedPacer_VrrCapFps(141.0); // the VRR ceiling is above the cap
        terms.cap_us = 1e6 / 144.0;
        CHECK(reserve_flags(terms, &interval) == kUpBoundVrr);
        CHECK(Near(interval, terms.vrr_us));

        terms = UnifiedIntervalTerms{};
        terms.cap_us = 8333.33;
        terms.game_us = 10000.0;
        CHECK(reserve_flags(terms, &interval) == kUpBoundGame);
        CHECK(Near(interval, 10000.0));

        // Idle: I_idle replaces the cap; I_vrr is left out (and I_gpu, below).
        terms = UnifiedIntervalTerms{};
        terms.cap_us = 8333.33;
        terms.idle_us = 33333.0;
        terms.vrr_us = 50000.0;
        terms.idle = true;
        CHECK(reserve_flags(terms, &interval) == kUpBoundCap);
        CHECK(Near(interval, 33333.0));

        // The idle interval does not shrink below the game's limit.
        terms.game_us = 40000.0;
        CHECK(reserve_flags(terms, &interval) == kUpBoundGame);
        CHECK(Near(interval, 40000.0));

        // Ties set every bit (within 1 µs); a term 1.5 µs below does not bind.
        terms = UnifiedIntervalTerms{};
        terms.cap_us = 8333.0;
        terms.game_us = 8333.9;
        CHECK(reserve_flags(terms, &interval) == (kUpBoundCap | kUpBoundGame));
        CHECK(Near(interval, 8333.9));
        terms.game_us = 8334.5;
        CHECK(reserve_flags(terms, &interval) == kUpBoundGame);
        terms.vrr_us = 8334.5;
        CHECK(reserve_flags(terms, &interval) == (kUpBoundVrr | kUpBoundGame));
        CHECK(UnifiedPacer_BindingFlags(terms, 8334.0, 8334.5) == (kUpBoundVrr | kUpBoundGame | kUpBoundGpu));
        CHECK(UnifiedPacer_BindingFlags(terms, 0.0, 8334.5) == (kUpBoundVrr | kUpBoundGame));

        // No term: no interval, nothing reserved.
        {
            UnifiedPacer law;
            law.Reset(1, 1, kFreq);
            UnifiedStart start{};
            CHECK(law.Reserve(1, 1, kStart, UnifiedIntervalTerms{}, &start) == UnifiedReserveStatus::Invalid);
            CHECK(!law.HasReservation(1));
        }

        // The VRR cap with the cap off and the driver's ceiling.
        CHECK(Near(UnifiedPacer_VrrCapFps(120.0), 116.0));
        CHECK(Near(UnifiedPacer_VrrCapFps(144.0), 138.0));
        CHECK(Near(UnifiedPacer_VrrCapFps(60.0), 59.0));
        CHECK(UnifiedPacer_VrrCapFps(0.0) == 0.0);
        CHECK(UnifiedPacer_VrrCapFps(-5.0) == 0.0);
        CHECK(UnifiedPacer_VrrCapFps(std::nan("")) == 0.0);
    }

    void TestGpuIntervalBindsClampsAndMedian()
    {
        // I_gpu binds: completions every 10 ms, queue wait on target.
        {
            Driver driver(CapFps(120.0));
            FeedFrames(&driver, 30, 10000.0, [](uint64_t) { return kEarlierTargetUs; });
            const UnifiedStatus status = driver.law.Status(driver.grid);
            CHECK(status.bindable);
            CHECK(Near(status.service_us, 10000.0));
            CHECK(Near(status.gpu_interval_us, 10000.0)); // P = 0
            UnifiedStart start{};
            CHECK(driver.law.Reserve(driver.grid, 1, driver.DefaultArrival(), driver.terms, &start) ==
                UnifiedReserveStatus::Reserved);
            CHECK((start.flags & (kUpBoundCap | kUpBoundVrr | kUpBoundGame | kUpBoundGpu)) == kUpBoundGpu);
            CHECK(Near(start.interval_us, 10000.0));
            CHECK(Near(start.gpu_interval_us, 10000.0));
        }
        // I_gpu below the cap does not bind: the cap holds.
        {
            Driver driver(CapFps(60.0));
            FeedFrames(&driver, 30, 10000.0, [](uint64_t) { return kEarlierTargetUs; });
            UnifiedStart start{};
            CHECK(driver.law.Reserve(driver.grid, 1, driver.DefaultArrival(), driver.terms, &start) ==
                UnifiedReserveStatus::Reserved);
            CHECK((start.flags & (kUpBoundCap | kUpBoundGpu)) == kUpBoundCap);
            CHECK(Near(start.interval_us, 1e6 / 60.0));
        }
        // In the Idle regime I_gpu is left out.
        {
            Driver driver(CapFps(120.0));
            FeedFrames(&driver, 30, 10000.0, [](uint64_t) { return kEarlierTargetUs; });
            driver.terms.idle = true;
            driver.terms.idle_us = 33333.0;
            UnifiedStart start{};
            CHECK(driver.law.Reserve(driver.grid, 1, driver.DefaultArrival(), driver.terms, &start) ==
                UnifiedReserveStatus::Reserved);
            CHECK(Near(start.interval_us, 33333.0));
            CHECK(start.gpu_interval_us == 0.0);
            CHECK((start.flags & kUpBoundGpu) == 0);
        }
        // Clamp at +G/8: a queue wait far above target adds at most G/8.
        {
            Driver driver;
            FeedFrames(&driver, 30, 8000.0, [](uint64_t) { return 40000.0; });
            const UnifiedStatus status = driver.law.Status(driver.grid);
            CHECK(status.bindable);
            CHECK(Near(status.gpu_interval_us, 8000.0 + 1000.0));
        }
        // The gain is 0.1 and the earlier law's target 4 ms (set explicitly by the
        // Driver): P = (z̃ − 4000) × 0.1, pinned exactly. The shipped defaults are
        // pinned in TestShippedDefaults.
        CHECK(kUnifiedGain == 0.1);
        CHECK(Driver().law.Options().queue_target_us == 4000.0);
        // No clamp inside ±G/8: z̃ = 6000 → P = +200.
        {
            Driver driver;
            FeedFrames(&driver, 30, 8000.0, [](uint64_t) { return 6000.0; });
            CHECK(Near(driver.law.Status(driver.grid).gpu_interval_us, 8200.0));
        }
        // z̃ = 5000 → P = +100 exactly.
        {
            Driver driver;
            FeedFrames(&driver, 30, 8000.0, [](uint64_t) { return 5000.0; });
            CHECK(Near(driver.law.Status(driver.grid).gpu_interval_us, 8100.0));
        }
        // Below the target P is negative and unclamped inside ±G/8: G = 400 µs,
        // z_used = 3800 → P = −20 (the clamp is −50).
        {
            Driver driver(CapFps(2000.0));
            FeedFrames(&driver, 30, 400.0, [](uint64_t) { return 3800.0; });
            const UnifiedStatus status = driver.law.Status(driver.grid);
            CHECK(status.bindable);
            CHECK(Near(status.gpu_interval_us, 380.0));
        }
        // Clamp at −G/8: G = 200 µs, z_used = 600 → P = −340 → −25.
        {
            Driver driver(CapFps(2000.0));
            FeedFrames(&driver, 30, 200.0, [](uint64_t) { return 600.0; });
            const UnifiedStatus status = driver.law.Status(driver.grid);
            CHECK(status.bindable);
            CHECK(Near(status.service_us, 200.0));
            CHECK(Near(status.gpu_interval_us, 175.0));
        }
        // Median of the newest three used z_used, in any order.
        {
            const double tails[][3] = {{3000.0, 20000.0, 6000.0}, {20000.0, 6000.0, 3000.0}, {6000.0, 3000.0, 20000.0}};
            for (const auto& tail : tails) {
                Driver driver;
                FeedFrames(&driver, 24, 8000.0, [](uint64_t) { return kEarlierTargetUs; });
                const double z0 = tail[0], z1 = tail[1], z2 = tail[2];
                int index = 0;
                FeedFrames(&driver, 3, 8000.0, [&](uint64_t) {
                    const double values[] = {z0, z1, z2};
                    return values[index++];
                });
                const UnifiedStatus status = driver.law.Status(driver.grid);
                CHECK(Near(status.queue_wait_us, 6000.0));
                CHECK(Near(status.queue_wait_raw_us, tail[2]));
                CHECK(Near(status.gpu_interval_us, 8000.0 + 200.0));
            }
        }
    }

    // The cause of the queue cycle measured with a 1.5 ms target: the estimator counts a
    // pair only when z_used > 0.5 ms, so a queue that varies around a target near
    // that threshold takes pairs out of the newest 32. The GPU needs 8 ms per frame,
    // the cap is 144 fps (6.94 ms): with the hardware term the frames start every
    // ~8 ms, without it every 6.94 ms, faster than the GPU.
    void TestSaturationThresholdPairs()
    {
        struct Outcome {
            UnifiedStatus status;
            UnifiedStart start;
        };
        auto run = [](int frames, const std::function<double(uint64_t)>& z_of) {
            Driver driver(CapFps(144.0));
            FeedFrames(&driver, frames, 8000.0, z_of);
            Outcome out{};
            out.status = driver.law.Status(driver.grid);
            CHECK(driver.law.Reserve(driver.grid, 1, driver.DefaultArrival(), driver.terms, &out.start) ==
                UnifiedReserveStatus::Reserved);
            return out;
        };
        const double cap_us = 1e6 / 144.0;
        auto alternate = [](uint64_t n) { return n % 2 == 0 ? 0.0 : 3000.0; };

        // Every frame at 3 ms (a queue standing well above the threshold): all 32
        // pairs are saturated, the service estimate is usable and the term binds.
        {
            const Outcome out = run(40, [](uint64_t) { return 3000.0; });
            CHECK(out.status.valid_pairs == 32);
            CHECK(out.status.service_usable);
            CHECK(out.status.bindable);
            CHECK(Near(out.status.service_us, 8000.0));
            CHECK(Near(out.start.interval_us, 8000.0 + (3000.0 - kEarlierTargetUs) * kUnifiedGain)); // 7900
            CHECK((out.start.flags & kUpBoundGpu) != 0);
        }
        // The queue wait of every second frame is 0 and of the others 3 ms: any 32
        // consecutive frames hold exactly 16 saturated pairs, which is EXACTLY the
        // minimum (kUnifiedMinPairs), so the estimate stays usable and bound. A
        // target near the saturation threshold relies on this edge: one more
        // unsaturated frame in the window and the term unbinds (below). z̃ is the
        // median of the newest three used z_used, so it alternates with the frame
        // parity (0 after a 0 ms frame, 3 ms after a 3 ms frame).
        {
            const Outcome even = run(40, alternate); // the newest frame has queue wait 0: z̃ = 0
            CHECK(even.status.valid_pairs == 16);
            CHECK(even.status.service_usable);
            CHECK(even.status.bindable);
            CHECK(Near(even.status.queue_wait_us, 0.0));
            CHECK(Near(even.start.interval_us, 8000.0 - 400.0)); // P = (0 − 4000) × 0.1
            CHECK((even.start.flags & kUpBoundGpu) != 0);
            CHECK((even.start.flags & kUpFewPairs) == 0);

            const Outcome odd = run(41, alternate); // the newest frame has queue wait 3 ms: z̃ = 3 ms
            CHECK(odd.status.valid_pairs == 16);
            CHECK(odd.status.service_usable);
            CHECK(odd.status.bindable);
            CHECK(Near(odd.status.queue_wait_us, 3000.0));
            CHECK(Near(odd.start.interval_us, 7900.0));          // P = (3000 − 4000) × 0.1
            CHECK((odd.start.flags & kUpBoundGpu) != 0);
        }
        // One saturated frame in three: 11 pairs in the newest 32, fewer than 16:
        // the service is not usable, the term does not bind and the frames start
        // at the cap interval, faster than the GPU's 8 ms.
        {
            const Outcome out = run(40, [](uint64_t n) { return n % 3 == 0 ? 3000.0 : 0.0; });
            CHECK(out.status.valid_pairs == 11);
            CHECK(!out.status.service_usable);
            CHECK(!out.status.bindable);
            CHECK(out.status.gpu_interval_us == 0.0);
            CHECK((out.status.flags & kUpFewPairs) != 0);
            CHECK(Near(out.start.interval_us, cap_us));
            CHECK(out.start.interval_us < 8000.0);
            CHECK((out.start.flags & kUpBoundGpu) == 0);
            CHECK((out.start.flags & kUpBoundCap) != 0);
        }
    }

    // ── 3b. Developer options: every-frame service estimate and the queue target ──

    // Frames whose completions are `spacing_us` apart (the first completes one
    // spacing after kReadyBase), each with its signed queue wait and the hold the
    // successor subtracts.
    struct SpacedFeed {
        int64_t ready = kReadyBase;

        FrameResult Frame(Driver* driver, double spacing_us, double z_raw_us, double hold_us = 0.0,
            double active_us = 0.0)
        {
            const FrameResult frame = driver->Frame();
            ready += Ticks(spacing_us);
            driver->law.OnReady(frame.grid, frame.key, ready);
            driver->law.OnOutput(frame.grid, frame.key, ready + Ticks(hold_us), hold_us > 0.0);
            driver->law.OnReport(frame.key, z_raw_us, active_us);
            return frame;
        }
    };

    Driver OptionDriver(bool every_frame_service, double queue_target_us = kUnifiedQueueTargetUs)
    {
        Driver driver(CapFps(120.0));
        UnifiedOptions options{};
        options.every_frame_service = every_frame_service;
        options.queue_target_us = queue_target_us;
        driver.law.SetOptions(options);
        return driver;
    }

    void TestEveryFrameServiceNoFrameWaited()
    {
        // No frame waited: the signed queue wait is −1 ms on every frame and the
        // completions are 9 ms apart. Today's estimator needs z_used above 0.5 ms,
        // so it has no pair; with the key every frame says the GPU needs 8 ms.
        for (const bool key : {false, true}) {
            Driver driver = OptionDriver(key);
            SpacedFeed feed;
            for (int i = 0; i < 40; ++i)
                feed.Frame(&driver, 9000.0, -1000.0);
            const UnifiedStatus status = driver.law.Status(driver.grid);
            if (!key) {
                CHECK(status.valid_pairs == 0);
                CHECK(!status.service_valid && !status.service_usable && !status.bindable);
                CHECK((status.flags & kUpFewPairs) != 0);
            } else {
                CHECK(status.valid_pairs == 32);
                CHECK(status.service_valid && status.service_usable);
                CHECK(Near(status.service_us, 8000.0));
                CHECK((status.flags & kUpFewPairs) == 0);
                CHECK(status.bindable);
                CHECK(Near(status.gpu_interval_us, 8000.0 - 200.0)); // P = (0 − 2000) × 0.1, the default target
            }
        }
    }

    void TestEveryFrameServiceAllWaitedIsUnchanged()
    {
        // Every frame waited (positive queue wait above the threshold): the sample
        // is today's, so the estimate is identical with and without the key.
        UnifiedStatus statuses[2];
        for (int key = 0; key < 2; ++key) {
            Driver driver = OptionDriver(key != 0);
            SpacedFeed feed;
            for (int i = 0; i < 40; ++i)
                feed.Frame(&driver, 8000.0 + 100.0 * (i % 3), 3000.0 + 100.0 * (i % 5));
            statuses[key] = driver.law.Status(driver.grid);
        }
        CHECK(statuses[0].valid_pairs == 32);
        CHECK(statuses[1].valid_pairs == 32);
        CHECK(statuses[0].service_usable && statuses[1].service_usable);
        CHECK(statuses[0].service_us == statuses[1].service_us);
        CHECK(statuses[0].gpu_interval_us == statuses[1].gpu_interval_us);
        CHECK(Near(statuses[0].service_us, 8100.0, 20.0));
    }

    void TestEveryFrameServiceCycleInMiniature()
    {
        // N.42: 40 frames wait at a spacing of 10 ms, then 40 frames at 9.5 ms in
        // which the GPU waited 0.8 ms each (queue wait −0.8 ms): the GPU needs 8.7 ms.
        // After k frames of the second phase the newest 32 hold 32 − k of the first.
        Driver off = OptionDriver(false);
        Driver on = OptionDriver(true);
        SpacedFeed feed_off;
        SpacedFeed feed_on;
        for (int i = 0; i < 40; ++i) {
            feed_off.Frame(&off, 10000.0, 3000.0);
            feed_on.Frame(&on, 10000.0, 3000.0);
        }
        CHECK(Near(off.law.Status(off.grid).service_us, 10000.0));
        CHECK(Near(on.law.Status(on.grid).service_us, 10000.0));

        double previous_on = 10000.0;
        for (int k = 1; k <= 40; ++k) {
            feed_off.Frame(&off, 9500.0, -800.0);
            feed_on.Frame(&on, 9500.0, -800.0);
            const UnifiedStatus s_off = off.law.Status(off.grid);
            const UnifiedStatus s_on = on.law.Status(on.grid);

            // With the key the estimate falls toward 8.7 ms and stays usable.
            CHECK(s_on.service_usable);
            CHECK(s_on.bindable);
            CHECK(s_on.valid_pairs == 32);
            const double expected = k >= 32 ? 8700.0 : ((32.0 - k) * 10000.0 + k * 8700.0) / 32.0;
            CHECK(Near(s_on.service_us, expected, 1e-6));
            CHECK(s_on.service_us <= previous_on + 1e-9);
            previous_on = s_on.service_us;

            // Without it the estimate stays at 10 ms while 16 pairs remain, then is unusable.
            const uint32_t pairs = k <= 32 ? 32 - k : 0;
            CHECK(s_off.valid_pairs == pairs);
            if (pairs >= 16) {
                CHECK(s_off.service_usable);
                CHECK(Near(s_off.service_us, 10000.0));
            } else {
                CHECK(!s_off.service_usable);
                CHECK(!s_off.bindable);
                CHECK((s_off.flags & kUpFewPairs) != 0);
            }
            if (pairs > 0)
                CHECK(Near(s_off.service_us, 10000.0));
        }
        CHECK(Near(previous_on, 8700.0));
    }

    void TestEveryFrameServiceExcludesNonPositiveSamples()
    {
        // A wait larger than (or equal to) the completion spacing leaves nothing: excluded.
        for (const double wait_us : {9500.0, 9000.0}) {
            Driver driver = OptionDriver(true);
            SpacedFeed feed;
            for (int i = 0; i < 40; ++i)
                feed.Frame(&driver, 9000.0, -wait_us);
            const UnifiedStatus status = driver.law.Status(driver.grid);
            CHECK(status.valid_pairs == 0);
            CHECK(!status.service_usable);
            CHECK(!status.service_sample_valid);
            CHECK(status.gpu_wait_valid);
            CHECK(Near(status.gpu_wait_us, wait_us));
        }
        // Mixed: frames with a non-positive sample drop out, the rest count.
        Driver driver = OptionDriver(true);
        SpacedFeed feed;
        for (int i = 0; i < 40; ++i)
            feed.Frame(&driver, 9000.0, i % 2 == 0 ? -9500.0 : -1000.0);
        const UnifiedStatus status = driver.law.Status(driver.grid);
        CHECK(status.valid_pairs == 16);
        CHECK(Near(status.service_us, 8000.0));
    }

    void TestServiceCorrectionRefusedBelowActiveTime()
    {
        // A corrected sample is never accepted below the GPU's own active time.
        struct Case {
            double wait_us;
            double active_us;
            double expected_us;
            bool refused;
        };
        const Case cases[] = {
            {3000.0, 8000.0, 10000.0, true},  // 7 ms < 8 ms active: keeps the uncorrected 10 ms
            {3000.0, 6500.0, 7000.0, false},  // 7 ms >= 6.5 ms: the corrected sample
            {3000.0, 7000.0, 7000.0, false},  // equal: accepted
            {3000.0, 0.0, 7000.0, false},     // no active time reported
            {0.0, 20000.0, 10000.0, false},   // no wait: never refuses
        };
        for (const Case& c : cases) {
            Driver driver = OptionDriver(true);
            SpacedFeed feed;
            for (int i = 0; i < 40; ++i)
                feed.Frame(&driver, 10000.0, -c.wait_us, 0.0, c.active_us);
            const UnifiedStatus status = driver.law.Status(driver.grid);
            CHECKF(status.valid_pairs == 32, "wait %.0f active %.0f", c.wait_us, c.active_us);
            CHECKF(Near(status.service_us, c.expected_us), "wait %.0f active %.0f: %.3f", c.wait_us, c.active_us,
                status.service_us);
            CHECK(status.service_sample_valid && Near(status.service_sample_us, c.expected_us));
            CHECK(status.service_correction_refused == c.refused);
            CHECK(((status.flags & kUpCorrectionRefused) != 0) == c.refused);
            CHECK(Near(status.gpu_wait_us, c.wait_us));
            // The plan's start flags carry the bit.
            UnifiedStart start{};
            CHECK(driver.law.Reserve(driver.grid, 1, driver.DefaultArrival(), driver.terms, &start) ==
                UnifiedReserveStatus::Reserved);
            CHECK(((start.flags & kUpCorrectionRefused) != 0) == c.refused);
        }
        // With the key off no correction is applied: the telemetry sample is still the one
        // the key would use, the field reports the refusal, but the bit stays clear and
        // the estimator (saturated frames only) has no pair.
        {
            Driver driver = OptionDriver(false);
            SpacedFeed feed;
            for (int i = 0; i < 40; ++i)
                feed.Frame(&driver, 10000.0, -3000.0, 0.0, 8000.0);
            const UnifiedStatus status = driver.law.Status(driver.grid);
            CHECK(status.service_correction_refused);
            CHECK(Near(status.service_sample_us, 10000.0));
            CHECK((status.flags & kUpCorrectionRefused) == 0);
            CHECK(status.valid_pairs == 0);
            UnifiedStart start{};
            CHECK(driver.law.Reserve(driver.grid, 1, driver.DefaultArrival(), driver.terms, &start) ==
                UnifiedReserveStatus::Reserved);
            CHECK((start.flags & kUpCorrectionRefused) == 0);
        }
        // The bit follows the NEWEST used report: a refused frame followed by an accepted one.
        {
            Driver driver = OptionDriver(true);
            SpacedFeed feed;
            for (int i = 0; i < 39; ++i)
                feed.Frame(&driver, 10000.0, -3000.0, 0.0, 8000.0);
            feed.Frame(&driver, 10000.0, -3000.0, 0.0, 6500.0);
            CHECK(!driver.law.Status(driver.grid).service_correction_refused);
            // Mixed estimate: 31 refused (10000) and one accepted (7000) among the newest 32.
            CHECK(Near(driver.law.Status(driver.grid).service_us, (31.0 * 10000.0 + 7000.0) / 32.0));
        }
    }

    void TestStatusServiceSampleAndWait()
    {
        // Pinned values for the newest used report, with the key off and on alike.
        for (const bool key : {false, true}) {
            // The GPU waited 0.8 ms: sample = 9000 − 800.
            {
                Driver driver = OptionDriver(key);
                SpacedFeed feed;
                for (int i = 0; i < 5; ++i)
                    feed.Frame(&driver, 9000.0, -800.0);
                const UnifiedStatus status = driver.law.Status(driver.grid);
                CHECK(status.gpu_wait_valid && Near(status.gpu_wait_us, 800.0));
                CHECK(status.service_sample_valid && Near(status.service_sample_us, 8200.0));
            }
            // The frame's work waited (positive queue wait): no GPU wait, sample = the spacing.
            {
                Driver driver = OptionDriver(key);
                SpacedFeed feed;
                for (int i = 0; i < 5; ++i)
                    feed.Frame(&driver, 9000.0, 3000.0);
                const UnifiedStatus status = driver.law.Status(driver.grid);
                CHECK(status.gpu_wait_valid && status.gpu_wait_us == 0.0);
                CHECK(status.service_sample_valid && Near(status.service_sample_us, 9000.0));
            }
            // The predecessor's hold is taken out: 9000 − 1000 hold − 800 wait.
            {
                Driver driver = OptionDriver(key);
                SpacedFeed feed;
                feed.Frame(&driver, 9000.0, 0.0);
                feed.Frame(&driver, 9000.0, 0.0, 1000.0);
                feed.Frame(&driver, 9000.0, -800.0);
                const UnifiedStatus status = driver.law.Status(driver.grid);
                CHECK(Near(status.service_sample_us, 9000.0 - 1000.0 - 800.0));
                CHECK(Near(status.gpu_wait_us, 800.0));
            }
            // Nothing used yet: neither valid.
            {
                Driver driver = OptionDriver(key);
                SpacedFeed feed;
                feed.Frame(&driver, 9000.0, -800.0); // the first frame has no predecessor: dropped
                const UnifiedStatus status = driver.law.Status(driver.grid);
                CHECK(!status.gpu_wait_valid && !status.service_sample_valid);
            }
        }
    }

    void TestQueueTargetOption()
    {
        auto interval_for = [](double target_us, double z_us) {
            Driver driver = OptionDriver(false, target_us);
            FeedFrames(&driver, 30, 8000.0, [z_us](uint64_t) { return z_us; });
            const UnifiedStatus status = driver.law.Status(driver.grid);
            CHECK(status.bindable);
            return status.gpu_interval_us;
        };
        // The queue term uses the given target: P = (z̃ − target) × 0.1.
        CHECK(Near(interval_for(2000.0, 6000.0), 8400.0));  // +400
        CHECK(Near(interval_for(4000.0, 6000.0), 8200.0));  // +200 (the earlier law's target)
        CHECK(Near(interval_for(7000.0, 6000.0), 7900.0));  // −100
        CHECK(Near(interval_for(0.0, 6000.0), 8600.0));
        // The default is the constant (2000): the shipped law's number.
        CHECK(UnifiedOptions{}.queue_target_us == kUnifiedQueueTargetUs);
        CHECK(Near(interval_for(kUnifiedQueueTargetUs, 6000.0), 8400.0));
        {
            Driver driver(CapFps(120.0), /*shipped_defaults=*/true);
            FeedFrames(&driver, 30, 8000.0, [](uint64_t) { return 6000.0; });
            CHECK(Near(driver.law.Status(driver.grid).gpu_interval_us, 8400.0));
        }
        // A non-finite or negative target falls back to the default.
        CHECK(Near(interval_for(std::nan(""), 6000.0), 8400.0));
        CHECK(Near(interval_for(-1.0, 6000.0), 8400.0));
        // Changing the options changes the next status (the cache is not stale), and a
        // Reset keeps them.
        Driver driver(CapFps(120.0), /*shipped_defaults=*/true);
        FeedFrames(&driver, 30, 8000.0, [](uint64_t) { return 6000.0; });
        CHECK(Near(driver.law.Status(driver.grid).gpu_interval_us, 8400.0));
        UnifiedOptions options{};
        options.queue_target_us = 4000.0;
        driver.law.SetOptions(options);
        CHECK(Near(driver.law.Status(driver.grid).gpu_interval_us, 8200.0));
        driver.law.Reset(driver.grid, 1, kFreq);
        CHECK(driver.law.Options().queue_target_us == 4000.0);
        CHECK(driver.law.Options().every_frame_service); // the other option kept its default
    }

    void TestShippedDefaults()
    {
        // The defaults of the options: every-frame estimate on, queue target 2000 µs.
        const UnifiedOptions defaults{};
        CHECK(defaults.every_frame_service);
        CHECK(defaults.queue_target_us == 2000.0);
        CHECK(kUnifiedQueueTargetUs == 2000.0);
        CHECK(UnifiedPacer{}.Options().every_frame_service);
        CHECK(UnifiedPacer{}.Options().queue_target_us == 2000.0);

        // Equilibrium at the defaults: P = (z̃ − 2000) × 0.1, pinned exactly.
        auto interval_for = [](double z_us) {
            Driver driver(CapFps(120.0), /*shipped_defaults=*/true);
            FeedFrames(&driver, 30, 8000.0, [z_us](uint64_t) { return z_us; });
            const UnifiedStatus status = driver.law.Status(driver.grid);
            CHECK(status.bindable);
            return status.gpu_interval_us;
        };
        CHECK(Near(interval_for(6000.0), 8400.0));   // +400
        CHECK(Near(interval_for(3000.0), 8100.0));   // +100
        CHECK(Near(interval_for(2000.0), 8000.0));   // on target
        CHECK(Near(interval_for(1000.0), 7900.0));   // −100
        CHECK(Near(interval_for(40000.0), 9000.0));  // +G/8 clamp

        // The default estimator needs no standing queue: no frame waited at all, the
        // estimate is still usable (the earlier law has no pair here).
        Driver shipped(CapFps(120.0), /*shipped_defaults=*/true);
        Driver earlier(CapFps(120.0));
        SpacedFeed feed_a;
        SpacedFeed feed_b;
        for (int i = 0; i < 40; ++i) {
            feed_a.Frame(&shipped, 9000.0, -1000.0);
            feed_b.Frame(&earlier, 9000.0, -1000.0);
        }
        const UnifiedStatus a = shipped.law.Status(shipped.grid);
        const UnifiedStatus b = earlier.law.Status(earlier.grid);
        CHECK(a.service_usable && a.bindable && Near(a.service_us, 8000.0));
        CHECK(!b.service_usable && !b.bindable && b.valid_pairs == 0);
    }

    // ── 4. Pairs, hold subtraction and dropped reports ──

    void TestPairsFollowProducerFrameIds()
    {
        // Consecutive frame ids: every report after the first pairs.
        {
            Driver driver;
            FeedFrames(&driver, 20, 8000.0, [](uint64_t) { return 3000.0; });
            const UnifiedStatus status = driver.law.Status(driver.grid);
            CHECK(status.valid_pairs == 19); // frame 1 has no predecessor
            CHECK(driver.law.Counters().reports_used == 19);
            CHECK(driver.law.Counters().reports_unresolved == 1); // frame 1's report
        }
        // A gap in the frame id: the report of the frame after the gap is dropped.
        {
            Driver driver;
            const uint64_t ids[] = {100, 101, 102, 104, 105};
            double z = 3000.0;
            for (const uint64_t id : ids) {
                FrameSpec spec{};
                spec.frame_id = id;
                const FrameResult frame = driver.Frame(spec);
                if (id == 104)
                    z = 77777.0;
                else
                    z = 3000.0;
                driver.Evidence(frame, kReadyBase + static_cast<int64_t>(frame.grid) * Ticks(8000.0), 0.0, z);
            }
            const UnifiedStatus status = driver.law.Status(driver.grid);
            CHECK(status.valid_pairs == 3); // 101, 102, 105
            CHECK(driver.law.Counters().reports_unresolved == 2); // frame 100 and the report after the gap
            CHECK(driver.law.Counters().reports_used == 3);
            CHECK(!Near(status.queue_wait_raw_us, 77777.0)); // never used as z_raw
            CHECK(Near(status.queue_wait_us, 3000.0));
        }
        // Another producer epoch or controller generation: the pair is excluded and the report dropped.
        for (int variant = 0; variant < 2; ++variant) {
            Driver driver;
            FrameResult first = driver.Frame();
            driver.Evidence(first, kReadyBase + Ticks(8000.0), 0.0, 3000.0);
            FrameSpec spec{};
            spec.frame_id = 1002; // consecutive frame id ...
            spec.epoch = variant == 0 ? 2 : 1;            // ... but another epoch
            spec.key_generation = variant == 0 ? 1 : 2;   // ... or another generation
            FrameResult second = driver.Frame(spec);
            const uint64_t before = driver.law.Counters().reports_unresolved;
            driver.Evidence(second, kReadyBase + Ticks(16000.0), 0.0, 55555.0);
            CHECK(driver.law.Counters().reports_unresolved == before + 1);
            const UnifiedStatus status = driver.law.Status(driver.grid);
            CHECK(status.valid_pairs == 0);
            CHECK(!status.has_used);
        }
        // Another lifecycle: a reset between consecutive frame ids drops the pair.
        {
            Driver driver;
            FrameResult first = driver.Frame();
            driver.Evidence(first, kReadyBase + Ticks(8000.0), 0.0, 3000.0);
            driver.law.Reset(driver.grid, 1, kFreq);
            FrameResult second = driver.Frame(); // frame id 1002, but frame 1001 is outside the lifecycle
            const uint64_t before = driver.law.Counters().reports_unresolved;
            driver.Evidence(second, kReadyBase + Ticks(16000.0), 0.0, 55555.0);
            CHECK(driver.law.Counters().reports_unresolved == before + 1);
            CHECK(!driver.law.Status(driver.grid).has_used);
        }
        // Unknown predecessor hold: the report waits, is never z_raw, and is dropped 32 frames later.
        {
            Driver driver;
            FrameResult first = driver.Frame();
            driver.Evidence(first, kReadyBase + Ticks(8000.0), 0.0, 3000.0, /*output=*/false);
            FrameResult second = driver.Frame();
            driver.Evidence(second, kReadyBase + Ticks(16000.0), 0.0, 12345.0);
            const uint64_t unresolved = driver.law.Counters().reports_unresolved; // frame 1 (no predecessor)
            UnifiedStatus status = driver.law.Status(driver.grid);
            CHECK(!status.has_used);
            CHECK(status.valid_pairs == 0);
            CHECK(driver.law.Counters().reports_unresolved == unresolved);
            for (int i = 0; i < 32; ++i)
                driver.Frame();
            status = driver.law.Status(driver.grid); // 33 frames after frame 2: dropped
            CHECK(!status.has_used);
            CHECK(driver.law.Counters().reports_unresolved == unresolved + 1);
            CHECK(driver.law.Counters().reports_used == 0);
        }
        // The hold arrives before the 32 frames are up: the report is used with the hold subtracted.
        {
            Driver driver;
            FrameResult first = driver.Frame();
            driver.Evidence(first, kReadyBase + Ticks(8000.0), 2000.0, 3000.0, /*output=*/false);
            FrameResult second = driver.Frame();
            driver.Evidence(second, kReadyBase + Ticks(16000.0), 0.0, 3200.0);
            CHECK(!driver.law.Status(driver.grid).has_used);
            driver.law.OnOutput(first.grid, first.key, kReadyBase + Ticks(8000.0) + Ticks(2000.0), true);
            const UnifiedStatus status = driver.law.Status(driver.grid);
            CHECK(status.has_used);
            CHECK(Near(status.queue_wait_us, 1200.0));  // 3200 − 2000
            CHECK(Near(status.queue_wait_raw_us, 3200.0));
            CHECK(status.valid_pairs == 1);
        }
    }

    void TestHoldSubtractionAndSignedReport()
    {
        // z_used = max(0, z_raw − hold(pred)); the frame's own hold plays no part.
        {
            Driver driver;
            FrameResult first = driver.Frame();
            driver.Evidence(first, kReadyBase + Ticks(8000.0), 2000.0, 100.0);
            // z_raw 2400 − 2000 = 400: used, but not saturated (≤ 500).
            FrameResult second = driver.Frame();
            driver.Evidence(second, kReadyBase + Ticks(16000.0), 9999.0, 2400.0);
            UnifiedStatus status = driver.law.Status(driver.grid);
            CHECK(status.has_used);
            CHECK(status.valid_pairs == 0);
            CHECK(Near(status.queue_wait_us, 400.0));
            // pred hold 9999 → z_raw 2400 − 9999 → 0, not a pair either.
            FrameResult third = driver.Frame();
            driver.Evidence(third, kReadyBase + Ticks(24000.0), 0.0, 2400.0);
            // pred (second) hold 9999: z_used = 0; the newest two used are 0 and 400.
            status = driver.law.Status(driver.grid);
            CHECK(status.valid_pairs == 0);
            CHECK(Near(status.queue_wait_us, 200.0));
            // pred (third) hold 0: z_used = 2400 > 500: a pair; the frame's own hold (3000) is ignored.
            FrameResult fourth = driver.Frame();
            driver.Evidence(fourth, kReadyBase + Ticks(32000.0), 3000.0, 2400.0);
            status = driver.law.Status(driver.grid);
            CHECK(status.valid_pairs == 1);
        }
        // A negative z_raw stays signed: reported as is, z_used = 0.
        {
            Driver driver;
            FrameResult first = driver.Frame();
            driver.Evidence(first, kReadyBase + Ticks(8000.0), 0.0, 100.0);
            FrameResult second = driver.Frame();
            driver.Evidence(second, kReadyBase + Ticks(16000.0), 0.0, -300.0);
            const UnifiedStatus status = driver.law.Status(driver.grid);
            CHECK(status.has_used);
            CHECK(Near(status.queue_wait_raw_us, -300.0));
            CHECK(Near(status.queue_wait_us, 0.0));
            CHECK(status.valid_pairs == 0);
        }
        // A report with a missing timestamp (NAN) is not a report at all.
        {
            Driver driver;
            FrameResult first = driver.Frame();
            driver.Evidence(first, kReadyBase + Ticks(8000.0), 0.0, std::nan(""));
            CHECK(driver.law.Counters().reports_unresolved == 0);
            CHECK(!driver.law.Status(driver.grid).has_used);
        }
        // A report counts once.
        {
            Driver driver;
            FrameResult first = driver.Frame();
            driver.Evidence(first, kReadyBase + Ticks(8000.0), 0.0, 100.0);
            FrameResult second = driver.Frame();
            driver.Evidence(second, kReadyBase + Ticks(16000.0), 0.0, 3000.0);
            driver.law.OnReport(second.key, 9000.0);
            CHECK(driver.law.Counters().reports_used == 1);
            CHECK(Near(driver.law.Status(driver.grid).queue_wait_raw_us, 3000.0));
        }
    }

    // ── 5. Bindability, freeze and expiry ──

    void TestServiceSampleSubtractsPredecessorHold()
    {
        // A saturated pair whose predecessor was held for h: G_k = spacing − h.
        {
            Driver driver;
            FeedFrames(&driver, 30, 8000.0, [](uint64_t) { return 3000.0; }, [](uint64_t) { return 1500.0; });
            const UnifiedStatus status = driver.law.Status(driver.grid);
            CHECK(status.valid_pairs == 29);
            CHECK(Near(status.service_us, 6500.0));
        }
        // A predecessor that was not held: the plain spacing.
        {
            Driver driver;
            FeedFrames(&driver, 30, 8000.0, [](uint64_t) { return 3000.0; }, [](uint64_t) { return 0.0; });
            CHECK(Near(driver.law.Status(driver.grid).service_us, 8000.0));
        }
        // A sample at or below zero after the subtraction is excluded, not clamped:
        // predecessors alternate held 9000 µs (> spacing) and not held.
        {
            Driver driver;
            FeedFrames(&driver, 30, 8000.0, [](uint64_t) { return 12000.0; },
                [](uint64_t grid) { return grid % 2 == 0 ? 9000.0 : 0.0; });
            const UnifiedStatus status = driver.law.Status(driver.grid);
            CHECK(status.valid_pairs == 15); // frames 2, 4, ..., 30 follow an unheld frame
            CHECK(Near(status.service_us, 8000.0));
        }
        // Exactly zero after the subtraction is excluded too.
        {
            Driver driver;
            FeedFrames(&driver, 10, 8000.0, [](uint64_t) { return 12000.0; }, [](uint64_t) { return 8000.0; });
            CHECK(driver.law.Status(driver.grid).valid_pairs == 0);
        }
    }

    void TestBindabilityAndStaleness()
    {
        // 15 valid pairs: not bindable; 16: bindable.
        {
            Driver driver;
            FeedFrames(&driver, 16, 8000.0, [](uint64_t) { return 3000.0; });
            UnifiedStatus status = driver.law.Status(driver.grid);
            CHECK(status.valid_pairs == 15);
            CHECK(!status.bindable);
            CHECK(!status.service_valid);
            CHECK(status.gpu_interval_us == 0.0);
            CHECK((status.flags & kUpFewPairs) != 0);

            FeedFrames(&driver, 1, 8000.0, [](uint64_t) { return 3000.0; });
            status = driver.law.Status(driver.grid);
            CHECK(status.valid_pairs == 16);
            CHECK(status.bindable);
            CHECK(status.service_valid);
            CHECK(status.gpu_interval_us > 0.0);
            CHECK((status.flags & kUpFewPairs) == 0);
        }
        // Freeze at age 9 and expiry at age 33.
        {
            Driver driver;
            FeedFrames(&driver, 40, 8000.0, [](uint64_t) { return 3000.0; }); // newest used: grid 40
            const uint64_t newest_used = driver.grid - 1;
            auto status_at_age = [&](uint32_t age) { return driver.law.Status(newest_used + 1 + age); };
            UnifiedStatus status = status_at_age(0);
            CHECK(status.age == 0);
            CHECK((status.flags & (kUpStale | kUpExpired)) == 0);
            const double p_gpu = status.gpu_interval_us;
            CHECK(p_gpu > 0.0);

            status = status_at_age(8);
            CHECK(status.age == 8);
            CHECK((status.flags & kUpStale) == 0);
            status = status_at_age(9);
            CHECK(status.age == 9);
            CHECK((status.flags & kUpStale) != 0);
            CHECK((status.flags & kUpExpired) == 0);
            CHECK(status.bindable); // frozen, still bound while the window holds pairs
            CHECK(Near(status.gpu_interval_us, p_gpu));
            CHECK(Near(status.queue_wait_us, 3000.0)); // z̃ frozen

            status = status_at_age(32);
            CHECK(status.age == 32);
            CHECK((status.flags & kUpExpired) == 0);
            status = status_at_age(33);
            CHECK(status.age == 33);
            CHECK((status.flags & kUpExpired) != 0);
            CHECK(!status.bindable);
            CHECK(status.gpu_interval_us == 0.0);
        }
        // A stale P keeps its sign: staleness never sets P negative.
        {
            Driver driver;
            FeedFrames(&driver, 40, 8000.0, [](uint64_t) { return 9000.0; }); // P = +500, inside ±1000
            const uint64_t next = driver.grid;
            const UnifiedStatus fresh = driver.law.Status(next);
            CHECK(Near(fresh.gpu_interval_us, 8500.0));
            const UnifiedStatus stale = driver.law.Status(next + 10);
            CHECK((stale.flags & kUpStale) != 0);
            CHECK(Near(stale.gpu_interval_us, 8500.0));
        }
        // A reset drops the used report and every pair.
        {
            Driver driver;
            FeedFrames(&driver, 30, 8000.0, [](uint64_t) { return 3000.0; });
            CHECK(driver.law.Status(driver.grid).bindable);
            driver.law.Reset(driver.grid, 1, kFreq);
            const UnifiedStatus status = driver.law.Status(driver.grid);
            CHECK(!status.bindable);
            CHECK(!status.has_used);
            CHECK(status.valid_pairs == 0);
        }
    }

    // The pair window is the newest 32 real frames that have a used report: reports lag the
    // release by 5 frames here, so a window over grid indices would hold only 27 reported
    // frames and could never reach the 16-of-32 rule that the offline checks supported.
    void TestPairWindowCountsReportedFrames()
    {
        const auto run = [](bool one_fewer, UnifiedStatus* status, uint64_t* newest_reported) {
            Driver driver;
            std::vector<FrameResult> frames;
            const int total = 45;
            const int lag = 5;
            // Even frames saturate; frames 1 to 8 (outside the newest 32 reported) do too.
            auto z_of = [&](uint64_t grid) {
                if (one_fewer && grid == 10) return 100.0;
                return grid % 2 == 0 ? 3000.0 : 100.0;
            };
            for (int i = 1; i <= total; ++i) {
                frames.push_back(driver.Frame());
                const int delivered = i - lag;
                if (delivered >= 1) {
                    const FrameResult& frame = frames[static_cast<size_t>(delivered - 1)];
                    driver.Evidence(frame, kReadyBase + static_cast<int64_t>(frame.grid) * Ticks(8000.0), 0.0,
                        z_of(frame.grid));
                }
            }
            *newest_reported = static_cast<uint64_t>(total - lag);
            *status = driver.law.Status(driver.grid);
        };
        UnifiedStatus status{};
        uint64_t newest = 0;
        run(false, &status, &newest);
        CHECK(newest == 40);
        CHECK(status.age == 5);
        CHECK(status.valid_pairs == 16); // frames 10, 12, ..., 40 of the reported 9 to 40
        CHECK(status.bindable && status.service_valid);
        CHECK((status.flags & kUpFewPairs) == 0);
        CHECK(Near(status.service_us, 8000.0));
        run(true, &status, &newest);
        CHECK(status.valid_pairs == 15);
        CHECK(!status.bindable && !status.service_valid);
        CHECK((status.flags & kUpFewPairs) != 0);
    }

    // ── 6. Hard limit ──

    class ScriptEnv final : public UnifiedLimitEnv {
    public:
        struct Event {
            int64_t time;   // publication; also the completion's QPC
            uint64_t index;
        };
        int64_t now = 0;
        uint64_t base_ready = 0;
        int64_t base_ready_qpc = 0; // QPC of the newest completion before the events (0 = none)
        std::vector<Event> events;
        int64_t spurious_step = 0; // > 0: also wake without a change every step ticks
        int wait_calls = 0;
        std::vector<int64_t> deadlines;

        int64_t Now() noexcept override { return now; }
        uint64_t ReadyIndex() noexcept override
        {
            uint64_t ready = base_ready;
            for (const Event& event : events) {
                if (event.time <= now)
                    ready = std::max(ready, event.index);
            }
            return ready;
        }
        int64_t ReadyQpc() noexcept override
        {
            int64_t qpc = base_ready_qpc;
            for (const Event& event : events) {
                if (event.time <= now)
                    qpc = std::max(qpc, event.time);
            }
            return qpc;
        }
        void WaitForChange(uint64_t /*observed*/, int64_t deadline_qpc) noexcept override
        {
            ++wait_calls;
            deadlines.push_back(deadline_qpc);
            int64_t wake = deadline_qpc;
            for (const Event& event : events) {
                if (event.time > now && event.time < wake)
                    wake = event.time;
            }
            if (spurious_step > 0 && now + spurious_step < wake)
                wake = now + spurious_step;
            now = wake;
        }
    };

    UnifiedLimitInput LimitInput(uint64_t grid, uint64_t lifecycle_base, int64_t target, int64_t interval)
    {
        UnifiedLimitInput input{};
        input.grid_index = grid;
        input.lifecycle_base = lifecycle_base;
        input.target_qpc = target;
        input.interval_ticks = interval;
        return input;
    }

    constexpr int64_t kLimitInterval = 83300; // 8.33 ms

    void TestHardLimitBlocksAndRetires()
    {
        // The constant is pinned: the hard limit waits at four unfinished frames, never below.
        CHECK(kUnifiedMaxOutstanding == 4);
        // Fewer than four outstanding never wait, whatever the evidence (three is the boundary).
        {
            ScriptEnv env;
            env.now = 1000;
            const UnifiedLimitResult free_result = UnifiedHardLimit(LimitInput(4, 1, 1000, kLimitInterval), &env);
            CHECK(free_result.flags == 0);
            CHECK(free_result.outstanding == 3);
            CHECK(free_result.wait_ticks == 0);
            CHECK(free_result.ready_age_ticks == -1); // no completion yet
            CHECK(env.wait_calls == 0);
        }
        // Three outstanding with fresh evidence and a completion pending still never wait.
        {
            ScriptEnv env;
            env.now = 1000;
            env.base_ready_qpc = 900;
            env.events.push_back({1500, 1});
            const UnifiedLimitResult free_fresh = UnifiedHardLimit(LimitInput(4, 1, 1000, kLimitInterval), &env);
            CHECK(free_fresh.flags == 0);
            CHECK(free_fresh.outstanding == 3);
            CHECK(free_fresh.wait_ticks == 0);
            CHECK(env.wait_calls == 0);
        }
        // Four outstanding with fresh evidence block until a completion lands.
        {
            ScriptEnv env;
            env.now = 1000;
            env.base_ready_qpc = 900;
            env.events.push_back({1500, 1});
            const UnifiedLimitResult blocked = UnifiedHardLimit(LimitInput(5, 1, 1000, kLimitInterval), &env);
            CHECK(blocked.flags == kUpLimitWaited);
            CHECK(blocked.outstanding == 3);
            CHECK(blocked.wait_ticks == 500);
            CHECK(blocked.ready_index == 1);
            CHECK(blocked.ready_age_ticks == 100); // E − the newest completion at E
        }
        // A later completion retires every earlier one.
        {
            ScriptEnv env;
            env.base_ready = 5; // frames 6, 7, 8, 9 outstanding at n = 10
            env.now = 10000;
            env.base_ready_qpc = 9000;
            env.events.push_back({11000, 9});
            const UnifiedLimitResult result = UnifiedHardLimit(LimitInput(10, 1, 10000, kLimitInterval), &env);
            CHECK(result.flags == kUpLimitWaited);
            CHECK(result.outstanding == 0);
            CHECK(result.wait_ticks == 1000);
            CHECK(env.wait_calls == 1);
        }
        // The lifecycle base retires what came before it.
        {
            ScriptEnv env;
            env.base_ready = 5;
            const UnifiedLimitResult result = UnifiedHardLimit(LimitInput(22, 20, 0, kLimitInterval), &env);
            CHECK(result.flags == 0);
            CHECK(result.outstanding == 2); // frames 20 and 21
        }
    }

    void TestHardLimitOneAbsoluteDeadline()
    {
        ScriptEnv env;
        env.now = 5000;
        env.base_ready_qpc = 4900;
        env.spurious_step = 100; // spurious wake-ups, no completion
        const UnifiedLimitResult result = UnifiedHardLimit(LimitInput(5, 1, 5000, 1000), &env);
        CHECK(result.flags == (kUpLimitWaited | kUpLimitTimeout));
        CHECK(result.wait_ticks == 1000);          // not extended by the wake-ups
        CHECK(result.outstanding == 4);
        CHECK(env.wait_calls >= 5);
        for (const int64_t deadline : env.deadlines)
            CHECK(deadline == 6000);               // one absolute deadline every time
    }

    // Completion of frame k at R(k) + depth/2 × I, published `delay` later; frame n
    // is evaluated as it releases (E = T = R(n) = n × I).
    void RunPipeline(int64_t interval_ticks, int64_t delay_ticks, int64_t depth_half_intervals, int* total_waits,
        int* timeouts)
    {
        ScriptEnv env;
        for (uint64_t k = 1; k <= 1000; ++k) {
            const int64_t release = static_cast<int64_t>(k) * interval_ticks;
            env.events.push_back({release + interval_ticks * depth_half_intervals / 2 + delay_ticks, k});
        }
        *total_waits = 0;
        *timeouts = 0;
        for (uint64_t n = 1; n <= 1000; ++n) {
            env.now = static_cast<int64_t>(n) * interval_ticks;
            const UnifiedLimitResult result =
                UnifiedHardLimit(LimitInput(n, 1, env.now, interval_ticks), &env);
            if (result.flags != 0 || result.wait_ticks != 0 ||
                result.outstanding >= static_cast<int32_t>(kUnifiedMaxOutstanding))
                ++*total_waits;
            if (result.flags & kUpLimitTimeout)
                ++*timeouts;
        }
    }

    void TestHardLimitFourDeepPipeline()
    {
        int waits = 0;
        int timeouts = 0;
        // Completion 3.5 intervals after the release: three frames unfinished at each release, below the limit.
        RunPipeline(Ticks(8330.0), 0, 7, &waits, &timeouts); // I = 8.33 ms, published at once
        CHECK(waits == 0);
        CHECK(timeouts == 0);
        RunPipeline(Ticks(16700.0), Ticks(5000.0), 7, &waits, &timeouts); // I = 16.7 ms, 5 ms publication delay
        CHECK(waits == 0);
        CHECK(timeouts == 0);
        // Completion 4.5 intervals after the release: four frames unfinished, the limit engages and every
        // wait ends on a completion inside its one interval (no time-out).
        RunPipeline(Ticks(8330.0), 0, 9, &waits, &timeouts);
        CHECK(waits > 0);
        CHECK(timeouts == 0);
    }

    void TestHardLimitLateCompletionPublication()
    {
        // The completion happened 1 ms before R(n) but is published 5 ms
        // later: the release waits 4 ms and does not time out.
        const int64_t interval = Ticks(8330.0);
        const int64_t release = 100 * interval;
        ScriptEnv env;
        env.base_ready = 6;
        env.base_ready_qpc = release - Ticks(2000.0);
        env.events.push_back({release - Ticks(1000.0) + Ticks(5000.0), 7});
        env.now = release;
        const UnifiedLimitResult result = UnifiedHardLimit(LimitInput(11, 1, release, interval), &env);
        CHECK(result.flags == kUpLimitWaited);
        CHECK(result.wait_ticks == Ticks(4000.0));
        CHECK(result.outstanding == 3);
    }

    void TestHardLimitStaleCompletions()
    {
        // (a) Completions stop for 100 ms while frames keep arriving: once the
        // newest completion is older than two intervals no frame waits.
        ScriptEnv env;
        const int64_t interval = kLimitInterval;
        for (uint64_t k = 1; k <= 20; ++k)
            env.events.push_back({static_cast<int64_t>(k) * interval + interval * 7 / 2, k});
        const int64_t last_completion = 20 * interval + interval * 7 / 2;
        int inactive_frames = 0;
        for (uint64_t n = 1; n <= 40; ++n) {
            env.now = static_cast<int64_t>(n) * interval;
            const int calls_before = env.wait_calls;
            const int64_t age = env.now - last_completion; // at E
            const UnifiedLimitResult result = UnifiedHardLimit(LimitInput(n, 1, env.now, interval), &env);
            if (n <= 24) {
                CHECK(result.flags == 0); // the pipeline still completes (three unfinished at most)
            } else if (age > 2 * interval) {
                CHECK(result.flags == kUpLimitInactive);
                CHECK(result.outstanding == -1);
                CHECK(result.wait_ticks == 0);
                CHECK(env.wait_calls == calls_before);
                ++inactive_frames;
            } else {
                // Fresh enough to wait, never longer than one interval.
                CHECK(result.wait_ticks <= interval);
            }
        }
        CHECK(inactive_frames == 15); // frames 26 to 40
        CHECK(40 * interval - last_completion >= Ticks(100000.0));
    }

    void TestHardLimitEvaluatedAtTarget()
    {
        // (b) An early arrival saw four unfinished frames, but the completion
        // lands before T: evaluated after the start wait, the limit does not wait.
        const int64_t target = 5000000;
        ScriptEnv env;
        env.base_ready = 5;
        env.base_ready_qpc = target - 40000;
        env.events.push_back({target - 5000, 6});
        env.now = target - 30000; // the arrival
        CHECK(UnifiedPacer_Outstanding(10, env.ReadyIndex(), 1) == 4);
        env.now = target;         // the start wait ran to T
        const UnifiedLimitResult result = UnifiedHardLimit(LimitInput(10, 1, target, kLimitInterval), &env);
        CHECK(result.flags == 0);
        CHECK((result.flags & kUpLimitWaited) == 0);
        CHECK(result.outstanding == 3);
        CHECK(result.wait_ticks == 0);
        CHECK(env.wait_calls == 0);
        CHECK(result.ready_age_ticks == 5000);
    }

    void TestHardLimitBacklogWaitsOneInterval()
    {
        // (c) A genuine four-deep backlog with fresh completions: one
        // absolute deadline one interval after T.
        const int64_t target = 5000000;
        {
            ScriptEnv env;
            env.base_ready = 5;
            env.base_ready_qpc = target - 40000;
            env.now = target;
            const UnifiedLimitResult result = UnifiedHardLimit(LimitInput(10, 1, target, kLimitInterval), &env);
            CHECK(result.flags == (kUpLimitWaited | kUpLimitTimeout));
            CHECK(result.wait_ticks == kLimitInterval);
            CHECK(env.now == target + kLimitInterval);
            CHECK(result.outstanding == 4);
            CHECK(env.wait_calls == 1);
            CHECK(env.deadlines[0] == target + kLimitInterval);
        }
        // A completion before the deadline ends the wait early: waited only.
        {
            ScriptEnv env;
            env.base_ready = 5;
            env.base_ready_qpc = target - 40000;
            env.events.push_back({target + 20000, 6});
            env.now = target;
            const UnifiedLimitResult result = UnifiedHardLimit(LimitInput(10, 1, target, kLimitInterval), &env);
            CHECK(result.flags == kUpLimitWaited);
            CHECK(result.wait_ticks == 20000);
            CHECK(result.outstanding == 3);
        }
        // E later than T + I (the start wait overran): the deadline is one
        // interval after E, so the limit still gets its full interval.
        {
            ScriptEnv env;
            env.base_ready = 5;
            env.base_ready_qpc = target + 200000;
            env.now = target + 3 * kLimitInterval; // E
            env.base_ready_qpc = env.now - 50000;
            const UnifiedLimitResult result = UnifiedHardLimit(LimitInput(10, 1, target, kLimitInterval), &env);
            CHECK(result.flags == (kUpLimitWaited | kUpLimitTimeout));
            CHECK(result.wait_ticks == kLimitInterval);
            CHECK(env.deadlines[0] == target + 3 * kLimitInterval + kLimitInterval);
        }
    }

    void TestHardLimitNoCompletionEver()
    {
        // (d) No completion ever (ReadyQpc 0): inactive, however many frames are outstanding.
        for (const uint64_t n : {5ull, 10ull, 200ull}) {
            ScriptEnv env;
            env.now = 777;
            const UnifiedLimitResult result = UnifiedHardLimit(LimitInput(n, 1, 777, kLimitInterval), &env);
            CHECK(result.flags == kUpLimitInactive);
            CHECK(result.outstanding == -1);
            CHECK(result.wait_ticks == 0);
            CHECK(result.ready_age_ticks == -1);
            CHECK(env.wait_calls == 0);
            CHECK(env.now == 777);
        }
    }

    void TestLimitInputsAndOutstanding()
    {
        UnifiedPacer law;
        law.Reset(1, 1, kFreq);
        UnifiedStart start{};
        CHECK(law.Reserve(1, 1, kStart, CapFps(120.0), &start) == UnifiedReserveStatus::Reserved);
        CHECK(start.interval_ticks == std::llround(TicksD(start.interval_us)));
        CHECK(start.interval_ticks >= 1);
        UnifiedStart marker{};
        UnifiedPacer other;
        other.Reset(1, 1, kFreq);
        CHECK(other.PlanStartAtMarker(1, 1, kStart, CapFps(60.0), &marker));
        CHECK(marker.interval_ticks == std::llround(TicksD(1e6 / 60.0)));
        // A tiny interval still rounds to at least one tick.
        UnifiedPacer tiny;
        tiny.Reset(1, 1, kFreq);
        UnifiedIntervalTerms terms{};
        terms.cap_us = 0.01;
        UnifiedStart tiny_start{};
        CHECK(tiny.Reserve(1, 1, kStart, terms, &tiny_start) == UnifiedReserveStatus::Reserved);
        CHECK(tiny_start.interval_ticks == 1);

        CHECK(UnifiedPacer_Outstanding(1, 0, 1) == 0);
        CHECK(UnifiedPacer_Outstanding(4, 0, 1) == 3);
        CHECK(UnifiedPacer_Outstanding(4, 9, 1) == 0);
        CHECK(UnifiedPacer_Outstanding(22, 5, 20) == 2);
    }

    // ── 7. Start resolution ──

    // A driver with two committed frames; the next grid index is 3.
    struct Primed {
        Driver driver;
        Primed() : driver(CapFps(120.0))
        {
            driver.Frame();
            driver.Frame();
        }
    };

    void TestResolveCompletedAndLate()
    {
        // Completed: R from the word; consumed; committed once; never before T.
        {
            Primed p;
            Driver& d = p.driver;
            const uint64_t n = d.grid;
            const int64_t arrival = d.DefaultArrival();
            UnifiedStart start{};
            CHECK(d.law.Reserve(n, 1, arrival, d.terms, &start) == UnifiedReserveStatus::Reserved);
            CHECK(start.token != 0);
            CHECK(!start.release_now);
            const int64_t release = UnifiedPacer_TargetTicks(start.target) + 5;
            const uint64_t word = UnifiedMailbox_Pack(start.token, UnifiedSleepOutcome::Completed, release - arrival);
            const UnifiedResolution resolution = d.law.Resolve(n, 1, word, arrival + 123456, d.terms);
            CHECK(resolution.kind == UnifiedResolutionKind::Consumed);
            CHECK(resolution.consume_word);
            CHECK(resolution.release_qpc == release);
            CHECK(resolution.release_qpc >= start.target.base);
            CHECK(resolution.marker_wait_target_qpc == 0);
            CHECK(resolution.start.token == start.token);
            CHECK(!d.law.HasReservation(n));
            uint32_t flags = 0;
            CHECK(!d.law.CommitRelease(n + 1, release, &flags)); // the index of the reservation only
            CHECK(d.law.CommitRelease(n, release, &flags));
            CHECK(flags == 0);
            CHECK(!d.law.CommitRelease(n, release, &flags)); // once
        }
        // Late: no start wait; R = A; flagged late when over 100 µs after T.
        {
            Primed p;
            Driver& d = p.driver;
            const uint64_t n = d.grid;
            const int64_t arrival = d.last_release + Ticks(30000.0);
            UnifiedStart start{};
            CHECK(d.law.Reserve(n, 1, arrival, d.terms, &start) == UnifiedReserveStatus::Reserved);
            CHECK(start.release_now);
            const uint64_t word = UnifiedMailbox_Pack(start.token, UnifiedSleepOutcome::Late, 0);
            const UnifiedResolution resolution = d.law.Resolve(n, 1, word, arrival, d.terms);
            CHECK(resolution.kind == UnifiedResolutionKind::Consumed);
            CHECK(resolution.release_qpc == arrival);
            uint32_t flags = 0;
            CHECK(d.law.CommitRelease(n, resolution.release_qpc, &flags));
            CHECK((flags & kUpLate) != 0);
            // The next target follows the release: no catch-up.
            UnifiedStart next{};
            CHECK(d.law.Reserve(n + 1, 1, arrival + 10, d.terms, &next) == UnifiedReserveStatus::Reserved);
            CHECK(static_cast<double>(next.target.base - arrival) + next.target.frac >=
                TicksD(next.interval_us) - 1e-6);
        }
    }

    void TestResolveFailedRunningSuperseded()
    {
        // Failed: consumed; the marker waits to T; R from the marker release.
        {
            Primed p;
            Driver& d = p.driver;
            const uint64_t n = d.grid;
            const int64_t arrival = d.DefaultArrival();
            UnifiedStart start{};
            CHECK(d.law.Reserve(n, 1, arrival, d.terms, &start) == UnifiedReserveStatus::Reserved);
            const uint64_t word = UnifiedMailbox_Pack(start.token, UnifiedSleepOutcome::Failed, 0);
            const UnifiedResolution resolution = d.law.Resolve(n, 1, word, arrival, d.terms);
            CHECK(resolution.kind == UnifiedResolutionKind::WaitFailed);
            CHECK(resolution.consume_word);
            CHECK((resolution.start.flags & kUpSleepFailed) != 0);
            CHECK(resolution.marker_wait_target_qpc == UnifiedPacer_TargetTicks(start.target));
            CHECK(resolution.release_qpc == 0);
            uint32_t flags = 0;
            const int64_t marker_release = resolution.marker_wait_target_qpc + 3;
            CHECK(d.law.CommitRelease(n, marker_release, &flags));
            CHECK(marker_release >= start.target.base);
        }
        // Still running: retired, the marker waits to T, a later publication is discarded.
        {
            Primed p;
            Driver& d = p.driver;
            const uint64_t n = d.grid;
            const int64_t arrival = d.DefaultArrival();
            UnifiedStart start{};
            CHECK(d.law.Reserve(n, 1, arrival, d.terms, &start) == UnifiedReserveStatus::Reserved);
            const UnifiedResolution resolution = d.law.Resolve(n, 1, 0, arrival, d.terms);
            CHECK(resolution.kind == UnifiedResolutionKind::WaitRunning);
            CHECK(!resolution.consume_word);
            CHECK((resolution.start.flags & kUpSleepRunning) != 0);
            CHECK(resolution.marker_wait_target_qpc == UnifiedPacer_TargetTicks(start.target));
            CHECK(!d.law.HasReservation(n));
            uint32_t flags = 0;
            CHECK(d.law.CommitRelease(n, resolution.marker_wait_target_qpc, &flags));
            ++d.grid;
            d.last_release = resolution.marker_wait_target_qpc;

            // The slow Sleep publishes after its reservation was retired. The next
            // frame's reservation (another token) does not accept that word.
            const uint64_t stale = UnifiedMailbox_Pack(start.token, UnifiedSleepOutcome::Completed, 1000);
            UnifiedStart next{};
            const int64_t next_arrival = d.DefaultArrival();
            CHECK(d.law.Reserve(d.grid, 1, next_arrival, d.terms, &next) == UnifiedReserveStatus::Reserved);
            CHECK(next.token != start.token);
            const UnifiedResolution mismatch = d.law.Resolve(d.grid, 1, stale, next_arrival, d.terms);
            CHECK(mismatch.kind == UnifiedResolutionKind::WaitRunning);
            CHECK(mismatch.release_qpc == 0);
            CHECK(!mismatch.consume_word);
        }
        // Superseded: a reservation of another index is cleared; the marker path plans the frame.
        {
            Primed p;
            Driver& d = p.driver;
            const uint64_t n = d.grid;
            const int64_t arrival = d.DefaultArrival();
            UnifiedStart start{};
            CHECK(d.law.Reserve(n, 1, arrival, d.terms, &start) == UnifiedReserveStatus::Reserved);
            const uint64_t word = UnifiedMailbox_Pack(start.token, UnifiedSleepOutcome::Completed,
                UnifiedPacer_TargetTicks(start.target) - arrival);
            const UnifiedResolution resolution = d.law.Resolve(n + 1, 1, word, arrival, d.terms);
            CHECK(resolution.kind == UnifiedResolutionKind::MarkerPath);
            CHECK((resolution.start.flags & kUpSuperseded) != 0);
            CHECK((resolution.start.flags & kUpMarkerStart) != 0);
            CHECK(resolution.start.token == 0);
            CHECK(resolution.start.grid_index == n + 1);
            CHECK(!resolution.consume_word);
            CHECK(resolution.release_qpc == 0);
            CHECK(!d.law.HasReservation(n));
            uint32_t flags = 0;
            CHECK(!d.law.CommitRelease(n, arrival, &flags));
            CHECK(d.law.CommitRelease(n + 1, std::max(arrival, resolution.marker_wait_target_qpc), &flags));
            CHECK(d.law.Counters().superseded == 1);
        }
        // Superseded by another controller generation.
        {
            Primed p;
            Driver& d = p.driver;
            const uint64_t n = d.grid;
            const int64_t arrival = d.DefaultArrival();
            UnifiedStart start{};
            CHECK(d.law.Reserve(n, 1, arrival, d.terms, &start) == UnifiedReserveStatus::Reserved);
            const uint64_t word = UnifiedMailbox_Pack(start.token, UnifiedSleepOutcome::Late, 0);
            const UnifiedResolution resolution = d.law.Resolve(n, 2, word, arrival, d.terms);
            CHECK(resolution.kind == UnifiedResolutionKind::MarkerPath);
            CHECK((resolution.start.flags & kUpSuperseded) != 0);
            CHECK((resolution.start.flags & kUpReset) != 0); // the generation restarted the law
            CHECK(resolution.start.release_now);
            CHECK(resolution.marker_wait_target_qpc == 0);
        }
    }

    void TestResolveMissingDuplicateAndEarlyWord()
    {
        // Missing: no Sleep ran; the law plans T(n) at the marker.
        {
            Primed p;
            Driver& d = p.driver;
            const uint64_t n = d.grid;
            const int64_t arrival = d.DefaultArrival();
            const UnifiedResolution resolution = d.law.Resolve(n, 1, 0, arrival, d.terms);
            CHECK(resolution.kind == UnifiedResolutionKind::MarkerPath);
            CHECK((resolution.start.flags & kUpMarkerStart) != 0);
            CHECK((resolution.start.flags & kUpSuperseded) == 0);
            CHECK(!resolution.start.release_now);
            CHECK(resolution.marker_wait_target_qpc == UnifiedPacer_TargetTicks(resolution.start.target));
            uint32_t flags = 0;
            CHECK(d.law.CommitRelease(n, resolution.marker_wait_target_qpc, &flags));
            CHECK(!d.law.CommitRelease(n, resolution.marker_wait_target_qpc, &flags));
        }
        // A stale word from the previous frame is not this reservation's word.
        {
            Primed p;
            Driver& d = p.driver;
            const uint64_t n = d.grid;
            const int64_t arrival = d.DefaultArrival();
            UnifiedStart start{};
            CHECK(d.law.Reserve(n, 1, arrival, d.terms, &start) == UnifiedReserveStatus::Reserved);
            const uint32_t other = start.token == 0x3FFFFFFFu ? 1u : start.token + 1;
            const uint64_t stale = UnifiedMailbox_Pack(other, UnifiedSleepOutcome::Completed,
                UnifiedPacer_TargetTicks(start.target) - arrival);
            const UnifiedResolution resolution = d.law.Resolve(n, 1, stale, arrival, d.terms);
            CHECK(resolution.kind == UnifiedResolutionKind::WaitRunning);
            CHECK(resolution.release_qpc == 0);
        }
        // Duplicate: the second Sleep reserves nothing; the first reservation resolves.
        {
            Primed p;
            Driver& d = p.driver;
            const uint64_t n = d.grid;
            const int64_t arrival = d.DefaultArrival();
            UnifiedStart first{};
            CHECK(d.law.Reserve(n, 1, arrival, d.terms, &first) == UnifiedReserveStatus::Reserved);
            UnifiedStart second{};
            CHECK(d.law.Reserve(n, 1, arrival + 50, d.terms, &second) == UnifiedReserveStatus::Duplicate);
            CHECK(!second.valid);
            CHECK((second.flags & kUpDuplicateSleep) != 0);
            CHECK(d.law.Counters().duplicates == 1);
            CHECK(d.law.Counters().reservations == 3); // two primed frames and this one
            const int64_t release = UnifiedPacer_TargetTicks(first.target);
            const uint64_t word = UnifiedMailbox_Pack(first.token, UnifiedSleepOutcome::Completed, release - arrival);
            const UnifiedResolution resolution = d.law.Resolve(n, 1, word, arrival, d.terms);
            CHECK(resolution.kind == UnifiedResolutionKind::Consumed);
            CHECK(resolution.release_qpc == release);
            uint32_t flags = 0;
            CHECK(d.law.CommitRelease(n, release, &flags));
        }
        // A word whose release is before T (and not a late arrival) is a failed wait: never earlier than T.
        {
            Primed p;
            Driver& d = p.driver;
            const uint64_t n = d.grid;
            const int64_t arrival = d.DefaultArrival();
            UnifiedStart start{};
            CHECK(d.law.Reserve(n, 1, arrival, d.terms, &start) == UnifiedReserveStatus::Reserved);
            const uint64_t word = UnifiedMailbox_Pack(start.token, UnifiedSleepOutcome::Completed,
                (start.target.base - arrival) / 2);
            const UnifiedResolution resolution = d.law.Resolve(n, 1, word, arrival, d.terms);
            CHECK(resolution.kind == UnifiedResolutionKind::WaitFailed);
            CHECK(resolution.release_qpc == 0);
            CHECK(resolution.marker_wait_target_qpc >= start.target.base);
        }
        // Tokens are nonzero and differ between reservations.
        {
            UnifiedPacer law;
            law.Reset(1, 1, kFreq);
            uint32_t previous = 0;
            for (uint64_t n = 1; n <= 2000; ++n) {
                UnifiedStart start{};
                CHECK(law.Reserve(n, 1, kStart + static_cast<int64_t>(n), CapFps(120.0), &start) ==
                    UnifiedReserveStatus::Reserved);
                CHECK(start.token != 0 && start.token <= 0x3FFFFFFFu);
                CHECK(start.token != previous);
                previous = start.token;
            }
        }
    }

    // ── 8. Resets ──

    void TestResets()
    {
        // Cap, N or regime change and key toggle: the owner calls Reset; the next frame starts at once.
        {
            Driver driver;
            FeedFrames(&driver, 30, 8000.0, [](uint64_t) { return 3000.0; });
            CHECK(driver.law.HasOrigin());
            CHECK(driver.law.Status(driver.grid).bindable);
            const uint64_t resets = driver.law.Counters().resets;
            driver.law.Reset(driver.grid, 1, kFreq);
            CHECK(driver.law.Counters().resets == resets + 1);
            CHECK(!driver.law.HasOrigin());
            CHECK(driver.law.LifecycleBase() == driver.grid);
            CHECK(!driver.law.Status(driver.grid).bindable);
            CHECK(!driver.law.Status(driver.grid).has_used);

            driver.terms.cap_us = 2.0 * 1e6 / 120.0; // N = 2
            UnifiedStart start{};
            const int64_t arrival = driver.DefaultArrival();
            CHECK(driver.law.Reserve(driver.grid, 1, arrival, driver.terms, &start) == UnifiedReserveStatus::Reserved);
            CHECK(start.target.base == arrival && start.target.frac == 0.0);
            CHECK(start.release_now);
            CHECK((start.flags & kUpReset) != 0);
            CHECK(Near(start.interval_us, 2.0 * 1e6 / 120.0));
            CHECK((start.flags & kUpBoundGpu) == 0); // cold: nothing binds
            CHECK(driver.law.HasReservation(driver.grid));
            driver.law.Reset(driver.grid, 1, kFreq); // a reset clears the reservation too
            CHECK(!driver.law.HasReservation(driver.grid));
            uint32_t flags = 0;
            CHECK(!driver.law.CommitRelease(driver.grid, arrival, &flags));
        }
        // Controller generation change: Reserve restarts the law at this frame.
        {
            Driver driver;
            driver.Frame();
            driver.Frame();
            UnifiedStart start{};
            const int64_t arrival = driver.DefaultArrival();
            CHECK(driver.law.Reserve(driver.grid, 2, arrival, driver.terms, &start) == UnifiedReserveStatus::Reserved);
            CHECK((start.flags & kUpReset) != 0);
            CHECK(start.release_now);
            CHECK(start.target.base == arrival);
            CHECK(driver.law.LifecycleBase() == driver.grid);
            CHECK(start.controller_generation == 2);
            driver.generation = 2;
            const uint64_t word = UnifiedMailbox_Pack(start.token, UnifiedSleepOutcome::Late, 0);
            const UnifiedResolution resolution = driver.law.Resolve(driver.grid, 2, word, arrival, driver.terms);
            CHECK(resolution.kind == UnifiedResolutionKind::Consumed);
        }
        // 250 ms gap: A − R(n − 1) over 250 ms restarts; at 250 ms or under it does not.
        {
            Driver driver;
            driver.Frame();
            const FrameResult last = driver.Frame();
            const int64_t gap = static_cast<int64_t>(kUnifiedGapResetUs * static_cast<double>(kFreq) / 1e6);
            {
                UnifiedPacer copy = driver.law;
                UnifiedStart start{};
                CHECK(copy.Reserve(driver.grid, 1, last.release + gap, driver.terms, &start) == UnifiedReserveStatus::Reserved);
                CHECK((start.flags & kUpReset) == 0);
                CHECK(start.release_now);
                CHECK(start.target.base < last.release + gap); // late, origin kept
            }
            {
                UnifiedPacer copy = driver.law;
                UnifiedStart start{};
                CHECK(copy.Reserve(driver.grid, 1, last.release + gap + 1, driver.terms, &start) == UnifiedReserveStatus::Reserved);
                CHECK((start.flags & kUpReset) != 0);
                CHECK(start.target.base == last.release + gap + 1);
                CHECK(start.release_now);
            }
            {
                UnifiedPacer copy = driver.law;
                UnifiedStart start{};
                CHECK(copy.PlanStartAtMarker(driver.grid, 1, last.release + gap + 1, driver.terms, &start));
                CHECK((start.flags & kUpReset) != 0);
                CHECK((start.flags & kUpMarkerStart) != 0);
            }
        }
        // Headroom: a quarter of the frames noisy (z_used > 0.5 ms): never bindable, the cap binds exactly.
        {
            Driver driver(CapFps(120.0));
            const double cap_us = driver.terms.cap_us;
            for (int i = 0; i < 400; ++i) {
                const FrameResult frame = driver.Frame();
                const double z = frame.grid % 4 == 0 ? 3000.0 : 100.0;
                driver.Evidence(frame, kReadyBase + static_cast<int64_t>(frame.grid) * Ticks(cap_us), 0.0, z);
                if (i < 40)
                    continue;
                UnifiedStart start{};
                // The next frame's start as a marker-site plan (nothing stored).
                CHECK(driver.law.PlanStartAtMarker(driver.grid, 1, driver.DefaultArrival(), driver.terms, &start));
                CHECK(start.interval_us == cap_us);
                CHECK((start.flags & kUpBoundGpu) == 0);
                CHECK((start.flags & kUpBoundCap) != 0);
                CHECK((start.flags & kUpFewPairs) != 0);
                CHECK(!driver.law.Status(driver.grid).bindable);
                CHECK(start.gpu_interval_us == 0.0);
            }
            const UnifiedStatus status = driver.law.Status(driver.grid);
            CHECK(status.valid_pairs >= 6 && status.valid_pairs <= 9);
            CHECK(driver.law.Counters().reports_used > 300);
        }
    }

    // ── 8b. The used-report stream, liveness and the display interval ──

    void TestUsedReportStream()
    {
        // Once per frame, with the exact z_used (z_raw less the predecessor's hold).
        {
            Driver driver;
            std::vector<FrameResult> frames;
            for (int i = 0; i < 5; ++i) {
                frames.push_back(driver.Frame());
                driver.Evidence(frames.back(), kReadyBase + static_cast<int64_t>(frames.back().grid) * Ticks(8000.0),
                    1000.0, 3000.0 + 10.0 * i);
            }
            UnifiedUsedReport report{};
            // Frame 1 has no predecessor: dropped, never used, never pushed.
            for (int i = 1; i < 5; ++i) {
                CHECK(driver.law.PopUsedReport(&report));
                CHECK(report.grid_index == frames[static_cast<size_t>(i)].grid);
                CHECK(report.key == frames[static_cast<size_t>(i)].key);
                CHECK(Near(report.z_used_us, 3000.0 + 10.0 * i - 1000.0));
            }
            CHECK(!driver.law.PopUsedReport(&report));
            // Asking the law again, or a repeated report, pushes nothing.
            driver.law.Status(driver.grid);
            driver.law.OnReport(frames[3].key, 9999.0);
            CHECK(!driver.law.PopUsedReport(&report));
            CHECK(!driver.law.PopUsedReport(nullptr));
        }
        // A report that becomes used later, through OnOutput, is pushed once with its exact value.
        {
            Driver driver;
            FrameResult first = driver.Frame();
            driver.Evidence(first, kReadyBase + Ticks(8000.0), 2000.0, 3000.0, /*output=*/false);
            FrameResult second = driver.Frame();
            driver.Evidence(second, kReadyBase + Ticks(16000.0), 0.0, 3200.0);
            UnifiedUsedReport report{};
            CHECK(!driver.law.PopUsedReport(&report));
            driver.law.OnOutput(first.grid, first.key, kReadyBase + Ticks(8000.0) + Ticks(2000.0), true);
            // No Status call: the output completing the hold resolves the waiting report at once.
            CHECK(driver.law.PopUsedReport(&report));
            CHECK(report.grid_index == second.grid);
            CHECK(report.key == second.key);
            CHECK(Near(report.z_used_us, 1200.0));
            driver.law.Status(driver.grid);
            driver.law.Status(driver.grid + 1);
            CHECK(!driver.law.PopUsedReport(&report));
        }
        // n's report arrives while its predecessor's output is unknown; the predecessor's
        // output arrives, then n's own readiness: the used report is popped right after the
        // output, and n's readiness pushes nothing more.
        {
            Driver driver;
            FrameResult first = driver.Frame();
            driver.law.OnReady(first.grid, first.key, kReadyBase + Ticks(8000.0));
            FrameResult second = driver.Frame();
            driver.law.OnReport(second.key, 3200.0); // pred (first): hold unknown
            UnifiedUsedReport report{};
            CHECK(!driver.law.PopUsedReport(&report));
            driver.law.OnOutput(first.grid, first.key, kReadyBase + Ticks(8000.0) + Ticks(2000.0), true);
            CHECK(driver.law.PopUsedReport(&report));
            CHECK(report.grid_index == second.grid);
            CHECK(Near(report.z_used_us, 1200.0));
            driver.law.OnReady(second.grid, second.key, kReadyBase + Ticks(16000.0));
            CHECK(!driver.law.PopUsedReport(&report));
            CHECK(driver.law.Counters().reports_used == 1);
        }
        // The predecessor's readiness completing the hold (its output came first) resolves it too.
        {
            Driver driver;
            FrameResult first = driver.Frame();
            driver.law.OnOutput(first.grid, first.key, kReadyBase + Ticks(8000.0) + Ticks(500.0), true);
            FrameResult second = driver.Frame();
            driver.law.OnReport(second.key, 3000.0);
            UnifiedUsedReport report{};
            CHECK(!driver.law.PopUsedReport(&report));
            driver.law.OnReady(first.grid, first.key, kReadyBase + Ticks(8000.0));
            CHECK(driver.law.PopUsedReport(&report));
            CHECK(report.grid_index == second.grid);
            CHECK(Near(report.z_used_us, 2500.0));
            CHECK(!driver.law.PopUsedReport(&report));
        }
        // A negative z_raw is used with z_used = 0 (the value the law stores); a dropped report is never pushed.
        {
            Driver driver;
            FrameResult first = driver.Frame();
            driver.Evidence(first, kReadyBase + Ticks(8000.0), 0.0, 100.0);
            FrameResult second = driver.Frame();
            driver.Evidence(second, kReadyBase + Ticks(16000.0), 0.0, -300.0);
            FrameSpec gap{};
            gap.frame_id = 5000; // not the successor of frame 1002: dropped
            FrameResult third = driver.Frame(gap);
            driver.Evidence(third, kReadyBase + Ticks(24000.0), 0.0, 7777.0);
            UnifiedUsedReport report{};
            CHECK(driver.law.PopUsedReport(&report));
            CHECK(report.grid_index == second.grid);
            CHECK(report.z_used_us == 0.0);
            CHECK(!driver.law.PopUsedReport(&report));
        }
        // Overflow: the ring holds 64; the oldest are dropped and counted. Reset clears it.
        {
            Driver driver;
            for (int i = 0; i < 100; ++i) {
                const FrameResult frame = driver.Frame();
                driver.Evidence(frame, kReadyBase + static_cast<int64_t>(frame.grid) * Ticks(8000.0), 0.0, 3000.0);
            }
            // Frames 2 to 100 were used: 99 reports, the newest 64 (37 to 100) remain.
            CHECK(driver.law.Counters().reports_used == 99);
            CHECK(driver.law.Counters().used_reports_dropped == 35);
            UnifiedUsedReport report{};
            uint64_t expected = 37;
            int popped = 0;
            while (driver.law.PopUsedReport(&report)) {
                CHECK(report.grid_index == expected);
                ++expected;
                ++popped;
            }
            CHECK(popped == 64);

            FrameResult more = driver.Frame();
            driver.Evidence(more, kReadyBase + static_cast<int64_t>(more.grid) * Ticks(8000.0), 0.0, 3000.0);
            driver.law.Reset(driver.grid, 1, kFreq);
            CHECK(!driver.law.PopUsedReport(&report));
            CHECK(driver.law.Counters().used_reports_dropped == 35); // cumulative
        }
    }

    void TestQueueWaitLive()
    {
        Driver driver;
        CHECK(!driver.law.QueueWaitLive(driver.grid)); // nothing used
        FeedFrames(&driver, 5, 8000.0, [](uint64_t) { return 3000.0; });
        const uint64_t newest_used = driver.grid - 1;
        const uint32_t used_before = static_cast<uint32_t>(driver.law.Counters().reports_used);
        CHECK(driver.law.QueueWaitLive(newest_used + 1));
        CHECK(driver.law.QueueWaitLive(newest_used + 1 + 32));   // age 32: live
        CHECK(!driver.law.QueueWaitLive(newest_used + 1 + 33));  // age 33: not live
        CHECK(!driver.law.QueueWaitLive(newest_used + 1 + 100));
        // Const and free of side effects: the law's state is unchanged by asking.
        CHECK(driver.law.Counters().reports_used == used_before);
        CHECK(driver.law.QueueWaitLive(newest_used + 1));
        // A new used report makes it live again.
        const FrameResult frame = driver.Frame();
        driver.Evidence(frame, kReadyBase + static_cast<int64_t>(frame.grid) * Ticks(8000.0), 0.0, 3000.0);
        CHECK(driver.law.QueueWaitLive(frame.grid + 1 + 32));
        CHECK(!driver.law.QueueWaitLive(frame.grid + 1 + 33));
        // After a reset nothing is live.
        driver.law.Reset(driver.grid, 1, kFreq);
        CHECK(!driver.law.QueueWaitLive(driver.grid));
        CHECK(!driver.law.QueueWaitLive(driver.grid + 1));
        // Reports that were dropped (no predecessor) do not make it live.
        FrameResult lone = driver.Frame();
        driver.Evidence(lone, kReadyBase + Ticks(8000.0), 0.0, 3000.0);
        CHECK(!driver.law.QueueWaitLive(driver.grid));
    }

    void TestDisplayInterval()
    {
        UnifiedIntervalTerms terms{};
        terms.cap_us = 1e6 / 120.0;
        CHECK(Near(UnifiedPacer_DisplayInterval(terms), terms.cap_us));
        terms.vrr_us = 7000.0;
        CHECK(Near(UnifiedPacer_DisplayInterval(terms), terms.cap_us));
        terms.game_us = 9000.0;
        CHECK(Near(UnifiedPacer_DisplayInterval(terms), 9000.0));
        terms.vrr_us = 9500.0;
        CHECK(Near(UnifiedPacer_DisplayInterval(terms), 9500.0));

        // Never I_gpu: with the GPU term binding the commanded interval grows, the display interval does not.
        Driver driver(CapFps(120.0));
        FeedFrames(&driver, 30, 10000.0, [](uint64_t) { return kEarlierTargetUs; });
        UnifiedStart start{};
        CHECK(driver.law.Reserve(driver.grid, 1, driver.DefaultArrival(), driver.terms, &start) ==
            UnifiedReserveStatus::Reserved);
        CHECK((start.flags & kUpBoundGpu) != 0);
        CHECK(Near(start.interval_us, 10000.0));
        CHECK(Near(UnifiedPacer_DisplayInterval(driver.terms), 1e6 / 120.0));
        CHECK(UnifiedPacer_DisplayInterval(driver.terms) < start.interval_us);

        // Idle: I_idle replaces the cap; I_vrr is left out.
        UnifiedIntervalTerms idle{};
        idle.cap_us = 8333.33;
        idle.idle_us = 33333.0;
        idle.vrr_us = 50000.0;
        idle.idle = true;
        CHECK(Near(UnifiedPacer_DisplayInterval(idle), 33333.0));
        CHECK(UnifiedPacer_DisplayInterval(UnifiedIntervalTerms{}) == 0.0);
    }

    // ── 8c. The hold credit ──
    //
    // Rule under test: a hold spends the GPU's spare time; spare time is banked
    // over one report window; a debt is repaid by releasing the next frames
    // earlier by the debt. Credit c in [-J, +W*J] (W = kUnifiedExpireAge)
    // starts at 0; each used report refills min(W*J, c + J − active(k)) once;
    // a granted frame spends its hold once (max(-J, c − h)); advance(n) =
    // max(0, −c) while the stream is live; a stream that is not live is
    // {false, 0, 0} with c = 0.

    constexpr double kCreditJ = 8000.0; // the display-side interval, µs
    constexpr double kCreditBank = static_cast<double>(kUnifiedExpireAge) * kCreditJ;

    struct CreditRig {
        Driver driver;
        UnifiedHoldGrant first_grant{};

        CreditRig()
        {
            // A live stream. Reports used before the first grant do not refill
            // (J is unknown); the credit starts at 0 at the first grant, made here.
            Fill(0.0);
            Fill(0.0);
            first_grant = Grant();
        }

        void Known(const FrameResult& frame, double hold_us)
        {
            const int64_t ready = kReadyBase + static_cast<int64_t>(frame.grid) * Ticks(8000.0);
            driver.law.OnReady(frame.grid, frame.key, ready);
            driver.law.OnOutput(frame.grid, frame.key, ready + Ticks(hold_us), hold_us > 0.0);
        }

        // A frame (never granted) with its hold known (0) and a report whose GPU
        // active time is `active_us` (0 = unknown): refills the credit once.
        FrameResult Fill(double active_us)
        {
            const FrameResult frame = driver.Frame();
            Known(frame, 0.0);
            driver.law.OnReport(frame.key, 3000.0, active_us);
            return frame;
        }

        // A frame without a report.
        FrameResult Idle()
        {
            const FrameResult frame = driver.Frame();
            Known(frame, 0.0);
            return frame;
        }

        // Plans a frame, asks for its grant, then its actual hold becomes known.
        UnifiedHoldGrant Grant(double hold_us = 0.0, FrameResult* out = nullptr, double j = kCreditJ)
        {
            const FrameResult frame = driver.Frame();
            const UnifiedHoldGrant grant = driver.law.GrantHold(frame.grid, j);
            Known(frame, hold_us);
            if (out)
                *out = frame;
            return grant;
        }

        // A grant for an already planned frame (its hold stays unknown).
        UnifiedHoldGrant GrantOnly(FrameResult* out, double j = kCreditJ)
        {
            *out = driver.Frame();
            return driver.law.GrantHold(out->grid, j);
        }
    };

    bool Is(const UnifiedHoldGrant& grant, bool live, double credit_us, double advance_us)
    {
        return grant.live == live && Near(grant.credit_us, credit_us) && Near(grant.advance_us, advance_us);
    }

    void TestHoldCreditStartsAtZero()
    {
        CreditRig rig;
        CHECK(Is(rig.first_grant, true, 0.0, 0.0)); // empty bank, no debt
        // Even after reports with spare time were used before the first grant.
        CreditRig other;
        CHECK(Is(other.Grant(), true, 0.0, 0.0));
    }

    void TestHoldCreditBanksAtMostWindowTimesJ()
    {
        CreditRig rig;
        for (int i = 0; i < 50; ++i) {
            rig.Fill(2000.0); // 6000 of spare time per report
            const double expected = std::min(kCreditBank, 6000.0 * (i + 1));
            const UnifiedHoldGrant grant = rig.Grant();
            CHECK(Is(grant, true, expected, 0.0));
            CHECK(grant.credit_us <= kCreditBank);
        }
        CHECK(Is(rig.Grant(), true, kCreditBank, 0.0)); // 256 ms: W × J, never above
    }

    void TestHoldCreditDebtAndAdvance()
    {
        // A debt d gives advance = d.
        {
            CreditRig rig;
            rig.Grant(5000.0); // granted, its 5 ms hold becomes known: c = −5000
            CHECK(Is(rig.Grant(), true, -5000.0, 5000.0));
            // Refill repays: the advance returns to 0.
            rig.Fill(3000.0); // +5000
            CHECK(Is(rig.Grant(), true, 0.0, 0.0));
            rig.Fill(3000.0);
            CHECK(Is(rig.Grant(), true, 5000.0, 0.0)); // the bank has spare time again
        }
        // Active time above J with holds drives the credit to −J and no lower.
        {
            CreditRig rig;
            double lowest = 0.0;
            for (int i = 0; i < 8; ++i) {
                rig.Fill(12000.0);                 // −4000 of spare time per report
                const UnifiedHoldGrant grant = rig.Grant(6000.0);
                lowest = std::min(lowest, grant.credit_us);
                CHECK(grant.credit_us >= -kCreditJ);
                CHECK(Near(grant.advance_us, -grant.credit_us));
            }
            CHECK(Near(lowest, -kCreditJ));
            CHECK(Is(rig.Grant(), true, -kCreditJ, kCreditJ));
        }
        // One long hold goes to −J, not further: the next frames are pulled in by J.
        {
            CreditRig rig;
            rig.Grant(20000.0);
            CHECK(Is(rig.Grant(), true, -kCreditJ, kCreditJ));
        }
        // A hold spends exactly its value, once.
        {
            CreditRig rig;
            rig.Fill(2000.0);
            rig.Fill(2000.0); // 12000
            FrameResult first;
            CHECK(Is(rig.Grant(6000.0, &first), true, 12000.0, 0.0));
            const int64_t ready = kReadyBase + static_cast<int64_t>(first.grid) * Ticks(8000.0);
            rig.driver.law.OnOutput(first.grid, first.key, ready + Ticks(9000.0), true); // a changed repeat
            rig.driver.law.OnReady(first.grid, first.key, ready);
            rig.driver.law.Status(rig.driver.grid);
            CHECK(Is(rig.Grant(), true, 6000.0, 0.0)); // 12000 − 6000, once
        }
        // Debit lag: two frames in flight both saw the bank; both holds are spent when known.
        {
            CreditRig rig;
            rig.Fill(2000.0);
            rig.Fill(2000.0); // 12000
            FrameResult one, two;
            CHECK(Is(rig.GrantOnly(&one), true, 12000.0, 0.0));
            CHECK(Is(rig.GrantOnly(&two), true, 12000.0, 0.0));
            rig.Known(one, 7000.0);
            rig.Known(two, 7000.0); // 12000 − 14000
            CHECK(Is(rig.Grant(), true, -2000.0, 2000.0));
        }
        // The output alone, or the readiness alone, is no known hold: nothing is spent yet.
        {
            CreditRig rig;
            rig.Fill(2000.0); // 6000
            FrameResult first;
            CHECK(Is(rig.GrantOnly(&first), true, 6000.0, 0.0));
            rig.driver.law.OnReady(first.grid, first.key, kReadyBase + Ticks(8000.0));
            CHECK(Is(rig.Grant(), true, 6000.0, 0.0));
            rig.driver.law.OnOutput(first.grid, first.key, kReadyBase + Ticks(8000.0) + Ticks(5000.0), true);
            CHECK(Is(rig.Grant(), true, 1000.0, 0.0)); // spent when it completed
        }
    }

    void TestHoldCreditGrantedFrameWithoutHoldDebitsNothing()
    {
        CreditRig rig;
        rig.Fill(2000.0); // 6000
        FrameResult granted;
        CHECK(Is(rig.GrantOnly(&granted), true, 6000.0, 0.0));
        CHECK(Is(rig.Grant(), true, 6000.0, 0.0)); // its hold is still unknown
        rig.Known(granted, 0.0);                  // a frame that was not held
        CHECK(Is(rig.Grant(), true, 6000.0, 0.0));
        // A frame that was never granted spends nothing either.
        const FrameResult stray = rig.driver.Frame();
        rig.Known(stray, 6000.0);
        CHECK(Is(rig.Grant(), true, 6000.0, 0.0));
    }

    void TestHoldCreditStreamNotLive()
    {
        // Nothing used yet: not live, credit 0, no advance.
        {
            Driver driver;
            const FrameResult frame = driver.Frame();
            CHECK(Is(driver.law.GrantHold(frame.grid, kCreditJ), false, 0.0, 0.0));
        }
        // A banked credit and a debt are both dropped when the stream is not live.
        {
            CreditRig rig;
            for (int i = 0; i < 10; ++i)
                rig.Fill(2000.0); // 60000
            CHECK(Is(rig.Grant(), true, 60000.0, 0.0));
            for (int i = 0; i < 34; ++i)
                rig.Idle(); // the newest used report is older than 32 frames
            CHECK(Is(rig.Grant(5000.0), false, 0.0, 0.0)); // its hold is not debited
            rig.Fill(8000.0); // a live stream again, no spare time
            rig.Fill(8000.0);
            CHECK(Is(rig.Grant(), true, 0.0, 0.0));
        }
        {
            CreditRig rig;
            rig.Grant(5000.0); // c = −5000
            CHECK(Is(rig.Grant(), true, -5000.0, 5000.0));
            for (int i = 0; i < 34; ++i)
                rig.Idle();
            CHECK(Is(rig.Grant(), false, 0.0, 0.0));
            rig.Fill(8000.0);
            rig.Fill(8000.0);
            CHECK(Is(rig.Grant(), true, 0.0, 0.0)); // not −5000
        }
        // Live at age 32, not live at age 33 (the law's own rule). The newest used
        // report is frame 2; the rig's own grant is frame 3.
        {
            CreditRig rig;
            for (int i = 0; i < 31; ++i)
                rig.Idle();
            CHECK(rig.Grant().live); // the grant's frame is index 35: age 32
        }
        {
            CreditRig rig;
            for (int i = 0; i < 32; ++i)
                rig.Idle();
            CHECK(!rig.Grant().live); // index 36: age 33
        }
        // Invalid J: {false, 0, 0}, nothing recorded.
        {
            CreditRig rig;
            const FrameResult frame = rig.driver.Frame();
            CHECK(Is(rig.driver.law.GrantHold(frame.grid, 0.0), false, 0.0, 0.0));
            CHECK(Is(rig.driver.law.GrantHold(frame.grid, std::nan("")), false, 0.0, 0.0));
            CHECK(Is(rig.driver.law.GrantHold(frame.grid, -5.0), false, 0.0, 0.0));
            CHECK(rig.driver.law.GrantHold(frame.grid, kCreditJ).live); // the frame was not consumed
        }
    }

    void TestHoldCreditResets()
    {
        auto bank = [](CreditRig* rig) {
            for (int i = 0; i < 10; ++i)
                rig->Fill(2000.0);
            CHECK(Is(rig->Grant(), true, 60000.0, 0.0));
        };
        // Reset gives 0.
        {
            CreditRig rig;
            bank(&rig);
            rig.driver.law.Reset(rig.driver.grid, 1, kFreq);
            rig.Fill(8000.0); // no predecessor in the new lifecycle: dropped
            rig.Fill(8000.0); // live again
            CHECK(Is(rig.Grant(), true, 0.0, 0.0));
        }
        // A debt is reset too.
        {
            CreditRig rig;
            rig.Grant(5000.0);
            CHECK(Is(rig.Grant(), true, -5000.0, 5000.0));
            rig.driver.law.Reset(rig.driver.grid, 1, kFreq);
            rig.Fill(8000.0);
            rig.Fill(8000.0);
            CHECK(Is(rig.Grant(), true, 0.0, 0.0));
        }
        // A controller generation change resets the law, and with it the credit.
        {
            CreditRig rig;
            bank(&rig);
            const uint64_t resets = rig.driver.law.Counters().resets;
            rig.driver.generation = 2;
            rig.Fill(8000.0); // Reserve restarts the law at this frame
            CHECK(rig.driver.law.Counters().resets == resets + 1);
            rig.Fill(8000.0);
            CHECK(Is(rig.Grant(), true, 0.0, 0.0));
        }
        // A frame of the old lifecycle spends nothing after the reset.
        {
            CreditRig rig;
            FrameResult pending;
            rig.GrantOnly(&pending);
            rig.driver.law.Reset(rig.driver.grid, 1, kFreq);
            rig.Known(pending, 6000.0);
            rig.Fill(8000.0);
            rig.Fill(8000.0);
            CHECK(Is(rig.Grant(), true, 0.0, 0.0));
        }
    }

    void TestHoldCreditBoundsFollowJ()
    {
        // The bounds are W × J and −J of the latest grant: a changed J clamps the credit into them.
        {
            CreditRig rig;
            for (int i = 0; i < 50; ++i)
                rig.Fill(2000.0);
            CHECK(Is(rig.Grant(), true, kCreditBank, 0.0));
            CHECK(Is(rig.Grant(0.0, nullptr, 4000.0), true, 32.0 * 4000.0, 0.0));
        }
        {
            CreditRig rig;
            rig.Grant(20000.0); // −8000
            CHECK(Is(rig.Grant(0.0, nullptr, 4000.0), true, -4000.0, 4000.0));
        }
    }

    void TestHoldCreditRefillsOncePerReport()
    {
        // Repeated events, Status and Grant calls never refill a report twice.
        {
            CreditRig rig;
            const FrameResult frame = rig.Fill(2000.0); // +6000
            for (int i = 0; i < 3; ++i) {
                rig.Known(frame, 0.0);
                rig.driver.law.OnReport(frame.key, 3000.0, 2000.0);
                rig.driver.law.Status(rig.driver.grid);
                rig.driver.law.Status(rig.driver.grid + 1);
            }
            FrameResult next;
            const UnifiedHoldGrant first = rig.GrantOnly(&next);
            CHECK(Is(first, true, 6000.0, 0.0));
            CHECK(Is(rig.driver.law.GrantHold(next.grid, kCreditJ), true, 6000.0, 0.0));
            CHECK(Is(rig.driver.law.GrantHold(next.grid, kCreditJ), true, 6000.0, 0.0));
            rig.Known(next, 0.0);
            CHECK(Is(rig.Grant(), true, 6000.0, 0.0)); // still 6000
        }
        // A report used later (its predecessor's hold arrives afterwards) refills once, then.
        {
            CreditRig rig;
            const FrameResult pred = rig.driver.Frame();
            const int64_t pred_ready = kReadyBase + static_cast<int64_t>(pred.grid) * Ticks(8000.0);
            rig.driver.law.OnReady(pred.grid, pred.key, pred_ready);
            const FrameResult x = rig.driver.Frame();
            rig.Known(x, 0.0);
            rig.driver.law.OnReport(x.key, 3000.0, 2000.0); // waits for pred's hold
            CHECK(Is(rig.Grant(), true, 0.0, 0.0));         // not used yet: no refill
            rig.driver.law.OnOutput(pred.grid, pred.key, pred_ready, false);
            rig.driver.law.OnOutput(pred.grid, pred.key, pred_ready, false);
            rig.driver.law.OnReady(pred.grid, pred.key, pred_ready);
            rig.driver.law.Status(rig.driver.grid);
            CHECK(Is(rig.Grant(), true, 6000.0, 0.0));      // refilled exactly once
        }
        // A report without a known GPU active time is used but proves no spare time.
        {
            CreditRig rig;
            rig.Fill(0.0);
            CHECK(Is(rig.Grant(), true, 0.0, 0.0));
        }
        // A dropped report (no predecessor) does not refill.
        {
            CreditRig rig;
            FrameSpec gap{};
            gap.frame_id = 90000; // not the successor of the previous frame
            const FrameResult lone = rig.driver.Frame(gap);
            rig.Known(lone, 0.0);
            rig.driver.law.OnReport(lone.key, 3000.0, 1000.0);
            CHECK(Is(rig.Grant(), true, 0.0, 0.0));
        }
    }

    void TestHoldCreditRepeatedGrantIsStored()
    {
        // A repeated grant of the same frame returns the stored grant even when a
        // refill AND a debit happened in between (it recomputes nothing).
        CreditRig rig;
        rig.Fill(2000.0);
        rig.Fill(2000.0);
        rig.Fill(2000.0); // 18000
        FrameResult p;
        FrameResult f;
        CHECK(Is(rig.GrantOnly(&p), true, 18000.0, 0.0)); // p's hold is still unknown
        const UnifiedHoldGrant g1 = rig.GrantOnly(&f);
        CHECK(Is(g1, true, 18000.0, 0.0));
        rig.Known(f, 0.0);
        rig.Known(p, 4000.0); // a debit: 18000 − 4000
        rig.Fill(2000.0);     // a refill: + 6000 → 20000
        const UnifiedHoldGrant g2 = rig.driver.law.GrantHold(f.grid, kCreditJ);
        CHECK(Is(g2, true, 18000.0, 0.0)); // unchanged
        CHECK(g2.credit_us == g1.credit_us && g2.advance_us == g1.advance_us && g2.live == g1.live);
        CHECK(Is(rig.Grant(), true, 20000.0, 0.0)); // the refill and the debit both happened
    }


    void TestServiceUsableDefinition()
    {
        // The one usable-Ĝ definition the hardware term uses: none, bindable, frozen (stale) and expired.
        Driver none;
        FeedFrames(&none, 3, 8000.0, [](uint64_t) { return 3000.0; });
        const UnifiedStatus cold = none.law.Status(none.grid);
        CHECK(!cold.service_usable);
        CHECK(!cold.bindable);

        Driver driver;
        FeedFrames(&driver, 40, 8000.0, [](uint64_t) { return 3000.0; });
        const uint64_t newest_used = driver.grid - 1;
        UnifiedStatus status = driver.law.Status(newest_used + 1);
        CHECK(status.service_usable);
        CHECK(status.bindable == (status.service_usable && status.gpu_interval_us > 0.0));
        CHECK(status.bindable);
        // Frozen, not expired: still usable (and bindable) while the window holds 16 pairs.
        for (const uint32_t age : {9u, 16u}) {
            status = driver.law.Status(newest_used + 1 + age);
            CHECK((status.flags & kUpStale) != 0);
            CHECK((status.flags & kUpExpired) == 0);
            CHECK(status.service_usable);
            CHECK(status.bindable == status.service_usable);
        }
        // Expired (age 33): not usable.
        status = driver.law.Status(newest_used + 1 + 33);
        CHECK((status.flags & kUpExpired) != 0);
        CHECK(!status.service_usable);
        CHECK(!status.bindable);
        // Fewer than 16 pairs: not usable.
        Driver few;
        FeedFrames(&few, 16, 8000.0, [](uint64_t) { return 3000.0; });
        const UnifiedStatus fewer = few.law.Status(few.grid);
        CHECK(fewer.valid_pairs == 15);
        CHECK(!fewer.service_usable);
        CHECK(!fewer.bindable);
    }

    // ── 8d. Hold share (unified_hold_share) and the measured frame interval C ──

    Driver ShareDriver(double share, const UnifiedIntervalTerms& terms = CapFps(120.0))
    {
        Driver driver(terms, /*shipped_defaults=*/true);
        UnifiedOptions options{};
        options.hold_share = share;
        driver.law.SetOptions(options);
        return driver;
    }

    void TestHoldShareMeasuredCycle()
    {
        // C is the plain completion spacing: holds and GPU waits are NOT subtracted.
        Driver driver = ShareDriver(0.06);
        SpacedFeed feed;
        for (int i = 0; i < 40; ++i)
            feed.Frame(&driver, 10000.0, i % 2 == 0 ? -800.0 : 3000.0, 1500.0);
        const UnifiedStatus status = driver.law.Status(driver.grid);
        CHECK(status.cycle_valid);
        CHECK(Near(status.cycle_us, 10000.0));
        // The service estimate does subtract the hold: it is not C.
        CHECK(status.service_usable);
        CHECK(!Near(status.service_us, status.cycle_us, 500.0));

        // Not valid below the minimum count (15 spacings), valid at 16.
        for (const int frames : {16, 17}) {
            Driver d = ShareDriver(0.06);
            SpacedFeed f;
            for (int i = 0; i < frames; ++i)
                f.Frame(&d, 10000.0, 3000.0);
            const UnifiedStatus s = d.law.Status(d.grid);
            CHECK(s.cycle_valid == (frames == 17));
            CHECK(s.cycle_us == (frames == 17 ? 10000.0 : 0.0));
        }
        // A spacing of zero or less is excluded: half the frames complete together with
        // their predecessor, the other spacings give C.
        {
            Driver d = ShareDriver(0.06);
            SpacedFeed f;
            for (int i = 0; i < 60; ++i)
                f.Frame(&d, i % 2 == 1 ? 0.0 : 12000.0, 3000.0);
            const UnifiedStatus s = d.law.Status(d.grid);
            CHECK(s.cycle_valid);
            CHECK(Near(s.cycle_us, 12000.0)); // included zeros would make the median 0
        }
        // C is the median: one spacing ten times the others (a hitch) does not move it.
        {
            Driver d = ShareDriver(0.06);
            SpacedFeed f;
            for (int i = 0; i < 40; ++i)
                f.Frame(&d, 10000.0, 3000.0);
            const UnifiedStatus before = d.law.Status(d.grid);
            f.Frame(&d, 100000.0, 3000.0);
            const UnifiedStatus after = d.law.Status(d.grid);
            CHECK(after.cycle_valid);
            CHECK(after.cycle_us == before.cycle_us);
            CHECK(Near(after.cycle_us, 10000.0));
        }
        // An even count uses the lower middle value: 9 spacings of 9 ms and 9 of 20 ms
        // (18 spacings, sorted index 8) give 9 ms, not the upper 20 ms or the mean 14.5 ms.
        {
            Driver d = ShareDriver(0.06);
            SpacedFeed f;
            f.Frame(&d, 10000.0, 3000.0); // the first frame has no predecessor
            for (int i = 0; i < 18; ++i)
                f.Frame(&d, i % 2 == 0 ? 9000.0 : 20000.0, 3000.0);
            const UnifiedStatus s = d.law.Status(d.grid);
            CHECK(s.cycle_valid);
            CHECK(Near(s.cycle_us, 9000.0));
        }
        // An odd count uses the exact middle: 17 spacings, 9 of 9 ms and 8 of 20 ms.
        {
            Driver d = ShareDriver(0.06);
            SpacedFeed f;
            f.Frame(&d, 10000.0, 3000.0);
            for (int i = 0; i < 17; ++i)
                f.Frame(&d, i % 2 == 0 ? 9000.0 : 20000.0, 3000.0);
            CHECK(Near(d.law.Status(d.grid).cycle_us, 9000.0));
        }
        // Freshness: valid up to age 32, absent from 33; Reset clears it.
        {
            Driver d = ShareDriver(0.06);
            SpacedFeed f;
            for (int i = 0; i < 40; ++i)
                f.Frame(&d, 10000.0, 3000.0);
            const uint64_t newest_used = d.grid - 1;
            CHECK(d.law.Status(newest_used + 1 + 32).cycle_valid);
            const UnifiedStatus expired = d.law.Status(newest_used + 1 + 33);
            CHECK(!expired.cycle_valid && expired.cycle_us == 0.0);
            d.law.Reset(d.grid, 1, kFreq);
            CHECK(!d.law.Status(d.grid).cycle_valid);
        }
    }

    // No cap the hardware can reach: only the VRR ceiling (12.7 ms) as the display-side
    // interval J while the GPU completes a frame every `spacing_us`; each frame's report
    // carries active_of(grid) of GPU active time and ai_of(grid) of frame-generation time
    // (0 = not reported). Returns the credit of the next frame's grant.
    using PerFrame = std::function<double(uint64_t)>;

    double ShareCredit(double share, double spacing_us, const PerFrame& active_of, int frames,
        const PerFrame& ai_of, UnifiedHoldGrant* grant_out = nullptr)
    {
        UnifiedIntervalTerms terms{};
        terms.vrr_us = 12700.0;
        Driver driver = ShareDriver(share, terms);
        SpacedFeed feed;
        for (int i = 0; i < frames; ++i) {
            const FrameResult frame = driver.Frame();
            driver.law.GrantHold(frame.grid, UnifiedPacer_DisplayInterval(terms));
            feed.ready += Ticks(spacing_us);
            driver.law.OnReady(frame.grid, frame.key, feed.ready);
            driver.law.OnOutput(frame.grid, frame.key, feed.ready, false);
            driver.law.OnReport(frame.key, 3000.0, active_of(frame.grid), ai_of(frame.grid));
        }
        const FrameResult last = driver.Frame();
        const UnifiedHoldGrant grant = driver.law.GrantHold(last.grid, UnifiedPacer_DisplayInterval(terms));
        CHECK(grant.live);
        if (grant_out)
            *grant_out = grant;
        return grant.credit_us;
    }

    double ShareCredit(double share, double spacing_us, double active_us, int frames = 60, double ai_us = 5000.0,
        UnifiedHoldGrant* grant_out = nullptr)
    {
        return ShareCredit(share, spacing_us, [active_us](uint64_t) { return active_us; }, frames,
            [ai_us](uint64_t) { return ai_us; }, grant_out);
    }

    constexpr double kShareJ = 12700.0;
    constexpr double kShareC = 14700.0;
    constexpr double kShareA0 = 0.06 * kShareC; // 882: the allowance with a large frame-generation time

    void TestHoldShareOffIsTheSignedRule()
    {
        // Share 0: the refill is the signed J − active (negative values included), the
        // credit drains to −J and the frame may not be held; the same as a law without
        // the option, whatever frame-generation time the reports carry.
        UnifiedHoldGrant grant{};
        CHECK(Near(ShareCredit(0.0, kShareC, 13000.0, 60, 5000.0, &grant), -kShareJ));
        CHECK(Near(grant.advance_us, kShareJ));
        CHECK(Near(ShareCredit(0.0, kShareC, 13000.0, 20, 5000.0), -19.0 * 300.0));
        CHECK(ShareCredit(0.0, kShareC, 13000.0, 60, 0.0) == ShareCredit(0.0, kShareC, 13000.0, 60, 5000.0));

        // Equal to a law without the option on every grant.
        UnifiedIntervalTerms terms{};
        terms.vrr_us = kShareJ;
        Driver a = ShareDriver(0.0, terms);
        Driver b(terms, /*shipped_defaults=*/true);
        SpacedFeed fa, fb;
        for (int i = 0; i < 40; ++i) {
            const FrameResult x = a.Frame();
            const FrameResult y = b.Frame();
            const UnifiedHoldGrant ga = a.law.GrantHold(x.grid, UnifiedPacer_DisplayInterval(terms));
            const UnifiedHoldGrant gb = b.law.GrantHold(y.grid, UnifiedPacer_DisplayInterval(terms));
            CHECK(ga.live == gb.live && ga.credit_us == gb.credit_us && ga.advance_us == gb.advance_us);
            fa.ready += Ticks(kShareC);
            fb.ready += Ticks(kShareC);
            a.law.OnReady(x.grid, x.key, fa.ready);
            a.law.OnOutput(x.grid, x.key, fa.ready, false);
            a.law.OnReport(x.key, 3000.0, 13000.0, 5000.0);
            b.law.OnReady(y.grid, y.key, fb.ready);
            b.law.OnOutput(y.grid, y.key, fb.ready, false);
            b.law.OnReport(y.key, 3000.0, 13000.0, 5000.0);
        }
        const UnifiedStatus s = a.law.Status(a.grid);
        CHECK(!s.cycle_valid && s.cycle_us == 0.0 && s.generation_us == 0.0); // nothing measured
    }

    void TestHoldShareRoomKeepsTheSignedRule()
    {
        // room = J − Am = 7700 ≥ a = 882: the signed refill and the bound W × J although
        // the share is set; heavy frames would keep debiting.
        const double plain = ShareCredit(0.0, kShareC, 5000.0);
        CHECK(plain > 0.0);
        CHECK(ShareCredit(0.06, kShareC, 5000.0) == plain);
        const double bank_j = static_cast<double>(kUnifiedExpireAge) * kShareJ;
        CHECK(Near(ShareCredit(0.06, kShareC, 5000.0, 200), bank_j));
        CHECK(Near(ShareCredit(0.0, kShareC, 5000.0, 200), bank_j));
        // With no frame-generation time (A = 0) the case is the signed one whatever the
        // share, even where no cap gives room (no budget is invented without it).
        CHECK(ShareCredit(0.06, kShareC, 13000.0, 60, 0.0) == ShareCredit(0.0, kShareC, 13000.0, 60, 0.0));
        CHECK(Near(ShareCredit(0.25, kShareC, 13000.0, 60, 0.0), -kShareJ));
    }

    void TestHoldShareNoRoomRefillsTheAllowance()
    {
        // room = J − Am = −300 < a: reports 2..16 have no valid C (signed −300 each, 15
        // reports), reports 17.. refill a = 882 on every report. Bank W × a = 28224.
        const double bank_a = static_cast<double>(kUnifiedExpireAge) * kShareA0;
        UnifiedHoldGrant grant{};
        const double early = ShareCredit(0.06, kShareC, 13000.0, 25, 5000.0, &grant);
        CHECK(Near(early, -15.0 * 300.0 + 9.0 * kShareA0));
        CHECK(grant.live && grant.advance_us == 0.0);
        // Never above W × a: a long run sits exactly at the bank, not at W × J.
        CHECK(Near(ShareCredit(0.06, kShareC, 13000.0, 200), bank_a));
        CHECK(bank_a < static_cast<double>(kUnifiedExpireAge) * kShareJ);

        // Heavy and light frames refill the same: every fourth report from frame 20 on
        // is light (1 ms) or unknown (0), the median stays heavy, the refill stays a.
        auto heavy_with = [](double odd_us) {
            return [odd_us](uint64_t grid) { return grid >= 20 && grid % 4 == 0 ? odd_us : 13000.0; };
        };
        const PerFrame ai = [](uint64_t) { return 5000.0; };
        const double expected = -15.0 * 300.0 + 9.0 * kShareA0;
        CHECK(Near(ShareCredit(0.06, kShareC, heavy_with(1000.0), 25, ai), expected));
        CHECK(Near(ShareCredit(0.06, kShareC, heavy_with(0.0), 25, ai), expected));

        // A credit above W × a is clamped when the case is entered: light frames bank
        // +11700 each under the signed rule before report 17.
        const PerFrame light_early = [](uint64_t grid) { return grid <= 16 && grid % 4 == 0 ? 1000.0 : 13000.0; };
        const double entered = ShareCredit(0.06, kShareC, light_early, 25, ai);
        CHECK(Near(entered, bank_a));
        CHECK(Near(ShareCredit(0.06, kShareC, light_early, 17, ai), bank_a)); // at the first no-room refill

        // The allowance is min(share × C, A): A below the share term -> A, above -> share × C.
        CHECK(Near(ShareCredit(0.06, kShareC, 13000.0, 40, 500.0), -15.0 * 300.0 + 24.0 * 500.0));
        CHECK(Near(ShareCredit(0.06, kShareC, 13000.0, 40, 5000.0), -15.0 * 300.0 + 24.0 * kShareA0));
        CHECK(Near(ShareCredit(0.03, kShareC, 13000.0, 40, 5000.0), -15.0 * 300.0 + 24.0 * 0.03 * kShareC));
    }

    void TestHoldShareCaseFlipsBothWays()
    {
        // Heavy for 70 frames (no room: the credit sits at W × a), light for 60 (the
        // median active time falls, room appears, the signed rule banks up to W × J),
        // heavy again for 20 (no room: clamped back to W × a).
        const double bank_a = static_cast<double>(kUnifiedExpireAge) * kShareA0;
        const PerFrame ai = [](uint64_t) { return 5000.0; };
        const PerFrame phases = [](uint64_t grid) { return grid > 70 && grid <= 130 ? 5000.0 : 13000.0; };
        CHECK(Near(ShareCredit(0.06, kShareC, phases, 70, ai), bank_a));
        const double after_light = ShareCredit(0.06, kShareC, phases, 130, ai);
        CHECK(after_light > bank_a);
        CHECK(after_light <= static_cast<double>(kUnifiedExpireAge) * kShareJ);
        CHECK(Near(ShareCredit(0.06, kShareC, phases, 150, ai), bank_a));
    }

    void TestHoldShareGrantClampsWhenTheCaseIsEntered()
    {
        // A credit banked under the signed rule (room, light frames) is clamped to W × a at
        // the grant where the case is entered (here a smaller display-side interval J
        // leaves no room), not only at the next refill.
        UnifiedIntervalTerms terms{};
        terms.vrr_us = kShareJ;
        Driver driver = ShareDriver(0.06, terms);
        SpacedFeed feed;
        for (int i = 0; i < 60; ++i) {
            const FrameResult frame = driver.Frame();
            driver.law.GrantHold(frame.grid, kShareJ);
            feed.ready += Ticks(kShareC);
            driver.law.OnReady(frame.grid, frame.key, feed.ready);
            driver.law.OnOutput(frame.grid, frame.key, feed.ready, false);
            driver.law.OnReport(frame.key, 3000.0, 5000.0, 5000.0);
        }
        const FrameResult next = driver.Frame();
        const UnifiedHoldGrant grant = driver.law.GrantHold(next.grid, 5000.0); // room = 0 < a = 882
        CHECK(grant.live);
        CHECK(Near(grant.credit_us, static_cast<double>(kUnifiedExpireAge) * kShareA0));
        // The same credit at the old J is far above W × a.
        Driver other = ShareDriver(0.06, terms);
        SpacedFeed f2;
        for (int i = 0; i < 60; ++i) {
            const FrameResult frame = other.Frame();
            other.law.GrantHold(frame.grid, kShareJ);
            f2.ready += Ticks(kShareC);
            other.law.OnReady(frame.grid, frame.key, f2.ready);
            other.law.OnOutput(frame.grid, frame.key, f2.ready, false);
            other.law.OnReport(frame.key, 3000.0, 5000.0, 5000.0);
        }
        const FrameResult again = other.Frame();
        CHECK(other.law.GrantHold(again.grid, kShareJ).credit_us > 10.0 * kShareA0 * kUnifiedExpireAge);
    }

    void TestHoldShareRefillBoundHoldsBetweenGrants()
    {
        // Several reports can be used between two grants, so the REFILL keeps W × a on its
        // own (the grant's clamp would hide a W × J bound). The credit sits at W × a − a/2
        // after a grant in the no-room case, three used reports then arrive without a grant
        // in between (each refills a = 882), and the credit must be exactly W × a: a later
        // granted frame's hold of W × a + a leaves a debt of exactly a. With a W × J bound
        // the credit would be W × a + 2.5 a and that hold would leave +1.5 a.
        const double bank_a = static_cast<double>(kUnifiedExpireAge) * kShareA0;
        UnifiedIntervalTerms terms{};
        terms.vrr_us = kShareJ;
        Driver driver = ShareDriver(0.06, terms);
        SpacedFeed feed;
        for (int i = 0; i < 70; ++i) { // saturate the bank: heavy frames, frame generation present
            const FrameResult frame = driver.Frame();
            driver.law.GrantHold(frame.grid, kShareJ);
            feed.ready += Ticks(kShareC);
            driver.law.OnReady(frame.grid, frame.key, feed.ready);
            driver.law.OnOutput(frame.grid, frame.key, feed.ready, false);
            driver.law.OnReport(frame.key, 3000.0, 13000.0, 5000.0);
        }
        FrameResult frames[5];
        for (FrameResult& frame : frames)
            frame = driver.Frame();
        // Frames 0 and 4 are granted at the full bank, before any further report.
        CHECK(Near(driver.law.GrantHold(frames[0].grid, kShareJ).credit_us, bank_a));
        CHECK(Near(driver.law.GrantHold(frames[4].grid, kShareJ).credit_us, bank_a));
        // Frame 0's hold of a/2 becomes known: the credit is W × a − a/2.
        const int64_t ready0 = feed.ready + Ticks(kShareC);
        driver.law.OnReady(frames[0].grid, frames[0].key, ready0);
        driver.law.OnOutput(frames[0].grid, frames[0].key, ready0 + Ticks(kShareA0 / 2.0), true);
        // Three used reports, no grant in between: frames 1..3 (not granted, nothing debited).
        int64_t ready = ready0;
        for (int i = 1; i <= 3; ++i) {
            ready += Ticks(kShareC);
            driver.law.OnReady(frames[i].grid, frames[i].key, ready);
            driver.law.OnOutput(frames[i].grid, frames[i].key, ready, false);
            driver.law.OnReport(frames[i].key, 3000.0, 13000.0, 5000.0);
        }
        CHECK(driver.law.Counters().reports_used >= 3);
        // Frame 4's hold of W × a + a: the credit becomes exactly −a.
        ready += Ticks(kShareC);
        driver.law.OnReady(frames[4].grid, frames[4].key, ready);
        driver.law.OnOutput(frames[4].grid, frames[4].key, ready + Ticks(bank_a + kShareA0), true);
        const FrameResult next = driver.Frame();
        const UnifiedHoldGrant grant = driver.law.GrantHold(next.grid, kShareJ);
        CHECK(grant.live);
        CHECK(Near(grant.credit_us, -kShareA0, 1.0));
        CHECK(Near(grant.advance_us, kShareA0, 1.0));
    }

    void TestHoldShareWindowMedians()
    {
        // A: the median frame-generation time of the window, lower middle, a report
        // without one counting 0. 40 frames -> the newest 32 used reports are frames 9..40.
        auto generation_for = [](int frames, const PerFrame& ai_of) {
            Driver driver = ShareDriver(0.06);
            SpacedFeed feed;
            for (int i = 0; i < frames; ++i) {
                const FrameResult frame = driver.Frame();
                feed.ready += Ticks(10000.0);
                driver.law.OnReady(frame.grid, frame.key, feed.ready);
                driver.law.OnOutput(frame.grid, frame.key, feed.ready, false);
                driver.law.OnReport(frame.key, 3000.0, 8000.0, ai_of(frame.grid));
            }
            return driver.law.Status(driver.grid).generation_us;
        };
        // Frames 9..23 (15 reports) carry none, 24..40 (17) carry 5000: sorted index 15 -> 5000.
        CHECK(Near(generation_for(40, [](uint64_t g) { return g < 24 ? 0.0 : 5000.0; }), 5000.0));
        // Frames 9..24 (16 reports) carry none, 25..40 (16) carry 5000: index 15 is still 0.
        CHECK(generation_for(40, [](uint64_t g) { return g < 25 ? 0.0 : 5000.0; }) == 0.0);
        // None at all: 0.
        CHECK(generation_for(40, [](uint64_t) { return 0.0; }) == 0.0);
        // Fewer than 16 reports in the window: 0; at 16 the median is taken.
        CHECK(generation_for(16, [](uint64_t) { return 5000.0; }) == 0.0);
        CHECK(Near(generation_for(17, [](uint64_t) { return 5000.0; }), 5000.0));
        // Non-finite and negative values count as none.
        CHECK(generation_for(40, [](uint64_t) { return -5.0; }) == 0.0);
        CHECK(generation_for(40, [](uint64_t) { return std::nan(""); }) == 0.0);

        // An unknown active time counts 0 in the median active time: with none reported
        // room is the whole J, the signed rule applies and an unknown active time
        // refills nothing, for the share 0 and the share alike.
        CHECK(ShareCredit(0.0, kShareC, 0.0) == 0.0);
        CHECK(ShareCredit(0.06, kShareC, 0.0) == 0.0);
    }

    void TestHoldShareLeavesTheStartLawAndDisplayIntervalAlone()
    {
        // The display-side interval stays the free function's value and the start law
        // stays max(base, I_gpu) with C valid and the share above 0.
        UnifiedIntervalTerms terms{};
        terms.vrr_us = 12700.0;
        auto start_for = [&terms](double share) {
            Driver driver = ShareDriver(share, terms);
            SpacedFeed feed;
            for (int i = 0; i < 40; ++i)
                feed.Frame(&driver, 14700.0, 3000.0, 0.0, 13000.0);
            const UnifiedStatus status = driver.law.Status(driver.grid);
            CHECK(status.cycle_valid == (share > 0.0));
            UnifiedStart start{};
            CHECK(driver.law.Reserve(driver.grid, 1, driver.DefaultArrival(), driver.terms, &start) ==
                UnifiedReserveStatus::Reserved);
            const double expected = std::max(UnifiedPacer_BaseInterval(terms), status.gpu_interval_us);
            CHECK(start.interval_us == expected);
            return start;
        };
        const UnifiedStart off = start_for(0.0);
        const UnifiedStart on = start_for(0.06);
        CHECK(on.interval_us == off.interval_us);
        CHECK(on.flags == off.flags);
        CHECK(Near(UnifiedPacer_DisplayInterval(terms), 12700.0));
    }

    void TestHoldShareOptionClamp()
    {
        UnifiedPacer law;
        UnifiedOptions options{};
        CHECK(options.hold_share == 0.0);
        options.hold_share = std::nan("");
        law.SetOptions(options);
        CHECK(law.Options().hold_share == 0.0);
        options.hold_share = 1.0;
        law.SetOptions(options);
        CHECK(law.Options().hold_share == 0.25);
        options.hold_share = -1.0;
        law.SetOptions(options);
        CHECK(law.Options().hold_share == 0.0);
        options.hold_share = 0.06;
        law.SetOptions(options);
        CHECK(law.Options().hold_share == 0.06);
        CHECK(kUnifiedHoldShareMax == 0.25);

        // A change shows in the next status (the cache is not stale).
        Driver toggled(CapFps(120.0), /*shipped_defaults=*/true);
        SpacedFeed f;
        for (int i = 0; i < 40; ++i)
            f.Frame(&toggled, 10000.0, 3000.0);
        CHECK(!toggled.law.Status(toggled.grid).cycle_valid);
        UnifiedOptions on{};
        on.hold_share = 0.06;
        toggled.law.SetOptions(on);
        CHECK(toggled.law.Status(toggled.grid).cycle_valid);
        toggled.law.SetOptions(UnifiedOptions{});
        CHECK(!toggled.law.Status(toggled.grid).cycle_valid);
    }

    // ── 9. Sleep-mode tuple, mailbox, anchor ──

    void TestSleepModeDesired()
    {
        UnifiedSleepModeInputs inputs{};
        inputs.game_interval_us = 16667;
        inputs.publication = 7;
        inputs.epoch = 3;

        // Key on, DMFG off: Unified, low latency off, interval 0.
        inputs.key = true;
        UnifiedSleepModeTuple tuple = UnifiedPacer_SleepModeDesired(inputs);
        CHECK(tuple.owner == UnifiedSleepOwner::Unified);
        CHECK(tuple.low_latency_off);
        CHECK(tuple.interval_us == 0);
        CHECK(tuple.publication == 7 && tuple.epoch == 3);

        // Key on, DMFG on: the game's own parameters.
        inputs.dmfg = true;
        tuple = UnifiedPacer_SleepModeDesired(inputs);
        CHECK(tuple.owner == UnifiedSleepOwner::Game);
        CHECK(!tuple.low_latency_off);
        CHECK(tuple.interval_us == 16667);
        inputs.dmfg = false;

        // Entering from DL (limiter interval) clears the interval.
        UnifiedSleepModeInputs limiter{};
        limiter.limiter_interval_us = 8333;
        limiter.driver_low_latency_off = true;
        limiter.game_interval_us = 16667;
        limiter.publication = 7;
        limiter.epoch = 2;
        const UnifiedSleepModeTuple dl = UnifiedPacer_SleepModeDesired(limiter);
        CHECK(dl.owner == UnifiedSleepOwner::Limiter);
        CHECK(dl.low_latency_off);
        CHECK(dl.interval_us == 8333);
        UnifiedSleepModeInputs entering = limiter;
        entering.key = true;
        entering.epoch = 3;
        const UnifiedSleepModeTuple unified = UnifiedPacer_SleepModeDesired(entering);
        CHECK(unified.interval_us == 0);
        CHECK(unified.low_latency_off);
        CHECK(unified != dl);
        // Even with the same epoch the tuple differs: the interval and owner moved.
        entering.epoch = 2;
        CHECK(UnifiedPacer_SleepModeDesired(entering) != dl);

        // Leaving with driver_low_latency_off true restores the game's interval.
        UnifiedSleepModeInputs leaving{};
        leaving.driver_low_latency_off = true;
        leaving.game_interval_us = 16667;
        leaving.publication = 7;
        leaving.epoch = 4;
        tuple = UnifiedPacer_SleepModeDesired(leaving);
        CHECK(tuple.owner == UnifiedSleepOwner::LowLatencyOff);
        CHECK(tuple.low_latency_off);
        CHECK(tuple.interval_us == 16667);
        CHECK(tuple != unified);

        // Leaving otherwise: the game's parameters.
        leaving.driver_low_latency_off = false;
        tuple = UnifiedPacer_SleepModeDesired(leaving);
        CHECK(tuple.owner == UnifiedSleepOwner::Game);
        CHECK(!tuple.low_latency_off);
        CHECK(tuple.interval_us == 16667);

        // The bypass state selects the override boolean.
        leaving.driver_low_latency_off = true;
        leaving.bypass = true;
        leaving.bypass_low_latency_off = false;
        CHECK(UnifiedPacer_SleepModeDesired(leaving).owner == UnifiedSleepOwner::Game);
        leaving.bypass_low_latency_off = true;
        leaving.driver_low_latency_off = false;
        CHECK(UnifiedPacer_SleepModeDesired(leaving).owner == UnifiedSleepOwner::LowLatencyOff);

        // The limiter's interval with low latency left to the game.
        limiter.driver_low_latency_off = false;
        tuple = UnifiedPacer_SleepModeDesired(limiter);
        CHECK(tuple.owner == UnifiedSleepOwner::Limiter);
        CHECK(!tuple.low_latency_off);
        CHECK(tuple.interval_us == 8333);

        // DMFG changes nothing with the key off.
        UnifiedSleepModeInputs dmfg_off{};
        dmfg_off.game_interval_us = 5000;
        UnifiedSleepModeInputs dmfg_on = dmfg_off;
        dmfg_on.dmfg = true;
        CHECK(UnifiedPacer_SleepModeDesired(dmfg_off) == UnifiedPacer_SleepModeDesired(dmfg_on));

        // Two tuples that differ only in epoch or publication are different: a
        // matching boolean alone is not a match.
        UnifiedSleepModeInputs a{};
        a.key = true;
        a.publication = 1;
        a.epoch = 1;
        UnifiedSleepModeInputs b = a;
        b.epoch = 2;
        UnifiedSleepModeInputs c = a;
        c.publication = 2;
        const UnifiedSleepModeTuple ta = UnifiedPacer_SleepModeDesired(a);
        const UnifiedSleepModeTuple tb = UnifiedPacer_SleepModeDesired(b);
        const UnifiedSleepModeTuple tc = UnifiedPacer_SleepModeDesired(c);
        CHECK(ta.owner == tb.owner && ta.low_latency_off == tb.low_latency_off && ta.interval_us == tb.interval_us);
        CHECK(ta != tb);
        CHECK(ta != tc);
        CHECK(ta == UnifiedPacer_SleepModeDesired(a));
    }

    void TestMailboxPacking()
    {
        const uint64_t word = UnifiedMailbox_Pack(0x3FFFFFFFu, UnifiedSleepOutcome::Failed, 0xFFFFFFFELL);
        CHECK(UnifiedMailbox_Token(word) == 0x3FFFFFFFu);
        CHECK(UnifiedMailbox_Outcome(word) == UnifiedSleepOutcome::Failed);
        CHECK(UnifiedMailbox_Delta(word) == 0xFFFFFFFELL);

        const UnifiedSleepOutcome outcomes[] = {UnifiedSleepOutcome::None, UnifiedSleepOutcome::Completed,
            UnifiedSleepOutcome::Late, UnifiedSleepOutcome::Failed};
        const int64_t deltas[] = {0, 1, 12345678, 0xFFFFFFFFLL, 0x80000000LL};
        const uint32_t tokens[] = {1, 2, 0x12345678u & 0x3FFFFFFFu, 0x3FFFFFFEu, 0x3FFFFFFFu};
        for (const uint32_t token : tokens) {
            for (const UnifiedSleepOutcome outcome : outcomes) {
                for (const int64_t delta : deltas) {
                    const uint64_t packed = UnifiedMailbox_Pack(token, outcome, delta);
                    CHECK(packed != 0);
                    CHECK(UnifiedMailbox_Token(packed) == token);
                    CHECK(UnifiedMailbox_Outcome(packed) == outcome);
                    CHECK(UnifiedMailbox_Delta(packed) == delta);
                }
            }
        }
        // The token is 30 bits: higher bits never leak into the outcome.
        const uint64_t masked = UnifiedMailbox_Pack(0xFFFFFFFFu, UnifiedSleepOutcome::Completed, 7);
        CHECK(UnifiedMailbox_Token(masked) == 0x3FFFFFFFu);
        CHECK(UnifiedMailbox_Outcome(masked) == UnifiedSleepOutcome::Completed);
        CHECK(UnifiedMailbox_Delta(masked) == 7);
        // A negative R − A cannot corrupt the word.
        const uint64_t negative = UnifiedMailbox_Pack(5, UnifiedSleepOutcome::Late, -10);
        CHECK(UnifiedMailbox_Token(negative) == 5);
        CHECK(UnifiedMailbox_Outcome(negative) == UnifiedSleepOutcome::Late);
        CHECK(UnifiedMailbox_Delta(negative) == 0);
        // 0 is "not published": a published word always carries a nonzero token.
        CHECK(UnifiedMailbox_Pack(1, UnifiedSleepOutcome::None, 0) != 0);
    }

    void CommitAt(UnifiedPacer* law, uint64_t n, int64_t release)
    {
        UnifiedStart start{};
        CHECK(law->PlanStartAtMarker(n, 1, release, CapFps(120.0), &start));
        uint32_t flags = 0;
        CHECK(law->CommitRelease(n, release, &flags));
    }

    void TestComputeAnchor()
    {
        // N = 1: B = R, no delta; H = R + L.
        {
            UnifiedPacer law;
            law.Reset(1, 1, kFreq);
            CommitAt(&law, 1, 1000000);
            CommitAt(&law, 2, 1083333);
            const UnifiedPacer::Anchor anchor = law.ComputeAnchor(2, 1083333, 1, 100.0);
            CHECK(anchor.b_qpc == 1083333);
            CHECK(anchor.delta_us == 0.0);
            CHECK(anchor.origin_qpc == 1083333 + 1000);
            CHECK(anchor.origin_fraction == 0.0);
        }
        // N = 2 splits exactly: B = R(n−1) + (R − R(n−1)) / 2.
        {
            UnifiedPacer law;
            law.Reset(1, 1, kFreq);
            CommitAt(&law, 1, 1000);
            CommitAt(&law, 2, 2001);
            const UnifiedPacer::Anchor anchor = law.ComputeAnchor(2, 2001, 2, 100.0);
            CHECK(anchor.b_qpc == 1500);
            CHECK(Near(anchor.delta_us, 50.05));          // (2001 − 1500.5) ticks
            CHECK(anchor.origin_qpc == 1500 + 1000);      // H = 1500.5 + 1000
            CHECK(Near(anchor.origin_fraction, 0.5));
        }
        // N = 3: remainder as a fraction of a tick.
        {
            UnifiedPacer law;
            law.Reset(1, 1, kFreq);
            CommitAt(&law, 1, 1000);
            CommitAt(&law, 2, 1010);
            const UnifiedPacer::Anchor anchor = law.ComputeAnchor(2, 1010, 3, 0.0);
            CHECK(anchor.b_qpc == 1003);
            CHECK(Near(anchor.origin_fraction, 1.0 / 3.0));
            CHECK(Near(anchor.delta_us, (10.0 - 3.0 - 1.0 / 3.0) / 10.0));
        }
        // The fraction carries into the whole part: 0.5 + 0.7 ticks = 1.2.
        {
            UnifiedPacer law;
            law.Reset(1, 1, kFreq);
            CommitAt(&law, 1, 1000);
            CommitAt(&law, 2, 2001);
            const UnifiedPacer::Anchor anchor = law.ComputeAnchor(2, 2001, 2, 0.07);
            CHECK(anchor.origin_qpc == 1501);
            CHECK(Near(anchor.origin_fraction, 0.2, 1e-9));
        }
        // No R(n−1) in the lifecycle: B = R.
        {
            UnifiedPacer law;
            law.Reset(1, 1, kFreq);
            CommitAt(&law, 1, 5000);
            const UnifiedPacer::Anchor first = law.ComputeAnchor(1, 5000, 2, 10.0);
            CHECK(first.b_qpc == 5000);
            CHECK(first.delta_us == 0.0);
            CHECK(first.origin_qpc == 5100);
            // After a reset the predecessor belongs to the old lifecycle.
            law.Reset(3, 1, kFreq);
            CommitAt(&law, 3, 9000);
            const UnifiedPacer::Anchor reset = law.ComputeAnchor(3, 9000, 2, 0.0);
            CHECK(reset.b_qpc == 9000);
            CHECK(reset.delta_us == 0.0);
            // A grid neighbour that never committed (a dropped marker) is no predecessor either.
            const UnifiedPacer::Anchor skipped = law.ComputeAnchor(5, 9500, 2, 0.0);
            CHECK(skipped.b_qpc == 9500);
        }
    }
}

int main()
{
    TestFractionalIntervalDrift();
    TestNoCatchUp();
    TestBindingTerms();
    TestGpuIntervalBindsClampsAndMedian();
    TestSaturationThresholdPairs();
    TestEveryFrameServiceNoFrameWaited();
    TestEveryFrameServiceAllWaitedIsUnchanged();
    TestEveryFrameServiceCycleInMiniature();
    TestEveryFrameServiceExcludesNonPositiveSamples();
    TestServiceCorrectionRefusedBelowActiveTime();
    TestStatusServiceSampleAndWait();
    TestQueueTargetOption();
    TestShippedDefaults();
    TestHoldShareMeasuredCycle();
    TestHoldShareOffIsTheSignedRule();
    TestHoldShareRoomKeepsTheSignedRule();
    TestHoldShareNoRoomRefillsTheAllowance();
    TestHoldShareCaseFlipsBothWays();
    TestHoldShareWindowMedians();
    TestHoldShareLeavesTheStartLawAndDisplayIntervalAlone();
    TestHoldShareOptionClamp();
    TestHoldShareGrantClampsWhenTheCaseIsEntered();
    TestHoldShareRefillBoundHoldsBetweenGrants();
    TestPairsFollowProducerFrameIds();
    TestHoldSubtractionAndSignedReport();
    TestServiceSampleSubtractsPredecessorHold();
    TestBindabilityAndStaleness();
    TestPairWindowCountsReportedFrames();
    TestHardLimitBlocksAndRetires();
    TestHardLimitOneAbsoluteDeadline();
    TestHardLimitFourDeepPipeline();
    TestHardLimitLateCompletionPublication();
    TestHardLimitStaleCompletions();
    TestHardLimitEvaluatedAtTarget();
    TestHardLimitBacklogWaitsOneInterval();
    TestHardLimitNoCompletionEver();
    TestLimitInputsAndOutstanding();
    TestResolveCompletedAndLate();
    TestResolveFailedRunningSuperseded();
    TestResolveMissingDuplicateAndEarlyWord();
    TestResets();
    TestUsedReportStream();
    TestQueueWaitLive();
    TestDisplayInterval();
    TestHoldCreditStartsAtZero();
    TestHoldCreditBanksAtMostWindowTimesJ();
    TestHoldCreditDebtAndAdvance();
    TestHoldCreditGrantedFrameWithoutHoldDebitsNothing();
    TestHoldCreditStreamNotLive();
    TestHoldCreditResets();
    TestHoldCreditBoundsFollowJ();
    TestHoldCreditRefillsOncePerReport();
    TestHoldCreditRepeatedGrantIsStored();
    TestServiceUsableDefinition();
    TestSleepModeDesired();
    TestMailboxPacking();
    TestComputeAnchor();

    if (s_failures != 0) {
        std::fprintf(stderr, "%d unified pacer test failure(s)\n", s_failures);
        return 1;
    }
    std::printf("unified pacer tests passed\n");
    return 0;
}
