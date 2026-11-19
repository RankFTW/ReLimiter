// Directly tests the production pure queue law of the unified pacer (src/unified_queue_law.cpp),
// the probe's feed ring (src/gpu_queue_law_feed.cpp) and the law's integration in the production
// UnifiedPacer (src/unified_pacer.cpp): samples from records, the window statistics and the
// fixed points of P, the fallback rule, one credit state under two rules, the key-off identity,
// the interval of a reservation. The probe's records on a live queue, the scheduler's drain and
// pacing quality still need live validation; the scripted records are a model.

#include "gpu_queue_law_feed.h"
#include "pacing_telemetry.h"
#include "unified_pacer.h"
#include "unified_queue_law.h"

#include <cmath>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <tuple>
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

    constexpr int64_t kFreq = 10000000; // 10 MHz QPC
    constexpr int64_t kStart = 123456789012345LL;
    constexpr double kJ = 10000.0;
    constexpr double kBank = static_cast<double>(kUnifiedExpireAge) * kJ;

    bool Near(double a, double b, double eps = 1e-6)
    {
        return std::fabs(a - b) <= eps;
    }

    int64_t Ticks(double us)
    {
        return static_cast<int64_t>(std::llround(us * static_cast<double>(kFreq) / 1e6));
    }

    PacingPlanKey Key(uint64_t frame_id)
    {
        PacingPlanKey key{};
        key.identity_domain = PacingIdentityDomain::NvApiFrame;
        key.producer_epoch = 1;
        key.controller_generation = 1;
        key.frame_id = frame_id;
        return key;
    }

    // A probe record with every law input valid (margin_free optional).
    PacingGpuQueueRecord Record(double cycle, double empty, double margin_free, double hold = 0.0,
        double boundary_idle = 0.0, double wait_idle = 0.0, bool with_margin = true)
    {
        PacingGpuQueueRecord record{};
        record.valid = PacingGpuQueueRecord::kCycle | PacingGpuQueueRecord::kEmpty |
            PacingGpuQueueRecord::kBoundaryIdle | PacingGpuQueueRecord::kHoldBlock |
            PacingGpuQueueRecord::kWaitIdle | (with_margin ? PacingGpuQueueRecord::kMinMarginFree : 0u);
        record.cycle_us = cycle;
        record.empty_us = empty;
        record.min_margin_free_us = with_margin ? margin_free : 0.0;
        record.hold_block_us = hold;
        record.boundary_idle_us = boundary_idle;
        record.wait_idle_us = wait_idle;
        return record;
    }

    // A record of the real frame `frame_id` (valid key, NvApi domain, producer epoch 1), measured
    // in probe session `session` (0 = unknown source).
    PacingGpuQueueRecord Keyed(PacingGpuQueueRecord record, uint64_t frame_id, uint64_t session = 0)
    {
        record.key.domain = PacingIdentityDomain::NvApiFrame;
        record.key.producer_epoch = 1;
        record.key.frame_id = frame_id;
        record.session_generation = session;
        return record;
    }

    PacingGpuQueueRecord Session(PacingGpuQueueRecord record, uint64_t session)
    {
        record.session_generation = session;
        return record;
    }

    // The record names the law grant its hold block is the debit of (law_hold_sequence, 0 = none).
    PacingGpuQueueRecord Named(PacingGpuQueueRecord record, uint64_t sequence)
    {
        record.law_hold_sequence = sequence;
        return record;
    }

    // The law's two funding terms: the cap's slack and the share.
    double Funding(const UnifiedQueueResult& r)
    {
        return r.refill_slack_us + r.refill_share_us;
    }

    UnifiedQueuePosition Position(uint64_t frame_id, uint32_t epoch = 1,
        PacingIdentityDomain domain = PacingIdentityDomain::NvApiFrame)
    {
        UnifiedQueuePosition position{};
        position.valid = true;
        position.domain = domain;
        position.producer_epoch = epoch;
        position.frame_id = frame_id;
        return position;
    }
    // A sample whose s' is s: a positive s is the margin, a negative one the starve.
    UnifiedQueueSample SampleS(double s, double cycle = 10000.0)
    {
        return s > 0.0 ? UnifiedQueueSample_FromRecord(Record(cycle, 0.0, s))
                       : UnifiedQueueSample_FromRecord(Record(cycle, -s, 0.0));
    }

    // need_frames defaults to 16 here: the window tests pin the 16-frame mean cycle as the base
    // (the law's own default is 4); the tests of the need mean pass it explicitly.
    UnifiedQueueParams Params(int rank = 2, double allow_us = 4000.0, double share = 0.0, int need_frames = 16)
    {
        UnifiedQueueParams params{};
        params.rank = rank;
        params.allow_us = allow_us;
        params.hold_share = share;
        params.need_frames = need_frames;
        return params;
    }

    void PushRepeated(UnifiedQueueWindow* window, const UnifiedQueueSample& sample, int count)
    {
        for (int i = 0; i < count; ++i)
            window->Push(sample);
    }

    void TestWindowCacheReplay()
    {
        const auto fields = [](const UnifiedQueueResult& r) {
            return std::make_tuple(r.computable, r.in_use, r.limited, r.guard_active, r.bound_active,
                r.valid_frames, r.base_us, r.need_us, r.service_us, r.q_us, r.p_us, r.guard_us,
                r.gpu_interval_us, r.refill_slack_us, r.refill_share_us);
        };
        UnifiedQueueWindow warm, unread;
        bool arrivals[UnifiedQueueWindow::kWindow]{};
        for (unsigned frame = 1; frame <= 128; ++frame) {
            if (frame == 75) {
                warm.Reset(); unread.Reset();
                for (bool& valid : arrivals) valid = false;
            }
            auto sample = SampleS(static_cast<int>(frame * 193 % 7001) - 2000, 8000.0 + frame * 23);
            sample.valid = frame % 19 < 16;
            const auto measured = frame % 7 ? Position(frame) : UnifiedQueuePosition{};
            warm.Push(sample, measured);
            unread.Push(sample, measured);
            arrivals[frame % UnifiedQueueWindow::kWindow] = sample.valid;
            uint32_t valid = 0;
            for (bool arrived : arrivals) valid += arrived;
            CHECK(warm.ValidArrivals() == valid);
            for (unsigned age = 0; age <= 6; ++age) {
                const auto planned = age % 2 ? Position(frame + age) : UnifiedQueuePosition{};
                // Change a single cache key at a time, then repeat the final query.
                for (unsigned query = 0; query <= 6; ++query) {
                    const auto params = Params(query >= 2 ? 4 : 2, query >= 3 ? 900.0 : 4000.0,
                        query >= 4 ? 0.1 : 0.0, query >= 5 ? 4 : 16);
                    const double j = query ? 8500.0 : 10900.0;
                    auto cold = unread; // never queried: the same window, always measured from scratch
                    CHECK(fields(warm.Compute(j, params, planned)) == fields(cold.Compute(j, params, planned)));
                }
                warm.NotePlanned();
                unread.NotePlanned();
            }
        }
    }

    // ── Samples ──

    void TestSampleFromRecord()
    {
        // starve = empty + boundary idle + wait idle; s' = -starve; N = cycle - hold - boundary idle - wait idle;
        // lost = starve + hold, all of it kept until a window judges it.
        const UnifiedQueueSample starved = UnifiedQueueSample_FromRecord(Record(10000.0, 300.0, 999.0, 1000.0, 200.0, 500.0));
        CHECK(starved.valid);
        CHECK(Near(starved.starve_us, 1000.0) && Near(starved.s_prime_us, -1000.0));
        CHECK(Near(starved.need_us, 8300.0) && Near(starved.service_us, 8000.0) && Near(starved.cycle_us, 10000.0));
        CHECK(Near(starved.hold_block_us, 1000.0) && Near(starved.boundary_idle_us, 200.0) &&
            Near(starved.wait_idle_us, 500.0) && Near(starved.margin_free_us, 999.0));
        CHECK(Near(starved.lost_us, 2000.0) && Near(starved.kept_starve_us, 1000.0) && Near(starved.kept_hold_us, 1000.0));

        // No starve: the margin without the hold's time; without its bit the record is no sample.
        const UnifiedQueueSample fed = UnifiedQueueSample_FromRecord(Record(10000.0, 0.0, 700.0));
        CHECK(fed.valid && Near(fed.s_prime_us, 700.0) && fed.starve_us == 0.0);
        CHECK(!UnifiedQueueSample_FromRecord(Record(10000.0, 0.0, 700.0, 0.0, 0.0, 0.0, false)).valid);
        // A starved frame needs no margin.
        const UnifiedQueueSample no_margin = UnifiedQueueSample_FromRecord(Record(10000.0, 50.0, 0.0, 0.0, 0.0, 0.0, false));
        CHECK(no_margin.valid && Near(no_margin.s_prime_us, -50.0));
        // The boundary idle alone is a starve, and so is the wait idle alone (a Wait pending with nothing behind it).
        CHECK(Near(UnifiedQueueSample_FromRecord(Record(10000.0, 0.0, 0.0, 0.0, 120.0, 0.0, false)).s_prime_us, -120.0));
        CHECK(Near(UnifiedQueueSample_FromRecord(Record(10000.0, 0.0, 0.0, 0.0, 0.0, 150.0, false)).s_prime_us, -150.0));

        // Every one of the five bits is required.
        const uint32_t bits[] = {PacingGpuQueueRecord::kCycle, PacingGpuQueueRecord::kEmpty,
            PacingGpuQueueRecord::kBoundaryIdle, PacingGpuQueueRecord::kHoldBlock, PacingGpuQueueRecord::kWaitIdle};
        for (const uint32_t bit : bits) {
            PacingGpuQueueRecord record = Record(10000.0, 100.0, 700.0);
            record.valid &= ~bit;
            CHECK(!UnifiedQueueSample_FromRecord(record).valid);
        }

        // N never goes below 0.
        CHECK(UnifiedQueueSample_FromRecord(Record(1000.0, 0.0, 10.0, 5000.0)).need_us == 0.0);
        // A value that is not finite or is negative is no sample.
        CHECK(!UnifiedQueueSample_FromRecord(Record(std::nan(""), 0.0, 10.0)).valid);
        CHECK(!UnifiedQueueSample_FromRecord(Record(10000.0, -1.0, 10.0)).valid);
        CHECK(!UnifiedQueueSample_FromRecord(Record(10000.0, 0.0, 10.0, std::nan(""))).valid);
    }

    // ── Window statistics ──

    UnifiedQueueWindow RampWindow() // margins 100, 200, ... 1600, oldest first
    {
        UnifiedQueueWindow window;
        for (int i = 1; i <= 16; ++i)
            window.Push(SampleS(100.0 * i));
        return window;
    }

    void TestOrderStatisticsMedianAndGuard()
    {
        const UnifiedQueueWindow window = RampWindow();
        // rank 2 of 16: 200; the median is the lower middle value: index 7 = 800.
        const UnifiedQueueResult base = window.Compute(5000.0, Params(2, 4000.0));
        CHECK(base.computable && base.in_use && base.valid_frames == 16);
        CHECK(Near(base.base_us, 10000.0) && Near(base.q_us, 200.0) && base.guard_us == 0.0);
        CHECK(Near(base.p_us, 200.0 / 32.0) && Near(base.gpu_interval_us, 10000.0 + 200.0 / 32.0));
        // Median - M binds when M is small: q = max(200, 800 - 400); the newest four (1300..1600)
        // exceed 2M = 800 by 500 at the minimum: guard = 500/8.
        const UnifiedQueueResult small_m = window.Compute(5000.0, Params(2, 400.0));
        CHECK(Near(small_m.q_us, 400.0) && Near(small_m.guard_us, 62.5) && small_m.guard_active);
        CHECK(Near(small_m.p_us, 400.0 / 32.0 + 62.5));
        const UnifiedQueueResult no_m = window.Compute(5000.0, Params(2, 0.0));
        CHECK(Near(no_m.q_us, 800.0) && Near(no_m.guard_us, 1300.0 / 8.0));
        // The rank selects the order statistic and is clamped to the window's size.
        CHECK(Near(window.Compute(5000.0, Params(1)).q_us, 100.0));
        CHECK(Near(window.Compute(5000.0, Params(8)).q_us, 800.0));
        CHECK(Near(window.Compute(5000.0, Params(99)).q_us, 1600.0));
        CHECK(Near(window.Compute(5000.0, Params(0)).q_us, 100.0)); // below 1: the smallest
        // A window of four valid samples: rank 8 is the 4th smallest; three are not computable.
        UnifiedQueueWindow four;
        for (int i = 1; i <= 3; ++i)
            four.Push(SampleS(100.0 * i));
        CHECK(!four.Compute(5000.0, Params()).computable && !four.Compute(5000.0, Params()).in_use);
        four.Push(SampleS(400.0));
        const UnifiedQueueResult small = four.Compute(5000.0, Params(8, 0.0));
        CHECK(small.computable && Near(small.q_us, 400.0));
        // The statistics are over the newest 16 valid samples: older ones are out.
        UnifiedQueueWindow rolled = window;
        PushRepeated(&rolled, SampleS(50.0), 16);
        CHECK(Near(rolled.Compute(5000.0, Params(1)).q_us, 50.0) && Near(rolled.Compute(5000.0, Params(16)).q_us, 50.0));
        // B is the mean cycle and N^ the mean need.
        UnifiedQueueWindow means;
        for (int i = 0; i < 8; ++i)
            means.Push(UnifiedQueueSample_FromRecord(Record(10000.0, 0.0, 100.0, 1000.0)));
        for (int i = 0; i < 8; ++i)
            means.Push(UnifiedQueueSample_FromRecord(Record(12000.0, 0.0, 100.0, 0.0, 0.0, 2000.0)));
        const UnifiedQueueResult mean = means.Compute(5000.0, Params());
        CHECK(Near(mean.base_us, 11000.0) && Near(mean.need_us, (9000.0 + 10000.0) / 2.0));
    }

    void TestPositivePartBoundAndClamp()
    {
        // Margins far above 2M: raw = q/32 + guard = 3125 + 11500.
        UnifiedQueueWindow fed;
        PushRepeated(&fed, SampleS(100000.0), 16);
        // No limit from J (unknown): the clamp B/8.
        UnifiedQueueResult none = fed.Compute(0.0, Params());
        CHECK(Near(none.p_us, 1250.0) && !none.limited && none.guard_active);
        // J close below B: the deficit B - J bounds P and is the binding limit.
        UnifiedQueueResult just_below = fed.Compute(9500.0, Params());
        CHECK(Near(just_below.p_us, 500.0) && just_below.limited);
        // J far below B: the clamp binds, not the deficit.
        UnifiedQueueResult well_below = fed.Compute(2000.0, Params());
        CHECK(Near(well_below.p_us, 1250.0) && !well_below.limited);
        // A cap at or above B: the positive part is 0 and that bound is the limit: I_gpu <= B.
        for (const double j : {10000.0, 12000.0}) {
            const UnifiedQueueResult capped = fed.Compute(j, Params());
            CHECK(capped.p_us == 0.0 && capped.limited && Near(capped.gpu_interval_us, 10000.0));
        }
        // A negative P is never limited by J and is clamped to -B/8.
        UnifiedQueueWindow starved;
        PushRepeated(&starved, SampleS(-80000.0), 16);
        const UnifiedQueueResult deep = starved.Compute(12000.0, Params());
        CHECK(Near(deep.q_us, -80000.0) && Near(deep.p_us, -1250.0) && !deep.limited && !deep.guard_active);
        UnifiedQueueWindow mild;
        PushRepeated(&mild, SampleS(-800.0), 16);
        const UnifiedQueueResult small = mild.Compute(12000.0, Params());
        CHECK(Near(small.p_us, -25.0) && Near(small.gpu_interval_us, 9975.0));
    }

    // ── Fixed points ──

    void TestGpuBoundPositiveMarginShrinks()
    {
        // No cap (J = a 144 Hz display's interval), a GPU cycle of 16 ms and a small positive
        // margin: the interval is above the base and the plant (the margin shrinks by P) settles
        // the margin down without ever making it a starve.
        UnifiedQueueWindow window;
        double margin = 1000.0;
        PushRepeated(&window, SampleS(margin, 16000.0), 16);
        double previous = margin;
        for (int frame = 0; frame < 40; ++frame) {
            const UnifiedQueueResult law = window.Compute(6944.0, Params());
            CHECK(law.in_use && law.p_us > 0.0 && law.gpu_interval_us > law.base_us);
            margin -= law.p_us;
            CHECK(margin < previous && margin > 0.0);
            previous = margin;
            window.Push(SampleS(margin, 16000.0));
        }
        CHECK(margin < 800.0);
    }

    void TestStarvedIntervalBelowBase()
    {
        // The queue sat empty before every frame: the interval is below the base, and the plant
        // (the starve shrinks by -P) walks it back toward feeding.
        UnifiedQueueWindow window;
        double starve = 900.0;
        PushRepeated(&window, SampleS(-starve, 16000.0), 16);
        double previous = starve;
        for (int frame = 0; frame < 10; ++frame) {
            const UnifiedQueueResult law = window.Compute(6944.0, Params());
            CHECK(law.p_us < 0.0 && law.gpu_interval_us < law.base_us && !law.limited);
            starve += law.p_us; // P < 0
            CHECK(starve < previous);
            previous = starve;
            window.Push(SampleS(-starve, 16000.0));
        }
    }

    void TestHeldCapWithSlack()
    {
        // Cap 20 ms; the GPU needs 16 ms and every 20 ms cycle has 4 ms of wait idle (a Wait pending with
        // nothing queued behind it): that is the cap's slack, starvation and not need. B is the need, q the
        // starvation, I_gpu stays below the cap with no limit flag, and the credit is refilled by the slack
        // J - S^ = 4 ms per record.
        UnifiedQueueWindow window;
        PushRepeated(&window, UnifiedQueueSample_FromRecord(Record(20000.0, 0.0, 3000.0, 0.0, 0.0, 4000.0)), 16);
        const UnifiedQueueResult law = window.Compute(20000.0, Params());
        CHECK(law.in_use && Near(law.q_us, -4000.0) && Near(law.p_us, -4000.0 / 32.0));
        CHECK(Near(law.base_us, 20000.0) && law.gpu_interval_us < 20000.0 && !law.limited); // F = 16: the mean cycle
        CHECK(Near(window.Compute(20000.0, Params(2, 4000.0, 0.0, 4)).base_us, 16000.0));     // F = 4: the need
        CHECK(Near(law.need_us, 16000.0) && Near(Funding(law), 4000.0));
        // A cap the GPU's need reaches (B = J) with a positive margin: P's positive part is 0, I_gpu = the
        // cap and the limit flag is set.
        UnifiedQueueWindow reached;
        PushRepeated(&reached, UnifiedQueueSample_FromRecord(Record(20000.0, 0.0, 3000.0)), 16);
        const UnifiedQueueResult held = reached.Compute(20000.0, Params(2, 4000.0, 0.0, 4));
        CHECK(held.in_use && held.q_us > 0.0);
        CHECK(held.p_us == 0.0 && Near(held.gpu_interval_us, 20000.0) && held.limited);
        // The same cap with a starved queue (1 ms empty and the 4 ms of wait idle): a negative P, I_gpu <= J,
        // no limit flag.
        UnifiedQueueWindow starved;
        PushRepeated(&starved, UnifiedQueueSample_FromRecord(Record(20000.0, 1000.0, 0.0, 0.0, 0.0, 4000.0)), 16);
        const UnifiedQueueResult below = starved.Compute(20000.0, Params());
        CHECK(Near(below.p_us, -5000.0 / 32.0) && below.gpu_interval_us < 20000.0 && !below.limited);
    }

    void TestCapRemovedIntervalFallsEveryFrame()
    {
        // A 20 ms cap held the cycle; with the cap removed the cycles are 16 ms: the mean cycle,
        // and so I_gpu, falls by 4 ms / 16 on every arrival for a full window.
        UnifiedQueueWindow window;
        PushRepeated(&window, SampleS(1500.0, 20000.0), 16);
        double previous = window.Compute(6944.0, Params()).gpu_interval_us;
        for (int frame = 0; frame < 16; ++frame) {
            window.Push(SampleS(1500.0, 16000.0));
            const double now = window.Compute(6944.0, Params()).gpu_interval_us;
            CHECK(now < previous && Near(previous - now, 250.0));
            previous = now;
        }
        CHECK(Near(previous, 16000.0 + 1500.0 / 32.0));
    }

    void TestLongerCycleEngagesTheGuard()
    {
        // A 20% longer cycle in one step: the queue backs up and the margins jump above 2M = 8 ms.
        // The guard engages when ALL the newest four margins exceed 2M (the smallest decides).
        UnifiedQueueWindow window;
        PushRepeated(&window, SampleS(2000.0, 16000.0), 16);
        CHECK(!window.Compute(8000.0, Params()).guard_active);
        for (int step = 1; step <= 3; ++step) {
            window.Push(SampleS(12000.0, 19200.0));
            const UnifiedQueueResult law = window.Compute(8000.0, Params());
            CHECK(!law.guard_active && law.guard_us == 0.0);
        }
        window.Push(SampleS(12000.0, 19200.0));
        const UnifiedQueueResult law = window.Compute(8000.0, Params());
        CHECK(law.guard_active && Near(law.guard_us, (12000.0 - 8000.0) / 8.0));
        CHECK(Near(law.base_us, (12.0 * 16000.0 + 4.0 * 19200.0) / 16.0));
        CHECK(Near(law.q_us, 2000.0) && Near(law.p_us, 2000.0 / 32.0 + 500.0));
        // M moves the engagement point: with M = 6 ms the same margins stay below 2M.
        CHECK(!window.Compute(8000.0, Params(2, 6000.0)).guard_active);
    }

    void TestSigmaAllowance()
    {
        const auto refill = [](double need, double share) {
            UnifiedQueueWindow window;
            PushRepeated(&window, SampleS(100.0, need), 16);
            return Funding(window.Compute(kJ, Params(2, 4000.0, share)));
        };
        // N^ <= J: the slack J - N^, and sigma adds nothing.
        CHECK(Near(refill(10000.0, 0.05), 0.0) && Near(refill(10000.0, 0.0), 0.0));
        CHECK(Near(refill(9000.0, 0.0), 1000.0) && Near(refill(9000.0, 0.05), 1000.0));
        // N^ = S^ far above J: sigma * S^ (sigma 0: nothing).
        CHECK(Near(refill(20000.0, 0.05), 1000.0) && Near(refill(20000.0, 0.0), 0.0));
        // Between: min(sigma * N^, N^ - J), continuous through J and through the crossing.
        CHECK(Near(refill(10200.0, 0.05), 200.0) && Near(refill(10600.0, 0.05), 530.0));
        CHECK(Near(refill(10001.0, 0.05), 1.0) && Near(refill(9999.0, 0.05), 1.0));
        const double crossing = kJ / (1.0 - 0.05);
        CHECK(Near(refill(crossing, 0.05), 0.05 * crossing, 1e-6));
        CHECK(Near(refill(crossing - 1.0, 0.05), 0.05 * crossing - 1.0, 1e-6)); // slope 1 below
        CHECK(Near(refill(crossing + 1.0, 0.05), 0.05 * crossing + 0.05, 1e-6)); // slope sigma above
    }

    void TestAllowanceUsesStarvationFreeService()
    {
        const auto compute = [](const PacingGpuQueueRecord& record, double share) {
            UnifiedQueueWindow window;
            PushRepeated(&window, UnifiedQueueSample_FromRecord(record), 16);
            return window.Compute(kJ, Params(2, 4000.0, share));
        };
        // S = cycle - hold - boundary idle - wait idle - empty; N does not subtract the empty time.
        const UnifiedQueueSample sample = UnifiedQueueSample_FromRecord(Record(14000.0, 500.0, 0.0, 2000.0, 300.0, 1000.0, false));
        CHECK(Near(sample.need_us, 10700.0) && Near(sample.service_us, 10200.0));
        CHECK(UnifiedQueueSample_FromRecord(Record(1000.0, 0.0, 10.0, 5000.0)).service_us == 0.0);
        // Starved inside the frames: N^ = 12 ms is above J = 10 ms only because the queue sat empty
        // 3 ms of each frame; S^ = 9 ms <= J: the slack is J - S^ = 1 ms (not N^'s nothing) and no
        // allowance (sigma applies to S^ above J only).
        const UnifiedQueueResult starved = compute(Record(12000.0, 3000.0, 0.0, 0.0, 0.0, 0.0, false), 0.05);
        CHECK(Near(starved.need_us, 12000.0) && Near(starved.service_us, 9000.0));
        CHECK(Near(Funding(starved), 1000.0));
        // The same cycle fed (no empty time): S^ = N^ = 12 ms above J: the allowance min(sigma S^, S^ - J).
        const UnifiedQueueResult fed = compute(Record(12000.0, 0.0, 800.0), 0.05);
        CHECK(Near(fed.service_us, 12000.0) && Near(Funding(fed), 600.0));
        // Wait idle and hold block are out of both.
        CHECK(Near(Funding(compute(Record(14000.0, 0.0, 100.0, 2000.0, 0.0, 1000.0), 0.05)), 550.0));
        // Starvation that still leaves S^ above J: the allowance is against S^ (11 ms), the slack
        // against N^ (12 ms): min(0.05 * 11000, 1000) = 550.
        CHECK(Near(Funding(compute(Record(12000.0, 1000.0, 0.0, 0.0, 0.0, 0.0, false), 0.05)), 550.0));
    }

    void TestHoldStepAtSaturationIsPaidOnlyByTheShare()
    {
        // No cap (J = a 144 Hz display's interval, below the GPU's 16 ms: no slack). Holds of 0.5 ms per frame
        // start between two frames: the cycle grows by 0.5 ms and the hold block is 0.5 ms; the margin without the
        // hold and the need do not move. The backlog is I_gpu above the hold-free need; steps its change per arrival.
        const auto run = [](int need_frames, double share, std::vector<double>* steps) {
            UnifiedQueueWindow window;
            PushRepeated(&window, SampleS(1500.0, 16000.0), 16);
            const auto backlog = [&] {
                const UnifiedQueueResult law = window.Compute(6944.0, Params(2, 4000.0, share, need_frames));
                CHECK(Near(law.need_us, 16000.0)); // the hold-free need, constant
                return law.gpu_interval_us - law.need_us;
            };
            const double before = backlog();
            CHECK(Near(before, 1500.0 / 32.0));
            double previous = before;
            for (int arrival = 1; arrival <= 40; ++arrival) {
                window.Push(UnifiedQueueSample_FromRecord(Record(16500.0, 0.0, 1500.0, 500.0)));
                const double now = backlog();
                steps->push_back(now - previous);
                previous = now;
            }
            return previous - before;
        };
        const auto expect = [](const std::vector<double>& steps, int from, int to, double step) {
            for (int i = from; i < to; ++i)
                CHECK(Near(steps[static_cast<size_t>(i)], step, 1e-9));
        };
        // F = 16, the first build's base (the mean cycle): it grows 0.5 ms / 16 an arrival for 16 arrivals, then holds.
        std::vector<double> first;
        CHECK(Near(run(16, 0.0, &first), 500.0));
        expect(first, 0, 16, 500.0 / 16.0);
        expect(first, 16, 40, 0.0);
        // F = 4, share 0: while the held frames are a minority of the window (8 of 16 at most) their block is a
        // transient and stays (125 us an arrival for 4 arrivals); from the 9th on it is the level most frames are
        // blocked at, the interval's own excess, and leaves the base as those frames enter the F-frame mean. After
        // 12 arrivals the interval is back at the need plus P: the hold is not paid by a longer interval.
        std::vector<double> unfunded;
        CHECK(Near(run(4, 0.0, &unfunded), 0.0, 1e-9));
        expect(unfunded, 0, 4, 125.0);
        expect(unfunded, 4, 8, 0.0);
        expect(unfunded, 8, 12, -125.0);
        expect(unfunded, 12, 40, 0.0);
        // F = 4, share 1/32 (Smooth: sigma * S^ = 0.5 ms): the share funds the common block, which stays: 0.5 ms in all.
        std::vector<double> funded;
        CHECK(Near(run(4, 1.0 / 32.0, &funded), 500.0));
        expect(funded, 0, 4, 125.0);
        expect(funded, 4, 40, 0.0);
    }

    // A valid sample of a frame the GPU term paced (or not). A paced frame's start wait is as long as its
    // starvation unless the test says otherwise: the wait then caused all of it.
    UnifiedQueueSample PacedSample(const PacingGpuQueueRecord& record, bool paced, double wait_us = -1.0)
    {
        UnifiedQueueSample sample = UnifiedQueueSample_FromRecord(record);
        sample.paced = paced;
        sample.paced_wait_us = !paced ? 0.0 : wait_us >= 0.0 ? wait_us : sample.starve_us;
        return sample;
    }

    // The base counts a paced frame's starvation only as far as its start wait can have caused it. Rogue Core
    // with the game and render threads in series (N.107): the GPU term paced every frame at 7.96 ms over a need
    // of 7.7 ms, one frame was held for 1.05 ms and then simulated for 43 ms, and its queue sat empty for
    // 39.85 ms. Counted whole, that frame raised the base by 39.85 / 16 = 2.49 ms and the interval with it.
    // The 0.26 ms every frame starved is the level most frames lose, the interval's own excess: it leaves the
    // base, which is the need, 7.7 ms; the spike's starvation beyond that level counts up to its start wait.
    void TestPacedStarvationCountsOnlyUpToTheStartWait()
    {
        const auto steady = [] { return PacedSample(Record(7960.0, 260.0, 0.0, 0.0, 0.0, 0.0, false), true); };
        UnifiedQueueWindow window;
        for (int i = 0; i < 40; ++i)
            window.Push(steady());
        const double before = window.Compute(7692.0, Params(2, 4000.0, 0.0, 4)).base_us;
        CHECK(Near(before, 7700.0));
        // The spike frame, then two ordinary ones (the need's four frames then hold one spike frame).
        window.Push(PacedSample(Record(47350.0, 39850.0, 0.0, 0.0, 0.0, 0.0, false), true, 1050.0));
        window.Push(steady());
        window.Push(steady());
        const UnifiedQueueResult bounded = window.Compute(7692.0, Params(2, 4000.0, 0.0, 4));
        // need 7.5 ms from the spike frame and 7.7 ms from the others; counted starvation 1.05 ms (the spike's
        // 39.59 ms beyond the common 0.26 ms, bounded by its wait) and none from the others.
        const double need = (7500.0 + 3.0 * 7700.0) / 4.0;
        CHECK(Near(bounded.base_us, need + 1050.0 / 16.0));
        CHECK(bounded.base_us - before <= 1050.0 / 16.0 + 1e-9);
        // The same frame with a start wait as long as its starvation is the pacer's own doing and counts whole
        // beyond the common level.
        UnifiedQueueWindow whole;
        for (int i = 0; i < 40; ++i)
            whole.Push(steady());
        whole.Push(PacedSample(Record(47350.0, 39850.0, 0.0, 0.0, 0.0, 0.0, false), true));
        whole.Push(steady());
        whole.Push(steady());
        CHECK(Near(whole.Compute(7692.0, Params(2, 4000.0, 0.0, 4)).base_us, need + (39850.0 - 260.0) / 16.0));
        // A paced frame that was not held at all counts nothing.
        UnifiedQueueWindow unheld;
        for (int i = 0; i < 40; ++i)
            unheld.Push(PacedSample(Record(7960.0, 260.0, 0.0, 0.0, 0.0, 0.0, false), true, 0.0));
        CHECK(Near(unheld.Compute(7692.0, Params(2, 4000.0, 0.0, 4)).base_us, 7700.0));
    }


    void TestUnpacedStarvationIsNotInTheBase()
    {
        // A cap of 20 ms held over a need of 16.4 ms: every frame starved by 3.6 ms and the CAP set
        // the starts (not paced): B is the need, not the cap's interval, and I_gpu stays below it.
        UnifiedQueueWindow window;
        for (int i = 0; i < 40; ++i)
            window.Push(PacedSample(Record(20000.0, 3600.0, 0.0, 0.0, 0.0, 0.0, false), false));
        const UnifiedQueueResult held = window.Compute(20000.0, Params(2, 4000.0, 0.0, 4));
        CHECK(Near(held.base_us, 16400.0) && held.gpu_interval_us < 20000.0 && !held.limited);
        // The same frames GPU-paced: the starvation every frame shares is the interval's own excess, not the
        // GPU's need: B is the need too (before the level was taken out it echoed the 20 ms for good).
        UnifiedQueueWindow paced;
        for (int i = 0; i < 40; ++i)
            paced.Push(PacedSample(Record(20000.0, 3600.0, 0.0, 0.0, 0.0, 0.0, false), true));
        CHECK(Near(paced.Compute(20000.0, Params(2, 4000.0, 0.0, 4)).base_us, 16400.0));
        // F = 16 never gates: the 16-frame mean cycle, whoever paced the frames.
        CHECK(Near(window.Compute(20000.0, Params(2, 4000.0, 0.0, 16)).base_us, 20000.0));
    }

    // The cap is removed from the window above (the GPU term sets the starts from now on): the law's
    // interval is the next frames' cycle. Returns I after `records` records.
    double RunCapRemoved(int need_frames, int records)
    {
        constexpr double kNeedUs = 16400.0;
        UnifiedQueueWindow window;
        for (int i = 0; i < 40; ++i)
            window.Push(PacedSample(Record(20000.0, 3600.0, 0.0, 0.0, 0.0, 0.0, false), false));
        double interval = window.Compute(6944.0, Params(2, 4000.0, 0.0, need_frames)).gpu_interval_us;
        for (int record = 1; record <= records; ++record) {
            const double cycle = interval > kNeedUs ? interval : kNeedUs; // the interval binds
            window.Push(PacedSample(Record(cycle, cycle - kNeedUs, 500.0), true));
            interval = window.Compute(6944.0, Params(2, 4000.0, 0.0, need_frames)).gpu_interval_us;
        }
        return interval;
    }

    void TestCapRemovedReachesTheNeedWithinFFrames()
    {
        // F = 4: the interval is at the need (within 150 us) within F + 2 records, since the cap's
        // starvation was never in B. F = 16 (the 16-frame mean cycle) is still at the cap's level.
        CHECK(RunCapRemoved(4, 4 + 2) <= 16400.0 + 150.0);
        CHECK(RunCapRemoved(16, 4 + 2) > 16400.0 + 1000.0);
        CHECK(RunCapRemoved(16, 16) > 16400.0 + 150.0);
    }

    // A fall of the need by 1.5 ms while the frames are GPU-paced: the interval is the cycle, so
    // the frames are starved by what the need fell. Returns the number of records after which the
    // law's interval is within 0.5 ms of the new need (0 = not within `limit`).
    int RunPacedNeedFall(int need_frames, int limit, double* at_f_plus_2)
    {
        constexpr double kNewNeedUs = 14500.0;
        UnifiedQueueWindow window;
        for (int i = 0; i < 40; ++i)
            window.Push(PacedSample(Record(16000.0, 0.0, 500.0), true));
        int reached = 0;
        double interval = 16000.0;
        for (int record = 1; record <= limit; ++record) {
            const double cycle = interval > kNewNeedUs ? interval : kNewNeedUs;
            window.Push(PacedSample(Record(cycle, cycle - kNewNeedUs, 500.0), true));
            interval = window.Compute(6944.0, Params(2, 4000.0, 0.0, need_frames)).gpu_interval_us;
            if (reached == 0 && interval <= kNewNeedUs + 500.0)
                reached = record;
            if (record == need_frames + 2 && at_f_plus_2)
                *at_f_plus_2 = interval;
        }
        return reached;
    }

    void TestPacedNeedFallIsFollowedWithinFFrames()
    {
        double fast_at = 0.0;
        const int fast = RunPacedNeedFall(4, 40, &fast_at);
        CHECK(fast > 0 && fast <= 4 + 2 && fast_at <= 14500.0 + 500.0);
        // F = 16, the 16-frame mean cycle: the old level is kept, only the small P brings it down.
        double slow_at = 0.0;
        const int slow = RunPacedNeedFall(16, 40, &slow_at);
        CHECK(slow == 0 || slow > 16);
        std::printf("need fall 1.5 ms (GPU-paced): F = 4 within 0.5 ms of the new need after %d records, F = 16 after %d (0 = not in 40)\n",
            fast, slow);
    }

    // The interval's excess, closed loop (Metal Eden, N.176: frame generation, no cap the GPU can reach, the GPU
    // term pacing every frame with a start wait of 11.7 ms). The window holds 16 fed frames of a 15.4 ms need; the
    // need then falls to 12.2 ms and stays. Frame n is planned at I(n), the law's interval from the records that
    // have arrived (`delay` frames late). The queue works 12.2 ms a frame: a frame planned above the need and the
    // backlog leaves the queue without work for the excess x = I - need - backlog, which shows in one face: empty
    // time inside the frame, idle time before its first submission (boundary idle), a Wait pending with nothing
    // queued behind it (wait idle), or the next frame's work blocked behind the frame generation input's hold
    // (hold block, no feeding margin); Alternating takes the gap and the hold in turns. A frame planned below
    // the need and the backlog grows the backlog, its feeding margin. Returns I(n) for n = 1..records and the
    // largest backlog.
    enum class ExcessFace { Empty, Gap, WaitIdle, Hold, Alternating };
    std::vector<double> RunExcess(ExcessFace face, int delay, int records, double* largest_backlog)
    {
        constexpr double kOldNeedUs = 15400.0;
        constexpr double kNeedUs = 12200.0;
        constexpr double kStartWaitUs = 11700.0;
        constexpr double kCeilingUs = 6944.0; // the display's ceiling, far below the need
        UnifiedQueueWindow window;
        for (int i = 0; i < 16; ++i)
            window.Push(PacedSample(Record(kOldNeedUs, 0.0, 500.0), true, kStartWaitUs));
        std::vector<PacingGpuQueueRecord> pending; // records of planned frames that have not arrived, oldest first
        std::vector<double> intervals;
        double backlog = 0.0;
        *largest_backlog = 0.0;
        for (int n = 1; n <= records; ++n) {
            while (static_cast<int>(pending.size()) > delay) {
                window.Push(PacedSample(pending.front(), true, kStartWaitUs));
                pending.erase(pending.begin());
            }
            const double interval = window.Compute(kCeilingUs, Params(2, 4000.0, 0.0, 4)).gpu_interval_us;
            intervals.push_back(interval);
            const double excess = std::max(0.0, interval - kNeedUs - backlog);
            backlog = std::max(0.0, backlog + kNeedUs - interval);
            *largest_backlog = std::max(*largest_backlog, backlog);
            const double cycle = kNeedUs + excess;
            const bool hold = face == ExcessFace::Hold || (face == ExcessFace::Alternating && n % 2 == 0);
            if (!(excess > 0.0))
                pending.push_back(Record(cycle, 0.0, backlog));                    // fed: the backlog is its margin
            else if (hold)
                pending.push_back(Record(cycle, 0.0, 0.0, excess));                // blocked behind the hold
            else if (face == ExcessFace::WaitIdle)
                pending.push_back(Record(cycle, 0.0, 0.0, 0.0, 0.0, excess));      // a Wait, nothing behind it
            else if (face == ExcessFace::Empty)
                pending.push_back(Record(cycle, excess, 0.0));                     // empty inside the frame
            else
                pending.push_back(Record(cycle, 0.0, 0.0, 0.0, excess));           // idle before its first submission
        }
        return intervals;
    }

    // Whichever face the excess takes, the interval leaves it: within 32 records it is within 0.1 ms of the need,
    // and from then on no frame is planned more than 0.1 ms above it nor 0.5 ms below it, with the queue's
    // backlog under 1 ms. A base that kept the excess held the interval near 15.4 ms (wait idle, hold) or let only
    // P bring it down, x / 32 a frame (empty, gap).
    void TestIntervalExcessLeavesWhicheverFaceItTakes()
    {
        constexpr double kNeedUs = 12200.0;
        const char* names[] = {"empty", "gap", "wait idle", "hold", "alternating"};
        for (const ExcessFace face :
            {ExcessFace::Empty, ExcessFace::Gap, ExcessFace::WaitIdle, ExcessFace::Hold, ExcessFace::Alternating}) {
            for (const int delay : {0, 2}) {
                double largest_backlog = 0.0;
                const std::vector<double> intervals = RunExcess(face, delay, 96, &largest_backlog);
                double high = intervals[31];
                double low = intervals[31];
                for (size_t i = 31; i < intervals.size(); ++i) {
                    high = std::max(high, intervals[i]);
                    low = std::min(low, intervals[i]);
                }
                CHECK(high <= kNeedUs + 100.0 && low >= kNeedUs - 500.0 && largest_backlog < 1000.0);
                std::printf("excess as %s, records %d late: I after 8 / 16 / 32 records %.0f / %.0f / %.0f us, "
                            "then %.0f to %.0f us, largest backlog %.0f us (need %.0f us)\n",
                    names[static_cast<int>(face)], delay, intervals[7], intervals[15], intervals[31], low, high,
                    largest_backlog, kNeedUs);
            }
        }
    }

    void TestNeedMeanIsUnbiasedWhenGpuPaced()
    {
        // GPU-paced frames, intermittent starvation (one frame in four, and a richer pattern) and
        // a varying need, all periodic with period 4: over whole periods the mean of B equals the
        // mean cycle for every F (the starvation is kept at its 16-frame mean in B, the need at its
        // F-frame mean: neither is dropped nor counted twice).
        const double need[4] = {15000.0, 16000.0, 17000.0, 16000.0};
        const double patterns[2][4] = {{0.0, 0.0, 2000.0, 0.0}, {0.0, 0.0, 2000.0, 400.0}};
        for (const auto& starve : patterns) {
            for (const int need_frames : {1, 3, 4, 16}) {
                for (const bool paced : {true, false}) {
                    UnifiedQueueWindow window;
                    const auto record = [&](int k) {
                        return PacedSample(Record(need[k % 4] + starve[k % 4], starve[k % 4], 500.0), paced);
                    };
                    for (int k = 0; k < 16; ++k)
                        window.Push(record(k));
                    double base_sum = 0.0;
                    double cycle_sum = 0.0;
                    double need_sum = 0.0;
                    constexpr int kRun = 4000;
                    for (int k = 16; k < 16 + kRun; ++k) {
                        window.Push(record(k));
                        base_sum += window.Compute(6944.0, Params(2, 4000.0, 0.0, need_frames)).base_us;
                        cycle_sum += need[k % 4] + starve[k % 4];
                        need_sum += need[k % 4];
                    }
                    // GPU-paced (or F = 16): no bias. Not paced with F < 16: the starvation is not B's
                    // to carry: B is the mean need.
                    const double expected = (paced || need_frames == 16) ? cycle_sum : need_sum;
                    CHECK(Near(base_sum / kRun, expected / kRun, 1e-6));
                }
            }
        }
    }

    void TestNeedFramesSixteenIsTheMeanCycle()
    {
        // Reference: the first build's base, the mean cycle of the newest 16 valid samples (fewer:
        // all), whoever paced the frames. F < 16: the mean F-term of the newest F plus the mean over the
        // newest 16 of the kept starvation of the GPU-paced frames, each sample's kept parts judged at its
        // arrival against the median loss of the window it entered (no cap here: no slack, no share, so the
        // F-term is cycle - starve - hold block + the kept hold block).
        uint32_t state = 20260101u;
        const auto next = [&] {
            state = state * 1664525u + 1013904223u;
            return static_cast<double>(state >> 8);
        };
        UnifiedQueueWindow window;
        std::vector<double> cycles;
        std::vector<double> losts;      // starve + hold block
        std::vector<double> terms;      // the F-term
        std::vector<double> paced_kept; // the kept starvation of a paced frame (its start wait is its starvation), else 0
        for (int k = 0; k < 200; ++k) {
            const double cycle = 12000.0 + std::fmod(next(), 6000.0);
            const double empty = std::fmod(next(), 3.0) < 1.0 ? std::fmod(next(), 2500.0) : 0.0;
            const double idle = std::fmod(next(), 5.0) < 1.0 ? std::fmod(next(), 300.0) : 0.0;
            const bool paced = std::fmod(next(), 2.0) < 1.0;
            const double hold = std::fmod(next(), 800.0);
            const double wait_idle = std::fmod(next(), 500.0);
            window.Push(PacedSample(Record(cycle, empty, 300.0, hold, idle, wait_idle), paced));
            const double starve = empty + idle + wait_idle;
            cycles.push_back(cycle);
            losts.push_back(starve + hold);
            const size_t count = cycles.size() < 16 ? cycles.size() : 16;
            std::vector<double> entered;
            for (size_t i = 0; i < count; ++i)
                entered.push_back(losts[losts.size() - 1 - i]);
            std::sort(entered.begin(), entered.end());
            const double level = entered[(count - 1) / 2];
            double kept_s = starve;
            double kept_h = hold;
            if (level > 0.0) {
                const double kept = std::max(0.0, starve + hold - level);
                kept_s = starve + hold > 0.0 ? kept * (starve / (starve + hold)) : 0.0;
                kept_h = std::max(0.0, kept - kept_s);
            }
            terms.push_back(cycle - starve - hold + kept_h);
            paced_kept.push_back(paced ? kept_s : 0.0);
            if (count < UnifiedQueueWindow::kMinValidInWindow)
                continue;
            double mean_cycle = 0.0;
            double mean_paced_kept = 0.0;
            for (size_t i = 0; i < count; ++i) {
                mean_cycle += cycles[cycles.size() - 1 - i];
                mean_paced_kept += paced_kept[paced_kept.size() - 1 - i];
            }
            mean_cycle /= static_cast<double>(count);
            mean_paced_kept /= static_cast<double>(count);
            CHECK(Near(window.Compute(6944.0, Params(2, 4000.0, 0.0, 16)).base_us, mean_cycle, 1e-8));
            CHECK(Near(window.Compute(6944.0, Params(2, 4000.0, 0.0, 99)).base_us, mean_cycle, 1e-8)); // clamped to the window
            // F = 1 (and 0, clamped up): the newest F-term plus the gated kept starvation mean.
            CHECK(Near(window.Compute(6944.0, Params(2, 4000.0, 0.0, 1)).base_us, terms.back() + mean_paced_kept, 1e-8));
            CHECK(Near(window.Compute(6944.0, Params(2, 4000.0, 0.0, 0)).base_us, terms.back() + mean_paced_kept, 1e-8));
            // F = 4: the mean F-term of the newest (up to 4) samples plus the gated kept starvation mean.
            const size_t f4 = count < 4 ? count : 4;
            double term4 = 0.0;
            for (size_t i = 0; i < f4; ++i)
                term4 += terms[terms.size() - 1 - i];
            CHECK(Near(window.Compute(6944.0, Params(2, 4000.0, 0.0, 4)).base_us,
                term4 / static_cast<double>(f4) + mean_paced_kept, 1e-8));
        }
    }

    void TestHeldCapRefillUsesTheService()
    {
        // Cap 20 ms, the GPU's service 16.4 ms: the 3.6 ms of slack is inside-frame empty time on
        // every frame. The credit's slack is J - S^ = 3.6 ms per frame (N^ counts that empty time
        // as need and is 20 ms: it would give nothing); I_gpu stays below the cap.
        UnifiedQueueWindow window;
        for (int i = 0; i < 40; ++i)
            window.Push(PacedSample(Record(20000.0, 3600.0, 0.0, 0.0, 0.0, 0.0, false), false));
        const UnifiedQueueResult law = window.Compute(20000.0, Params(2, 4000.0, 0.0, 4));
        CHECK(Near(law.service_us, 16400.0) && Near(law.need_us, 20000.0));
        CHECK(Near(Funding(law), 3600.0) && law.gpu_interval_us < 20000.0);
        // sigma only adds above J: nothing here.
        CHECK(Near(Funding(window.Compute(20000.0, Params(2, 4000.0, 0.05, 4))), 3600.0));
    }

    void TestPacedEntriesGateTheStarvationInTheBase();
    void TestGameLimitedFramesNeverBindTheGpuTerm();

    void TestInUseRules()
    {
        UnifiedQueueWindow window;
        CHECK(!window.Compute(kJ, Params()).computable && !window.InUse() && window.ValidArrivals() == 0);
        // 12 valid arrivals are needed: 11 are not enough.
        for (int i = 0; i < 11; ++i)
            window.Push(SampleS(500.0));
        CHECK(!window.InUse() && window.Compute(kJ, Params()).computable && !window.Compute(kJ, Params()).in_use);
        window.Push(SampleS(500.0));
        CHECK(window.InUse() && window.Compute(kJ, Params()).valid_frames == 12);
        // An arrival without a valid sample counts against the 16: four invalid arrivals keep 12
        // valid among the newest 16, the fifth pushes the oldest valid one out.
        for (int i = 0; i < 4; ++i)
            window.Push(UnifiedQueueSample{});
        CHECK(window.InUse() && window.ValidArrivals() == 12);
        window.Push(UnifiedQueueSample{});
        CHECK(!window.InUse() && window.ValidArrivals() == 11 && window.Compute(kJ, Params()).valid_frames == 11);
        // The statistics still describe the 12 valid samples (the window is not the arrivals).
        CHECK(window.Compute(kJ, Params()).computable && window.WindowCount() == 12);
        // Valid arrivals push the invalid ones out of the newest 16 and restore it.
        PushRepeated(&window, SampleS(500.0), 16);
        CHECK(window.InUse() && window.ValidArrivals() == 16);
        // Age: at most 4 planned frames since the newest valid arrival.
        for (int i = 0; i < 4; ++i)
            window.NotePlanned();
        CHECK(window.InUse());
        window.NotePlanned();
        CHECK(!window.InUse() && !window.Compute(kJ, Params()).in_use && window.Compute(kJ, Params()).computable);
        window.Push(SampleS(500.0));
        CHECK(window.InUse());
        window.Reset();
        CHECK(!window.InUse() && !window.Compute(kJ, Params()).computable && window.WindowCount() == 0 &&
            window.ValidArrivals() == 0);
    }

    // ── The law in the production pacer ──

    // One pacer fed like the owner: Plan = Status + GrantHold of the next index, Evidence = the
    // frame's completion, output and report. A report refills the credit by J - active.
    struct Rig {
        UnifiedPacer pacer;
        uint64_t grid = 0;
        int64_t ready = kStart;
        struct Planned {
            uint64_t grid = 0;
            PacingPlanKey key{};
        };

        // need_frames defaults to 16: the credit tests pin the 16-frame mean cycle as the base.
        explicit Rig(bool law, double hold_share = 0.0, int rank = 2, double allow_us = 4000.0, int need_frames = 16)
        {
            pacer.Reset(1, 1, kFreq);
            UnifiedOptions options{};
            options.queue_law = law;
            options.hold_share = hold_share;
            options.queue_rank = rank;
            options.queue_allow_us = allow_us;
            options.queue_need_frames = need_frames;
            pacer.SetOptions(options);
        }

        UnifiedHoldGrant Plan(Planned* out = nullptr, double j = kJ, bool paced = false)
        {
            ++grid;
            Planned frame{grid, Key(grid)};
            pacer.OnPlan(frame.grid, frame.key, kStart, paced);
            last_status = pacer.Status(frame.grid, j);
            const UnifiedHoldGrant grant = pacer.GrantHold(frame.grid, j);
            if (out)
                *out = frame;
            return grant;
        }

        void Evidence(const Planned& frame, double active_us, double hold_us = 0.0, double ai_frame_us = 0.0)
        {
            ready += Ticks(10000.0);
            pacer.OnReady(frame.grid, frame.key, ready);
            pacer.OnOutput(frame.grid, frame.key, ready + Ticks(hold_us), hold_us > 0.0);
            pacer.OnReport(frame.key, 3000.0, active_us, ai_frame_us);
        }

        void Records(const PacingGpuQueueRecord& record, int count)
        {
            for (int i = 0; i < count; ++i)
                pacer.OnQueueRecord(record);
        }

        UnifiedStatus last_status{};
    };

    void TestCreditContinuousAcrossRuleSwitches()
    {
        Rig rig(true);
        Rig::Planned frame;
        // The report-based rule: each used report refills J - active = 4000.
        for (int i = 0; i < 10; ++i) {
            rig.Plan(&frame);
            rig.Evidence(frame, 6000.0);
        }
        CHECK(Near(rig.Plan(&frame).credit_us, 9 * 4000.0)); // 9 used reports (the first has no predecessor)
        rig.Evidence(frame, 6000.0);                          // +4000: the law has no record yet
        // 16 records, cycle 8 ms (N = 8 ms): the law is in use from the 12th; each refills J - N^ =
        // 2 ms. The credit continues from where the report rule left it: nothing jumps or resets.
        rig.Records(Record(8000.0, 0.0, 500.0), 11);
        CHECK(!rig.pacer.QueueLawInUse());
        rig.Records(Record(8000.0, 0.0, 500.0), 5);
        CHECK(rig.pacer.QueueLawInUse());
        CHECK(Near(rig.Plan(&frame).credit_us, 40000.0 + 5 * 2000.0));
        CHECK((rig.last_status.flags & kUpQueueLaw) != 0 && rig.last_status.bindable);
        // Reports no longer refill while the law is in use (the old rule would add 4000 each).
        rig.Evidence(frame, 6000.0);
        CHECK(Near(rig.Plan(&frame).credit_us, 50000.0)); // plan 13: age 2
        rig.Evidence(frame, 6000.0);
        CHECK(Near(rig.Plan(&frame).credit_us, 50000.0)); // plan 14: age 3
        rig.Evidence(frame, 6000.0);
        CHECK(Near(rig.Plan(&frame).credit_us, 50000.0)); // plan 15: age 4, still in use
        CHECK((rig.last_status.flags & kUpQueueLaw) != 0);
        rig.Evidence(frame, 6000.0);
        // No record for 5 planned frames: the report-based rule is back, from the same credit.
        CHECK(Near(rig.Plan(&frame).credit_us, 50000.0));
        CHECK((rig.last_status.flags & kUpQueueLaw) == 0 && !rig.pacer.QueueLawInUse());
        rig.Evidence(frame, 6000.0);
        CHECK(Near(rig.Plan(&frame).credit_us, 54000.0));
        // Records come back: the window kept its samples and the law is in use again at the
        // first fresh one, refilling from the same credit.
        rig.Records(Record(8000.0, 0.0, 500.0), 1);
        CHECK(rig.pacer.QueueLawInUse());
        CHECK(Near(rig.Plan(&frame).credit_us, 56000.0));
    }

    void TestLawHoldSpendsComeFromRecords()
    {
        // Under the law a frame's own hold no longer spends the credit (the record's hold block
        // does); under the report rule it does.
        Rig rig(true);
        Rig::Planned frame;
        for (int i = 0; i < 4; ++i) {
            rig.Plan(&frame);
            rig.Evidence(frame, 10000.0); // active = J: the report rule refills nothing
        }
        rig.Records(Record(8000.0, 0.0, 500.0), 12);
        CHECK(rig.pacer.QueueLawInUse());
        const UnifiedHoldGrant before = rig.Plan(&frame);
        CHECK(before.live && Near(before.credit_us, 2000.0)); // the 12th record only
        rig.Evidence(frame, 10000.0, 3000.0);                   // a 3 ms hold on a granted frame
        CHECK(Near(rig.Plan(&frame).credit_us, 2000.0));
        // The report rule (no records): the same hold spends the credit.
        Rig plain(false);
        for (int i = 0; i < 4; ++i) {
            plain.Plan(&frame);
            plain.Evidence(frame, 6000.0);
        }
        const double credit = plain.Plan(&frame).credit_us;
        CHECK(Near(credit, 3 * 4000.0));
        plain.Evidence(frame, 6000.0, 3000.0); // spends 3000, then the report refills 4000
        CHECK(Near(plain.Plan(&frame).credit_us, credit - 3000.0 + 4000.0));
    }

    void TestRecordDebitAndBounds()
    {
        // The hold block of each arrived record that NAMES a law grant (its sequence) debits that grant beyond the cap's
        // slack max(0, J - S^): a hold within the slack fills time the GPU would idle at the cap and spends nothing. The
        // refill is J - S^ (S^ the mean service of the window including this record).
        Rig rig(true);
        Rig::Planned frame;
        rig.Plan(&frame); // J is known from the first grant
        rig.Records(Record(8000.0, 0.0, 500.0), 12);
        const UnifiedHoldGrant first_grant = rig.Plan(&frame);
        CHECK(Near(first_grant.credit_us, 2000.0) && first_grant.law_sequence != 0);
        rig.pacer.OnQueueRecord(Named(Record(8000.0, 0.0, 500.0, 1500.0), first_grant.law_sequence)); // service 6500
        const double service = (12.0 * 8000.0 + 6500.0) / 13.0; // slack kJ - service = 2115: the 1500 hold is inside it
        const UnifiedHoldGrant second_grant = rig.Plan(&frame);
        CHECK(Near(second_grant.credit_us, 2000.0 + (kJ - service), 1e-6));
        // A hold beyond the slack debits its excess.
        rig.pacer.OnQueueRecord(Named(Record(8000.0, 0.0, 500.0, 5000.0), second_grant.law_sequence)); // service 3000
        const double slack = kJ - (12.0 * 8000.0 + 6500.0 + 3000.0) / 14.0;
        const UnifiedHoldGrant third_grant = rig.Plan(&frame);
        CHECK(Near(third_grant.credit_us, second_grant.credit_us + slack - (5000.0 - slack), 1e-6));
        // Debt is bounded by -J.
        rig.pacer.OnQueueRecord(Named(Record(8000.0, 0.0, 500.0, 50000.0), third_grant.law_sequence));
        CHECK(Near(rig.Plan(&frame).credit_us, -kJ));
        // The bank is W x J (the report rule's no-room bank W x a does not apply under the law).
        Rig bank(true, 0.2); // sigma 0.2: the report rule would use a bank of W x a with room < a
        bank.Plan(&frame);
        bank.Records(Record(1000.0, 0.0, 500.0), 60); // N^ = 1000: refill 9000 each
        CHECK(Near(bank.Plan(&frame).credit_us, kBank));
    }

    // A window whose base is not usable (Compute's computable false) has no service to measure the slack by: its records
    // refill nothing (J - S^ with the unset S^ = 0 would add J per record) and a law grant's hold block is debited whole.
    void TestUncomputableWindowRefillsNothingAndDebitsTheWholeHold()
    {
        Rig rig(true);
        Rig::Planned frame;
        rig.Plan(&frame);
        rig.Records(Record(8000.0, 0.0, 500.0), 12);
        const UnifiedHoldGrant grant = rig.Plan(&frame);
        CHECK(grant.law_sequence != 0);
        const PacingGpuQueueRecord zero_cycle = Record(0.0, 0.0, 500.0); // valid; 16 of them make B = 0
        rig.Records(zero_cycle, 16);
        CHECK(rig.pacer.QueueLawInUse());
        const double before = rig.Plan(&frame).credit_us;
        rig.Records(zero_cycle, 3);
        CHECK(Near(rig.Plan(&frame).credit_us, before));
        rig.pacer.OnQueueRecord(Named(Record(0.0, 0.0, 500.0, 1500.0), grant.law_sequence));
        CHECK(Near(rig.Plan(&frame).credit_us, before - 1500.0));
    }

    // The share part of the law's refill, min(sigma * S^, max(0, S^ - J)), funds holds that frame generation overlaps: the pacer
    // bounds it by the median frame generation time A of the used reports (the bound of the report-based rule). A at least the
    // share term leaves the law's own refill; A = 0 (no frame generation: reports carry none) funds nothing, and the signed
    // J - S^ applies (a GPU that cannot reach J drains the credit by its deficit); the slack part max(0, J - S^) is never bounded.
    void TestLawShareRefillIsBoundedByFrameGenerationTime()
    {
        constexpr double kSigma = 1.0 / 32.0;
        // 14 ms frames against a 10 ms interval: S^ = 14 ms, no slack, share term min(sigma * 14 ms, 4 ms) = 437.5 us.
        const auto credit_gained_by_records = [](double generation_us, int records) {
            Rig rig(true, kSigma);
            Rig::Planned frame;
            for (int i = 0; i < 20; ++i) { // enough used reports for the window's medians (kUnifiedMinPairs)
                rig.Plan(&frame);
                rig.Evidence(frame, kJ, 0.0, generation_us); // active = J: the report rule's own refill is its allowance only
            }
            const double before = rig.Plan(&frame).credit_us;
            rig.Records(Record(14000.0, 0.0, 500.0), records);
            CHECK(rig.pacer.QueueLawInUse());
            return rig.Plan(&frame).credit_us - before;
        };
        // The law is in use from the 12th record: 16 records refill five times.
        CHECK(Near(credit_gained_by_records(0.0, 16), -kJ));                 // no frame generation: 4 ms deficits drain to -J
        CHECK(Near(credit_gained_by_records(300.0, 16), 5 * 300.0));         // A below the share term bounds it
        CHECK(Near(credit_gained_by_records(437.5, 16), 5 * 437.5));         // A equal to it: the law's own refill
        CHECK(Near(credit_gained_by_records(1700.0, 16), 5 * 437.5, 1e-9));  // A above it (the measured sizes): unchanged
        // The slack part is untouched by A: 8 ms frames leave 2 ms of slack each time whatever the report says.
        for (const double generation_us : {0.0, 300.0, 2000.0}) {
            Rig rig(true, kSigma);
            Rig::Planned frame;
            for (int i = 0; i < 20; ++i) {
                rig.Plan(&frame);
                rig.Evidence(frame, kJ, 0.0, generation_us);
            }
            const double before = rig.Plan(&frame).credit_us;
            rig.Records(Record(8000.0, 0.0, 500.0), 16);
            CHECK(Near(rig.Plan(&frame).credit_us - before, 5 * 2000.0));
        }
        // With the share 0 (the default: the bound is not evaluated) the deficit drains the credit to -J likewise.
        {
            Rig rig(true, 0.0);
            Rig::Planned frame;
            rig.Plan(&frame);
            rig.Records(Record(14000.0, 0.0, 500.0), 13);
            CHECK(Near(rig.Plan(&frame).credit_us, -2 * 4000.0)); // in use from the 12th record: two deficits of 4 ms
            rig.Records(Record(14000.0, 0.0, 500.0), 3);
            const UnifiedHoldGrant floor = rig.Plan(&frame);
            CHECK(Near(floor.credit_us, -kJ) && floor.saturated); // R4: released at once
        }
    }

    // The frame generation time A that bounds the share refill counts only while the report stream is live at the planned
    // index (the rule of Status and the credit's bank: QueueWaitLive). Queue records keep arriving while the reports stop:
    // once the reports are expired A is 0, the share stops refilling (the status reports the evidence expired) and the
    // signed J - S^ drains the credit by the GPU's deficit; when reports return the share refills again.
    void TestShareRefillStopsWhenTheReportsExpire()
    {
        constexpr double kSigma = 1.0 / 32.0;
        constexpr double kShareTerm = 437.5; // 14 ms frames against J = 10 ms: min(sigma * 14 ms, 4 ms)
        const PacingGpuQueueRecord record = Record(14000.0, 0.0, 500.0);
        Rig rig(true, kSigma);
        Rig::Planned frame;
        for (int i = 0; i < 20; ++i) {
            rig.Plan(&frame);
            rig.Evidence(frame, kJ, 0.0, 1700.0); // A = 1.7 ms, above the share term
        }
        rig.Plan(&frame);
        rig.Records(record, 16);
        CHECK(rig.pacer.QueueLawInUse());
        double credit = rig.Plan(&frame).credit_us;
        rig.Records(record, 1);
        CHECK(Near(rig.Plan(&frame).credit_us - credit, kShareTerm)); // live reports: the law's own refill

        // The reports stop; the records go on, one per planned frame. While the newest used report is no older than
        // kUnifiedExpireAge frames the share still refills; beyond it the 4 ms deficit drains the credit, to -J.
        for (uint32_t i = 0; i < kUnifiedExpireAge + 2; ++i) {
            rig.Plan(&frame);
            rig.Records(record, 1);
        }
        rig.Plan(&frame);
        CHECK((rig.last_status.flags & kUpExpired) != 0 && rig.pacer.QueueLawInUse());
        credit = rig.Plan(&frame).credit_us;
        for (int i = 0; i < 5; ++i) {
            rig.Records(record, 1);
            const double drained = std::max(-kJ, credit - 4000.0);
            CHECK(Near(rig.Plan(&frame).credit_us, drained)); // no share refill: the deficit S^ - J
            credit = drained;
        }

        // Reports return: the share refills again.
        for (int i = 0; i < 3; ++i) {
            rig.Plan(&frame);
            rig.Evidence(frame, kJ, 0.0, 1700.0);
            rig.Records(record, 1);
        }
        credit = rig.Plan(&frame).credit_us;
        CHECK((rig.last_status.flags & kUpExpired) == 0);
        rig.Records(record, 1);
        CHECK(Near(rig.Plan(&frame).credit_us - credit, kShareTerm));
    }

    // A live decrease of the hold share drops the banked surplus: credit = min(credit, 0). A debt, the outstanding grants and
    // their later debits stay; an increase and a rank change reset nothing.
    void TestShareDecreaseDropsTheBankedSurplus()
    {
        const auto with_share = [](UnifiedPacer* pacer, double share, int rank = 2) {
            UnifiedOptions options = pacer->Options();
            options.hold_share = share;
            options.queue_rank = rank;
            pacer->SetOptions(options);
        };
        // A positive credit: 16 records of 8 ms leave 2 ms of slack each, five refills after the law is in use.
        const auto banked = [](Rig* rig, Rig::Planned* frame) {
            rig->Plan(frame);
            rig->Records(Record(8000.0, 0.0, 500.0), 16);
            CHECK(rig->pacer.QueueLawInUse());
        };
        Rig::Planned frame;
        {
            Rig rig(true, 1.0 / 32.0);
            banked(&rig, &frame);
            CHECK(Near(rig.Plan(&frame).credit_us, 5 * 2000.0));
            with_share(&rig.pacer, 1.0 / 32.0, 4); // a rank change alone: nothing resets
            CHECK(Near(rig.Plan(&frame).credit_us, 5 * 2000.0));
            with_share(&rig.pacer, 0.0625, 4);     // an increase: nothing resets
            CHECK(Near(rig.Plan(&frame).credit_us, 5 * 2000.0));
            with_share(&rig.pacer, 0.0625, 4);     // the same share again: nothing resets
            CHECK(Near(rig.Plan(&frame).credit_us, 5 * 2000.0));
            with_share(&rig.pacer, 1.0 / 32.0, 4); // a decrease: the surplus is gone
            CHECK(Near(rig.Plan(&frame).credit_us, 0.0));
            rig.Records(Record(8000.0, 0.0, 500.0), 1); // the credit refills from 0 afterwards
            CHECK(Near(rig.Plan(&frame).credit_us, 2000.0));
        }
        {
            Rig rig(true, 1.0 / 32.0); // to 0: the share is off
            banked(&rig, &frame);
            with_share(&rig.pacer, 0.0);
            CHECK(Near(rig.Plan(&frame).credit_us, 0.0));
        }
        {
            // A debt stays: the decrease only caps a positive credit at 0.
            Rig rig(true, 1.0 / 32.0);
            banked(&rig, &frame);
            const UnifiedHoldGrant grant = rig.Plan(&frame);
            CHECK(grant.law_sequence != 0);
            rig.pacer.OnQueueRecord(Named(Record(8000.0, 0.0, 500.0, 50000.0), grant.law_sequence));
            CHECK(Near(rig.Plan(&frame).credit_us, -kJ));
            with_share(&rig.pacer, 0.0);
            CHECK(Near(rig.Plan(&frame).credit_us, -kJ));
        }
        {
            // An outstanding grant is still debited by its record after the decrease, from the credit that remains.
            Rig rig(true, 1.0 / 32.0);
            banked(&rig, &frame);
            const UnifiedHoldGrant grant = rig.Plan(&frame);
            CHECK(grant.law_sequence != 0 && Near(grant.credit_us, 5 * 2000.0));
            with_share(&rig.pacer, 0.0);
            rig.pacer.OnQueueRecord(Named(Record(8000.0, 0.0, 500.0, 3000.0), grant.law_sequence)); // service 5000
            // From 0 after the decrease: the record refills the slack J - S^ and the 3 ms hold is debited beyond it, so the
            // credit is small, never the 10 ms banked before.
            const double slack = kJ - (15.0 * 8000.0 + 5000.0) / 16.0;
            CHECK(Near(rig.Plan(&frame).credit_us, slack - (3000.0 - slack), 1e-6));
        }
    }

    void TestLawLivesWithoutReports()
    {
        // The credit's stream is the probe's records while the law is in use: no report is needed
        // for the grant to be live; without them or the law it is not.
        Rig rig(true);
        Rig::Planned frame;
        CHECK(!rig.Plan(&frame).live);
        rig.Records(Record(8000.0, 0.0, 500.0), 14);
        const UnifiedHoldGrant grant = rig.Plan(&frame);
        CHECK(grant.live && Near(grant.credit_us, 3 * 2000.0));
        Rig off(false);
        off.Records(Record(8000.0, 0.0, 500.0), 14);
        CHECK(!off.Plan(&frame).live);
    }

    // Reports and completions for 40 frames: the report-based I_gpu is valid (service 10 ms, P = 100 us).
    void RunReportFrames(Rig* rig, int frames, std::vector<UnifiedHoldGrant>* grants = nullptr,
        std::vector<UnifiedStatus>* statuses = nullptr)
    {
        Rig::Planned frame;
        for (int i = 0; i < frames; ++i) {
            const UnifiedHoldGrant grant = rig->Plan(&frame);
            if (grants)
                grants->push_back(grant);
            if (statuses)
                statuses->push_back(rig->last_status);
            rig->Evidence(frame, 6000.0, i % 5 == 3 ? 1500.0 : 0.0);
        }
    }

    void TestKeyOffAndNotInUseAreIdentical()
    {
        // The reference: the law off, no records.
        Rig reference(false);
        std::vector<UnifiedHoldGrant> reference_grants;
        std::vector<UnifiedStatus> reference_statuses;
        RunReportFrames(&reference, 40, &reference_grants, &reference_statuses);
        CHECK(reference_statuses.back().bindable && reference_statuses.back().gpu_interval_us > 0.0);

        // Key off with records offered: the window stays empty, everything as the reference.
        Rig off(false);
        off.Records(Record(8000.0, 0.0, 500.0), 40);
        std::vector<UnifiedHoldGrant> off_grants;
        std::vector<UnifiedStatus> off_statuses;
        RunReportFrames(&off, 40, &off_grants, &off_statuses);
        CHECK(!off.pacer.QueueLawInUse());
        for (size_t i = 0; i < off_grants.size(); ++i) {
            CHECK(off_grants[i].live == reference_grants[i].live && off_grants[i].credit_us == reference_grants[i].credit_us);
            CHECK(off_statuses[i].gpu_interval_us == reference_statuses[i].gpu_interval_us);
            CHECK(off_statuses[i].flags == reference_statuses[i].flags && off_statuses[i].bindable == reference_statuses[i].bindable);
            CHECK(!off_statuses[i].ql_valid && off_statuses[i].ql_valid_frames == -1);
        }

        // Key on but fewer than 12 valid arrivals: the report rule decides, bit for bit.
        Rig few(true);
        few.Records(Record(8000.0, 0.0, 500.0), 5);
        std::vector<UnifiedHoldGrant> few_grants;
        std::vector<UnifiedStatus> few_statuses;
        RunReportFrames(&few, 40, &few_grants, &few_statuses);
        CHECK(!few.pacer.QueueLawInUse());
        for (size_t i = 0; i < few_grants.size(); ++i) {
            CHECK(few_grants[i].live == reference_grants[i].live && few_grants[i].credit_us == reference_grants[i].credit_us);
            CHECK(few_statuses[i].gpu_interval_us == reference_statuses[i].gpu_interval_us);
            CHECK(few_statuses[i].flags == reference_statuses[i].flags && few_statuses[i].bindable == reference_statuses[i].bindable);
            CHECK(few_statuses[i].ql_valid_frames == 5 && few_statuses[i].ql_valid); // computable, not in use
        }

        // In use: the law's I_gpu replaces the report-based one.
        Rig on(true);
        on.Records(Record(8000.0, 0.0, 500.0), 16);
        Rig::Planned frame;
        on.Plan(&frame, 5000.0);
        CHECK(on.last_status.ql_valid && on.last_status.ql_valid_frames == 16);
        CHECK(Near(on.last_status.ql_base_us, 8000.0) && Near(on.last_status.ql_need_us, 8000.0) &&
            Near(on.last_status.ql_service_us, 8000.0));
        CHECK(Near(on.last_status.ql_q_us, 500.0) && Near(on.last_status.ql_p_us, 500.0 / 32.0));
        CHECK(on.last_status.bindable && Near(on.last_status.gpu_interval_us, 8000.0 + 500.0 / 32.0));
        // Both are computed: with reports flowing as well, the report-based value is still the
        // one the status carried before the law was in use.
        CHECK((on.last_status.flags & kUpQueueLaw) != 0 && (on.last_status.flags & kUpQueueLawGuard) == 0);
    }

    void TestStatusFlagsAndStatistics()
    {
        // Limited: a cap at the GPU's rate (J = B) with a positive margin; 4 ms of every cycle is ReLimiter's hold,
        // which the cap's slack pays (J - S^ = 4 ms refills the credit per record).
        Rig held(true);
        Rig::Planned frame;
        held.Plan(&frame, 20000.0); // J is known from the first grant
        held.Records(Record(20000.0, 0.0, 3000.0, 4000.0), 16);
        const UnifiedHoldGrant held_grant = held.Plan(&frame, 20000.0);
        CHECK((held.last_status.flags & kUpQueueLaw) != 0 && (held.last_status.flags & kUpQueueLawLimited) != 0);
        CHECK(Near(held.last_status.gpu_interval_us, 20000.0) && held.last_status.ql_p_us == 0.0);
        CHECK(held_grant.live && Near(held_grant.credit_us, 5 * 4000.0)); // 12th..16th refill 4 ms
        // Guard: the newest four margins are above 2M while the deficit is not limiting.
        Rig guard(true);
        guard.Records(Record(16000.0, 0.0, 2000.0), 12);
        guard.Records(Record(19200.0, 0.0, 12000.0), 4);
        guard.Plan(&frame, 8000.0);
        CHECK((guard.last_status.flags & kUpQueueLawGuard) != 0 && (guard.last_status.flags & kUpQueueLawLimited) == 0);
        CHECK(Near(guard.last_status.gpu_interval_us, 16800.0 + 2000.0 / 32.0 + 500.0));
        // Off or not in use: none of the bits.
        Rig off(false);
        off.Plan(&frame);
        CHECK((off.last_status.flags & (kUpQueueLaw | kUpQueueLawLimited | kUpQueueLawGuard)) == 0);
        CHECK(off.last_status.ql_valid_frames == -1);
        // Law on, no record yet: the count is 0 and nothing else.
        Rig empty(true);
        empty.Plan(&frame);
        CHECK(empty.last_status.ql_valid_frames == 0 && !empty.last_status.ql_valid && !empty.last_status.bindable);
    }

    void TestPlannedFramesAreCountedOncePerIndex()
    {
        Rig rig(true);
        rig.Records(Record(8000.0, 0.0, 500.0), 16);
        const auto in_use = [&](uint64_t index) {
            return (rig.pacer.Status(index, kJ).flags & kUpQueueLaw) != 0;
        };
        // The same index asked again (Reserve and the plan) is one planned frame.
        for (int i = 0; i < 5; ++i)
            CHECK(in_use(1));
        CHECK(in_use(2) && in_use(3) && in_use(4)); // ages 1..4 after index 1's own
        CHECK(!in_use(5));
        CHECK(!in_use(5));
        rig.Records(Record(8000.0, 0.0, 500.0), 1);
        CHECK(in_use(5)); // a fresh record: the age restarts (and index 5's frame is already counted)
    }

    void TestOptionsAndToggle()
    {
        UnifiedPacer pacer;
        pacer.Reset(1, 1, kFreq);
        UnifiedOptions options{};
        options.queue_law = true;
        options.queue_rank = 0;
        options.queue_allow_us = -5.0;
        pacer.SetOptions(options);
        CHECK(pacer.Options().queue_rank == 1 && pacer.Options().queue_allow_us == 0.0);
        options.queue_rank = 99;
        options.queue_allow_us = 99999.0;
        pacer.SetOptions(options);
        CHECK(pacer.Options().queue_rank == kUnifiedQueueRankMax && pacer.Options().queue_allow_us == kUnifiedQueueAllowMaxUs);
        options.queue_allow_us = std::nan("");
        pacer.SetOptions(options);
        CHECK(pacer.Options().queue_allow_us == 0.0);
        // The defaults are off, 2, 4000.
        const UnifiedOptions defaults{};
        CHECK(!defaults.queue_law && defaults.queue_rank == 2 && defaults.queue_allow_us == 4000.0);

        // The rank and M change what the law computes.
        Rig rank1(true, 0.0, 1);
        Rig rank2(true, 0.0, 2);
        Rig::Planned frame;
        for (Rig* rig : {&rank1, &rank2}) {
            for (int i = 1; i <= 16; ++i)
                rig->Records(Record(10000.0, 0.0, 100.0 * i), 1);
            rig->Plan(&frame, 5000.0);
        }
        CHECK(Near(rank1.last_status.ql_q_us, 100.0) && Near(rank2.last_status.ql_q_us, 200.0));
        Rig small_m(true, 0.0, 2, 400.0);
        for (int i = 1; i <= 16; ++i)
            small_m.Records(Record(10000.0, 0.0, 100.0 * i), 1);
        small_m.Plan(&frame, 5000.0);
        CHECK(Near(small_m.last_status.ql_q_us, 400.0) && (small_m.last_status.flags & kUpQueueLawGuard) != 0);

        // Law off: the window is gone and nothing of it runs; on again: it starts empty.
        Rig toggle(true);
        toggle.Records(Record(8000.0, 0.0, 500.0), 16);
        toggle.Plan(&frame);
        CHECK(toggle.pacer.QueueLawInUse() && toggle.last_status.ql_valid_frames == 16);
        UnifiedOptions off = toggle.pacer.Options();
        off.queue_law = false;
        toggle.pacer.SetOptions(off);
        toggle.Records(Record(8000.0, 0.0, 500.0), 16);
        CHECK(!toggle.pacer.QueueLawInUse());
        toggle.Plan(&frame);
        CHECK(toggle.last_status.ql_valid_frames == -1 && (toggle.last_status.flags & kUpQueueLaw) == 0);
        off.queue_law = true;
        toggle.pacer.SetOptions(off);
        toggle.Plan(&frame);
        CHECK(toggle.last_status.ql_valid_frames == 0 && !toggle.pacer.QueueLawInUse());
        // A lifecycle reset empties the window too.
        toggle.Records(Record(8000.0, 0.0, 500.0), 16);
        CHECK(toggle.pacer.QueueLawInUse());
        toggle.pacer.Reset(toggle.grid + 1, 1, kFreq);
        CHECK(!toggle.pacer.QueueLawInUse());
    }

    void TestReservationUsesTheLaw()
    {
        UnifiedIntervalTerms terms{};
        terms.cap_us = 16000.0; // J
        // GPU cycle 20 ms, margin 1 ms: I_gpu = B + 1000/32 (the deficit 4 ms is above B/8 = 2.5 ms).
        UnifiedPacer on;
        on.Reset(1, 1, kFreq);
        UnifiedOptions options{};
        options.queue_law = true;
        on.SetOptions(options);
        for (int i = 0; i < 16; ++i)
            on.OnQueueRecord(Record(20000.0, 0.0, 1000.0));
        UnifiedStart start{};
        CHECK(on.Reserve(1, 1, kStart, terms, &start) == UnifiedReserveStatus::Reserved);
        CHECK(Near(start.gpu_interval_us, 20000.0 + 1000.0 / 32.0) && Near(start.interval_us, start.gpu_interval_us));
        CHECK((start.flags & kUpQueueLaw) != 0 && (start.flags & kUpBoundGpu) != 0);
        // The law off: no I_gpu without reports and the cap decides.
        UnifiedPacer off;
        off.Reset(1, 1, kFreq);
        for (int i = 0; i < 16; ++i)
            off.OnQueueRecord(Record(20000.0, 0.0, 1000.0));
        UnifiedStart plain{};
        CHECK(off.Reserve(1, 1, kStart, terms, &plain) == UnifiedReserveStatus::Reserved);
        CHECK(plain.gpu_interval_us == 0.0 && Near(plain.interval_us, 16000.0) && (plain.flags & kUpQueueLaw) == 0);
        // The idle regime leaves I_gpu out, the law included.
        UnifiedPacer idle;
        idle.Reset(1, 1, kFreq);
        idle.SetOptions(options);
        for (int i = 0; i < 16; ++i)
            idle.OnQueueRecord(Record(20000.0, 0.0, 1000.0));
        UnifiedIntervalTerms idle_terms{};
        idle_terms.idle = true;
        idle_terms.idle_us = 100000.0;
        UnifiedStart idle_start{};
        CHECK(idle.Reserve(1, 1, kStart, idle_terms, &idle_start) == UnifiedReserveStatus::Reserved);
        CHECK(idle_start.gpu_interval_us == 0.0 && Near(idle_start.interval_us, 100000.0) && (idle_start.flags & kUpQueueLaw) == 0);
    }

    // ── Holds across a switch of rules, freshness in frames, probe sessions ──

    void TestHoldGrantedUnderFallbackIsDebitedOnceByIt()
    {
        // A hold granted under the report-based rule (its measured hold 3 ms) is debited at once
        // when it becomes known; its record (hold block 2.5 ms) arrives after the law came into use
        // and must not debit it again.
        Rig rig(true);
        Rig::Planned frame;
        for (int i = 0; i < 8; ++i) {
            rig.Plan(&frame);
            rig.Evidence(frame, 6000.0);
        }
        CHECK(Near(rig.Plan(&frame).credit_us, 7 * 4000.0)); // frame 9, granted under the report rule
        rig.Evidence(frame, 6000.0, 3000.0);                  // -3000 (spent), +4000 (the report)
        // The law comes into use at the 12th arrival, the record of frame 10: it carries frame 9's hold.
        rig.Records(Record(8000.0, 0.0, 500.0), 11);
        rig.pacer.OnQueueRecord(Keyed(Record(8000.0, 0.0, 500.0, 2500.0), 10));
        CHECK(rig.pacer.QueueLawInUse());
        const double need = (11.0 * 8000.0 + 5500.0) / 12.0;
        CHECK(Near(rig.Plan(&frame).credit_us, 29000.0 + (kJ - need)));
        // A repeated record never spends it either.
        rig.pacer.OnQueueRecord(Keyed(Record(8000.0, 0.0, 500.0, 2500.0), 10));
        const double need2 = (11.0 * 8000.0 + 2.0 * 5500.0) / 13.0;
        CHECK(Near(rig.Plan(&frame).credit_us, 29000.0 + (kJ - need) + (kJ - need2)));
    }

    void TestHoldGrantedUnderLawIsDebitedOnceByItsRecord()
    {
        // A hold granted under the queue law is not spent when it becomes known after the rule fell
        // back to the report-based one, and is spent by its record (hold block 2.5 ms) when that
        // arrives, also under the fallback, exactly once.
        Rig rig(true);
        Rig::Planned frame;
        for (int i = 0; i < 4; ++i) {
            rig.Plan(&frame);
            rig.Evidence(frame, 10000.0); // active = J: the report rule refills nothing
        }
        rig.Records(Record(8000.0, 0.0, 500.0), 12); // keyless: the arrival-count rule; +2000 at the 12th
        Rig::Planned held;
        const UnifiedHoldGrant held_grant = rig.Plan(&held);
        CHECK(Near(held_grant.credit_us, 2000.0) && held_grant.law_sequence != 0); // frame 5, granted under the law
        for (int i = 0; i < 7; ++i)                     // frames 6..12: no record, the law ages out
            rig.Plan(&frame);
        CHECK(!rig.pacer.QueueLawInUse());
        // Frame 5's hold (3 ms) becomes known under the fallback: not spent here.
        rig.Evidence(held, 10000.0, 3000.0);
        CHECK(Near(rig.Plan(&frame).credit_us, 2000.0)); // frame 13
        // Its record arrives late (a stale position, the fallback still in force): spent once, beyond the slack of the window
        // it joins (12 records of 8 ms and its own 5.5 ms of service).
        rig.pacer.OnQueueRecord(Named(Keyed(Record(8000.0, 0.0, 500.0, 2500.0), 6), held_grant.law_sequence));
        CHECK(!rig.pacer.QueueLawInUse());
        const double slack = kJ - (12.0 * 8000.0 + 5500.0) / 13.0;
        CHECK(Near(rig.Plan(&frame).credit_us, 2000.0 - (2500.0 - slack), 1e-6));
        rig.pacer.OnQueueRecord(Named(Keyed(Record(8000.0, 0.0, 500.0, 2500.0), 6), held_grant.law_sequence));
        CHECK(Near(rig.Plan(&frame).credit_us, 2000.0 - (2500.0 - slack), 1e-6));
    }

    void TestFreshnessIsMeasuredInFrames()
    {
        // The window level: the largest position counts; the planned frame at most 4 past it.
        UnifiedQueueWindow window;
        for (int i = 1; i <= 16; ++i)
            window.Push(SampleS(500.0), Position(static_cast<uint64_t>(i)));
        CHECK(window.InUse(Position(20)) && !window.InUse(Position(21)));
        // Out of order by position: accepted into the statistics, the largest still decides.
        window.Push(SampleS(500.0), Position(3));
        CHECK(window.InUse(Position(20)) && !window.InUse(Position(21)));
        CHECK(window.Compute(kJ, Params(), Position(20)).in_use && !window.Compute(kJ, Params(), Position(21)).in_use);
        // An arrival-fresh batch of old frames does not make it fresher.
        for (int i = 0; i < 16; ++i)
            window.Push(SampleS(500.0), Position(static_cast<uint64_t>(1 + i)));
        CHECK(!window.InUse(Position(40)));
        // Not comparable (unknown planned position, another epoch or domain, a keyless newest
        // arrival): the arrival-count rule decides.
        CHECK(window.InUse() && window.InUse(Position(40, 2)) &&
            window.InUse(Position(40, 1, PacingIdentityDomain::PclFrame)));
        for (int i = 0; i < 5; ++i)
            window.NotePlanned();
        CHECK(!window.InUse() && !window.InUse(Position(40, 2)));
        window.Push(SampleS(500.0)); // keyless: a fresh arrival
        CHECK(window.InUse(Position(40)));
        // Another epoch is a new stream: its positions replace the old ones.
        UnifiedQueueWindow epochs;
        for (int i = 0; i < 16; ++i)
            epochs.Push(SampleS(500.0), Position(1000));
        for (int i = 0; i < 12; ++i)
            epochs.Push(SampleS(500.0), Position(5, 2));
        CHECK(epochs.InUse(Position(9, 2)) && !epochs.InUse(Position(10, 2)));
    }

    void TestDelayedBatchOfOldRecordsStaysFallback()
    {
        Rig rig(true);
        Rig::Planned frame;
        for (int i = 0; i < 25; ++i)
            rig.Plan(&frame);
        // A delayed batch: the records of frames 1..16 arrive when frame 26 is being planned.
        for (uint64_t id = 1; id <= 16; ++id)
            rig.pacer.OnQueueRecord(Keyed(Record(8000.0, 0.0, 500.0), id));
        CHECK(!rig.pacer.QueueLawInUse());
        const UnifiedHoldGrant stale = rig.Plan(&frame, 5000.0);
        CHECK(!stale.live && (rig.last_status.flags & kUpQueueLaw) == 0 && !rig.last_status.bindable);
        CHECK(rig.last_status.ql_valid && rig.last_status.ql_valid_frames == 16); // computed, not authoritative
        // Fresh records in order (frames 22..25) make the largest position fresh again.
        for (uint64_t id = 22; id <= 25; ++id)
            rig.pacer.OnQueueRecord(Keyed(Record(8000.0, 0.0, 500.0), id));
        CHECK(rig.pacer.QueueLawInUse());
        rig.Plan(&frame, 5000.0);
        CHECK((rig.last_status.flags & kUpQueueLaw) != 0 && rig.last_status.bindable);
        // Old records arriving later, however many, do not move the position and do not keep it fresh.
        for (int i = 0; i < 10; ++i)
            rig.Plan(&frame);
        for (uint64_t id = 1; id <= 16; ++id)
            rig.pacer.OnQueueRecord(Keyed(Record(8000.0, 0.0, 500.0), id));
        CHECK(!rig.pacer.QueueLawInUse());
    }

    void TestProbeSessionRestartsTheWindowAndKeepsTheCredit()
    {
        Rig rig(true);
        Rig::Planned frame;
        for (int i = 0; i < 4; ++i) {
            rig.Plan(&frame);
            rig.Evidence(frame, 10000.0); // reports live, refill nothing
        }
        for (int i = 0; i < 16; ++i)
            rig.pacer.OnQueueRecord(Session(Record(8000.0, 0.0, 500.0), 7));
        CHECK(rig.pacer.QueueLawInUse());
        CHECK(Near(rig.Plan(&frame).credit_us, 5 * 2000.0));
        // The queue was adopted again: the first record of session 8 restarts the window; the
        // credit is as it was and the report-based rule is back.
        rig.pacer.OnQueueRecord(Session(Record(8000.0, 0.0, 500.0), 8));
        CHECK(!rig.pacer.QueueLawInUse());
        CHECK(Near(rig.Plan(&frame).credit_us, 10000.0) && rig.last_status.ql_valid_frames == 1);
        // A late record of the retired session is dropped: no arrival, no debit.
        rig.pacer.OnQueueRecord(Session(Record(8000.0, 0.0, 500.0, 6000.0), 7));
        CHECK(Near(rig.Plan(&frame).credit_us, 10000.0) && rig.last_status.ql_valid_frames == 1);
        // Records of an unknown source (0) are accepted.
        rig.pacer.OnQueueRecord(Record(8000.0, 0.0, 500.0));
        CHECK(rig.Plan(&frame).live && rig.last_status.ql_valid_frames == 2);
        // The new session fills its own window: in use from its 12th arrival, refilling from there.
        for (int i = 0; i < 14; ++i)
            rig.pacer.OnQueueRecord(Session(Record(8000.0, 0.0, 500.0), 8));
        CHECK(rig.pacer.QueueLawInUse());
        CHECK(Near(rig.Plan(&frame).credit_us, 10000.0 + 5 * 2000.0));
    }

    void TestPacedEntriesGateTheStarvationInTheBase()
    {
        // The record of frame k takes `paced` from frame k's own plan entry (OnPlan), and the flag
        // 0x1000000 says the record that entered since the last plan was paced. Every other frame starves
        // 1 ms (half the window at most: a transient, all of it kept), the others are fed; the F-term is
        // 15 ms on every frame, and the paced frames add their kept starvation's 16-frame mean, 0.5 ms.
        for (const bool paced : {true, false}) {
            Rig rig(true, 0.0, 2, 4000.0, 4);
            Rig::Planned frame;
            for (uint64_t k = 1; k <= 24; ++k) {
                rig.Plan(&frame, 5000.0, paced);
                rig.pacer.OnQueueRecord(Keyed(k % 2 == 0 ? Record(16000.0, 1000.0, 0.0, 0.0, 0.0, 0.0, false)
                                                         : Record(15000.0, 0.0, 500.0), k));
            }
            rig.Plan(&frame, 5000.0, paced);
            CHECK(rig.last_status.ql_valid);
            CHECK(Near(rig.last_status.ql_base_us, paced ? 15500.0 : 15000.0));
            CHECK(((rig.last_status.flags & kUpQueueLawPaced) != 0) == paced);
            rig.Plan(&frame, 5000.0, paced); // no record since the last plan
            CHECK((rig.last_status.flags & kUpQueueLawPaced) == 0);
        }
        // A record with no plan entry (keyless, or a frame never planned) is not paced.
        Rig rig(true, 0.0, 2, 4000.0, 4);
        Rig::Planned frame;
        for (uint64_t k = 1; k <= 24; ++k)
            rig.Plan(&frame, 5000.0, true);
        for (int i = 0; i < 8; ++i)
            rig.pacer.OnQueueRecord(Record(16000.0, 1000.0, 0.0, 0.0, 0.0, 0.0, false));
        for (uint64_t id = 100; id < 108; ++id)
            rig.pacer.OnQueueRecord(Keyed(Record(16000.0, 1000.0, 0.0, 0.0, 0.0, 0.0, false), id));
        rig.Plan(&frame, 5000.0, true);
        CHECK(Near(rig.last_status.ql_base_us, 15000.0) && (rig.last_status.flags & kUpQueueLawPaced) == 0);
    }

    void TestGameLimitedFramesNeverBindTheGpuTerm()
    {
        // The game's own limit sets the interval (20 ms) and every frame is starved by 3.6 ms; no
        // entry is GPU-paced: B is the need (16.4 ms) and I_gpu stays below the game term, which
        // keeps binding.
        UnifiedPacer pacer;
        pacer.Reset(1, 1, kFreq);
        UnifiedOptions options{};
        options.queue_law = true;
        pacer.SetOptions(options);
        for (int i = 0; i < 16; ++i)
            pacer.OnQueueRecord(Record(20000.0, 3600.0, 0.0, 0.0, 0.0, 0.0, false));
        UnifiedIntervalTerms terms{};
        terms.game_us = 20000.0;
        UnifiedStart start{};
        CHECK(pacer.Reserve(1, 1, kStart, terms, &start) == UnifiedReserveStatus::Reserved);
        CHECK((start.flags & kUpQueueLaw) != 0 && (start.flags & kUpBoundGpu) == 0);
        CHECK(Near(start.interval_us, 20000.0) && start.gpu_interval_us > 0.0 && start.gpu_interval_us < 20000.0);
    }

    void TestNeedFramesOption()
    {
        UnifiedPacer pacer;
        pacer.Reset(1, 1, kFreq);
        CHECK(UnifiedOptions{}.queue_need_frames == 4);
        UnifiedOptions options{};
        options.queue_law = true;
        options.queue_need_frames = 0;
        pacer.SetOptions(options);
        CHECK(pacer.Options().queue_need_frames == 1);
        options.queue_need_frames = 99;
        pacer.SetOptions(options);
        CHECK(pacer.Options().queue_need_frames == kUnifiedQueueNeedFramesMax);
        // It reaches the law's base: a 20% longer cycle in one step (starve 0): F = 4 follows within
        // four records, F = 16 the 16-frame mean.
        for (const int need_frames : {4, 16}) {
            Rig rig(true, 0.0, 2, 4000.0, need_frames);
            Rig::Planned frame;
            rig.Plan(&frame, 5000.0);
            rig.Records(Record(16000.0, 0.0, 500.0), 12);
            rig.Records(Record(19200.0, 0.0, 500.0), 4);
            rig.Plan(&frame, 5000.0);
            CHECK(Near(rig.last_status.ql_base_us, need_frames == 4 ? 19200.0 : 16800.0));
        }
    }

    void TestWholeIntervalBound()
    {
        // Two windows of different lengths, frames GPU-paced, cap 20 ms (J): 9 fed frames of 16 ms, then 3 that
        // starved 4 ms (a minority: their starvation is kept), then the need rises to 20 ms on 4 fed frames.
        // B = mean4(F-term) + mean16(kept paced starvation) = 20 + 0.75 = 20.75 ms is above the mean cycle,
        // 17.75 ms; the bound C + max(0, C - J) = 17.75 ms cuts I_gpu to it and sets the flag. P stays B's
        // correction: q = max(-4 ms, 0.5 - 4 ms) = -3.5 ms.
        const auto fill = [](const auto& push) {
            for (uint64_t k = 1; k <= 16; ++k)
                push(k, k <= 9 ? Record(16000.0, 0.0, 500.0)
                       : k <= 12 ? Record(20000.0, 4000.0, 0.0, 0.0, 0.0, 0.0, false)
                                 : Record(20000.0, 0.0, 500.0));
        };
        UnifiedQueueWindow window;
        fill([&](uint64_t, const PacingGpuQueueRecord& record) { window.Push(PacedSample(record, true)); });
        const UnifiedQueueResult bounded = window.Compute(20000.0, Params(2, 4000.0, 0.0, 4));
        CHECK(Near(bounded.base_us, 20750.0) && Near(bounded.p_us, -3500.0 / 32.0));
        CHECK(bounded.bound_active && Near(bounded.gpu_interval_us, 17750.0));
        // F = 16: the first build's value (B = the mean cycle, B + P), no bound, no flag.
        const UnifiedQueueResult first = window.Compute(20000.0, Params(2, 4000.0, 0.0, 16));
        CHECK(!first.bound_active && Near(first.base_us, 17750.0) && Near(first.gpu_interval_us, 17750.0 - 3500.0 / 32.0));
        // A GPU-bound stream (cycles above J): the bound C + (C - J) is far above B + P: not active.
        UnifiedQueueWindow gpu_bound;
        for (int i = 0; i < 16; ++i)
            gpu_bound.Push(PacedSample(Record(20000.0, 0.0, 500.0), true));
        const UnifiedQueueResult unbounded = gpu_bound.Compute(10000.0, Params(2, 4000.0, 0.0, 4));
        CHECK(!unbounded.bound_active && Near(unbounded.gpu_interval_us, 20000.0 + 500.0 / 32.0));
        // Through the pacer: the same frames, planned GPU-paced; the status carries I_gpu = 17.75 ms and
        // the flag 0x2000000 (F = 4), and neither with F = 16.
        for (const int need_frames : {4, 16}) {
            Rig rig(true, 0.0, 2, 4000.0, need_frames);
            Rig::Planned frame;
            fill([&](uint64_t k, const PacingGpuQueueRecord& record) {
                rig.Plan(&frame, 20000.0, true);
                rig.pacer.OnQueueRecord(Keyed(record, k));
            });
            rig.Plan(&frame, 20000.0, true);
            CHECK((rig.last_status.flags & kUpQueueLaw) != 0);
            CHECK(((rig.last_status.flags & kUpQueueLawBound) != 0) == (need_frames == 4));
            CHECK(Near(rig.last_status.gpu_interval_us, need_frames == 4 ? 17750.0 : 17750.0 - 3500.0 / 32.0));
        }
    }

    void TestNonFrameGenerationRecordsTakeTheAttributionKey()
    {
        // Without frame generation the probe's readiness site tags the record with the key of the
        // Present's PRESENT_START attribution (peeked, not consumed): the same key the frame's plan
        // entry has. The rules of the peek are the take's.
        const int64_t max_age = kFreq / 10;
        PacingPresentAttribution attribution{};
        attribution.key = PacingFrameKey{PacingIdentityDomain::NvApiFrame, 1, 30};
        attribution.marker_qpc = kStart;
        PacingTelemetry_SetPresentAttribution(attribution);
        CHECK(PacingTelemetry_PeekPresentAttributionKey(kStart + 1000, max_age) == attribution.key);
        CHECK(PacingTelemetry_PeekPresentAttributionKey(kStart + 1000, max_age) == attribution.key); // not consumed
        CHECK(PacingTelemetry_PeekPresentAttributionKey(kStart + max_age + 1, max_age) == PacingFrameKey{});
        CHECK(PacingTelemetry_PeekPresentAttributionKey(kStart - 1, max_age) == PacingFrameKey{});
        CHECK(PacingTelemetry_TakePresentAttribution(kStart + 1000, max_age).key == attribution.key); // the row still takes it
        CHECK(PacingTelemetry_PeekPresentAttributionKey(kStart + 1000, max_age) == PacingFrameKey{});

        // The record the probe pushes for a non-FG Present: its Present identity and that key. It
        // resolves to its frame's plan entry and counts the GPU-paced starvation; a record without a
        // key (the marker was on another thread) or with the key of a frame never planned stays unpaced.
        // Every even frame starves 1 ms and every odd one is fed (the starved ones a minority: kept whole).
        const auto probe_record = [&](uint64_t frame_id, bool keyed) {
            attribution.key = PacingFrameKey{PacingIdentityDomain::NvApiFrame, 1, frame_id};
            PacingTelemetry_SetPresentAttribution(attribution);
            PacingGpuQueueRecord record = frame_id % 2 == 0 ? Record(16000.0, 1000.0, 0.0, 0.0, 0.0, 0.0, false)
                                                            : Record(15000.0, 0.0, 500.0);
            record.swapchain_generation = 9;
            record.present_sequence = 1000 + frame_id;
            if (keyed)
                record.key = PacingTelemetry_PeekPresentAttributionKey(kStart + 1000, max_age);
            PacingTelemetry_TakePresentAttribution(kStart + 1000, max_age);
            return record;
        };
        Rig keyed(true, 0.0, 2, 4000.0, 4);
        Rig::Planned frame;
        for (uint64_t k = 1; k <= 24; ++k)
            keyed.Plan(&frame, 5000.0, true);
        for (uint64_t k = 9; k <= 24; ++k)
            keyed.pacer.OnQueueRecord(probe_record(k, true));
        keyed.Plan(&frame, 5000.0, true);
        CHECK(Near(keyed.last_status.ql_base_us, 15500.0)); // need 15000 + paced starve 8 x 1000 / 16
        CHECK((keyed.last_status.flags & kUpQueueLawPaced) != 0);
        Rig mixed(true, 0.0, 2, 4000.0, 4);
        for (uint64_t k = 1; k <= 24; ++k)
            mixed.Plan(&frame, 5000.0, true);
        for (uint64_t k = 9; k <= 16; ++k)
            mixed.pacer.OnQueueRecord(probe_record(k, false));   // no key
        for (uint64_t k = 117; k <= 124; ++k)
            mixed.pacer.OnQueueRecord(probe_record(k, true));    // a frame that was never planned
        mixed.Plan(&frame, 5000.0, true);
        CHECK(Near(mixed.last_status.ql_base_us, 15000.0) && (mixed.last_status.flags & kUpQueueLawPaced) == 0);
    }

    void TestQueueLawSummaryCounters()
    {
        Rig rig(true);
        Rig::Planned frame;
        for (int i = 0; i < 4; ++i) {
            rig.Plan(&frame);
            rig.Evidence(frame, 10000.0);
        }
        rig.Records(Record(8000.0, 0.0, 500.0), 12); // keyless: the law is in use from the 12th
        Rig::Planned first;
        Rig::Planned second;
        const UnifiedHoldGrant first_grant = rig.Plan(&first);  // frame 5, granted by the law
        rig.Plan(&second); // frame 6, granted by the law
        rig.Evidence(first, 10000.0, 3000.0);
        rig.Evidence(second, 10000.0, 1000.0);
        // The record of frame 6 carries frame 5's hold: debited by the record. Frame 6's hold has
        // no record (frame 7's): it was never paid.
        rig.pacer.OnQueueRecord(Named(Keyed(Record(8000.0, 0.0, 500.0, 2500.0), 6), first_grant.law_sequence));
        UnifiedQueueLawSummary s = rig.pacer.QueueLawSummary();
        CHECK(s.frames_planned == 6 && s.frames_in_use == 2);
        CHECK(s.grants_law == 2 && s.grants_report == 2);
        CHECK(s.records_accepted == 13 && s.records_valid == 13 && s.records_late == 0 && s.records_retired == 0);
        CHECK(s.holds_debited_record == 1 && s.holds_debited_report == 0 && s.holds_never_debited == 1);
        CHECK(rig.pacer.QueueLawSummary().holds_never_debited == 1); // counted once per frame
        // Probe sessions: a retired session's record is dropped and counted, not accepted.
        rig.pacer.OnQueueRecord(Session(Record(8000.0, 0.0, 500.0), 8));
        rig.pacer.OnQueueRecord(Session(Record(8000.0, 0.0, 500.0), 7));
        s = rig.pacer.QueueLawSummary();
        CHECK(s.records_accepted == 14 && s.records_retired == 1);
        // Cleared: zero until something counts again.
        rig.pacer.ClearQueueLawSummary();
        s = rig.pacer.QueueLawSummary();
        CHECK(s.frames_planned == 0 && s.records_accepted == 0 && s.grants_law == 0 && s.holds_never_debited == 0);
        // A hold debited by the report rule counts as such (a frame granted under it, hold known).
        Rig report(true);
        for (int i = 0; i < 4; ++i) {
            report.Plan(&frame);
            report.Evidence(frame, 6000.0, i == 3 ? 1500.0 : 0.0);
        }
        CHECK(report.pacer.QueueLawSummary().holds_debited_report == 1);
        // The key off: nothing counts.
        Rig off(false);
        for (int i = 0; i < 4; ++i) {
            off.Plan(&frame);
            off.Evidence(frame, 6000.0, 1500.0);
        }
        off.Records(Record(8000.0, 0.0, 500.0), 16);
        const UnifiedQueueLawSummary none = off.pacer.QueueLawSummary();
        CHECK(none.frames_planned == 0 && none.records_accepted == 0 && none.grants_report == 0 &&
            none.holds_debited_report == 0 && none.holds_never_debited == 0);
    }

    void TestBaseCarriesHoldsOnlyBeyondTheCapsSlack()
    {
        // Cap 20 ms (J), the GPU's service about 15.4 ms: the cap leaves a slack of J - S^ and the
        // holds fill it. Hold blocking up to the slack is paid by the cap, not by the GPU: it drops
        // out of the base (B = need_free + the part of a hold above the slack).
        const double empties[4] = {0.0, 250.0, 500.0, 250.0};
        UnifiedQueueWindow filled;
        for (int i = 0; i < 40; ++i)
            filled.Push(PacedSample(Record(20000.0, empties[i % 4], 500.0, 4600.0), false));
        const UnifiedQueueResult held = filled.Compute(20000.0, Params(2, 4000.0, 0.0, 4));
        CHECK(Near(held.service_us, 15150.0));
        CHECK(Near(held.base_us, 15150.0) && held.base_us < 16500.0); // not the cap's 19.75 ms
        CHECK(held.gpu_interval_us < 20000.0 && !held.bound_active);
        // F = 16 keeps the first build's base: the mean cycle, holds included.
        CHECK(Near(filled.Compute(20000.0, Params(2, 4000.0, 0.0, 16)).base_us, 20000.0));

        // 5.2 ms of hold on every frame with the same 15.4 ms service (the cycle is 20.6 ms): the slack is
        // 4.6 ms, and the hold every frame shares is the window's common level, not a transient: it leaves the
        // base whole (B = S^), where it once kept the 0.6 ms above the slack. The cap, in the GPU's reach,
        // keeps binding either way.
        UnifiedQueueWindow over;
        for (int i = 0; i < 40; ++i)
            over.Push(PacedSample(Record(20600.0, 0.0, 500.0, 5200.0), false));
        const UnifiedQueueResult beyond = over.Compute(20000.0, Params(2, 4000.0, 0.0, 4));
        CHECK(Near(beyond.service_us, 15400.0) && Near(beyond.base_us, 15400.0));

        // No cap (the VRR interval is far below the service): no slack. Every frame is blocked 0.5 ms and every
        // other one also starves 0.3 ms. The 0.5 ms every frame loses is the common level and leaves the base;
        // the odd frames' 0.3 ms beyond it stays, split in proportion between their starvation (112.5 us, which
        // a GPU-paced frame adds back at its 16-frame mean) and their block (187.5 us).
        UnifiedQueueWindow uncapped;
        UnifiedQueueWindow uncapped_paced;
        for (int i = 0; i < 40; ++i) {
            const PacingGpuQueueRecord record = Record(16500.0, i % 2 == 0 ? 0.0 : 300.0, 500.0, 500.0);
            uncapped.Push(PacedSample(record, false));
            uncapped_paced.Push(PacedSample(record, true));
        }
        CHECK(Near(uncapped.Compute(6944.0, Params(2, 4000.0, 0.0, 4)).base_us, (16000.0 + 15887.5) / 2.0));
        CHECK(Near(uncapped_paced.Compute(6944.0, Params(2, 4000.0, 0.0, 4)).base_us, (16000.0 + 15887.5) / 2.0 + 56.25));
    }

    PacingGpuQueueRecord RoleRecord(const PacingGpuQueueRecord& record, uint8_t role)
    {
        PacingGpuQueueRecord out = record;
        out.queue_role = role;
        return out;
    }

    // The order's block on the render queue found by dependency is ReLimiter's own idle, never the GPU's need: the
    // base of such a record's samples holds none of it (unified_queue_law.h, the invariant). The capture that
    // broke it: Rogue Core at a cap of 130 (J = 7.69 ms), the GPU's service above the cap so no slack, every frame
    // held to its target.
    void TestRenderQueueOrderBlockIsNotNeed()
    {
        const double j = 7690.0;
        const UnifiedQueueParams params = Params(2, 4000.0, 0.0, 4); // the law's own F
        // `steady` records of cycle `steady_cycle` / hold block `steady_hold`, then `blocked` of 11.4 ms / 3.0 ms
        // of block; no starve, a margin of 0.3 ms.
        const auto compute = [&](uint8_t role, int steady, double steady_cycle, double steady_hold, int blocked) {
            UnifiedQueueWindow window;
            for (int i = 0; i < steady; ++i)
                window.Push(UnifiedQueueSample_FromRecord(RoleRecord(Record(steady_cycle, 0.0, 300.0, steady_hold), role)));
            for (int i = 0; i < blocked; ++i)
                window.Push(UnifiedQueueSample_FromRecord(RoleRecord(Record(11400.0, 0.0, 300.0, 3000.0), role)));
            return window.Compute(j, params);
        };

        // 16 records of 11.4 ms with 3.0 ms of order block: the base is the need, 8.4 ms, not the cycle. The
        // render queue found by dependency under frame generation (role 4) is the same.
        for (const uint8_t role : {kUnifiedQueueRoleRenderQueue, kUnifiedQueueRoleFgRenderQueue}) {
            const UnifiedQueueResult render = compute(role, 0, 0.0, 0.0, 16);
            CHECK(render.computable);
            CHECK(Near(render.base_us, 8400.0) && Near(render.need_us, 8400.0) && Near(render.service_us, 8400.0));
            CHECK(render.service_us > j); // the cap leaves no slack: the removal is the role's, not the slack's
            CHECK(Near(render.gpu_interval_us, 8400.0 + 300.0 / 32.0)); // P is the margin term, as before
        }
        // The same records of a queue with no role, the presenting queue and the frame generation's input: the
        // block every frame shares is the window's common level, ReLimiter's own excess at a cap the GPU cannot
        // reach, and leaves the base as well (it held it at 11.4 ms before).
        for (const uint8_t role : {uint8_t{0}, uint8_t{1}, uint8_t{2}}) {
            const UnifiedQueueResult held = compute(role, 0, 0.0, 0.0, 16);
            CHECK(Near(held.base_us, 8400.0) && Near(held.need_us, 8400.0) && Near(held.service_us, 8400.0));
        }

        // 12 records of 8.4 ms, then 4 of 11.4 ms / 3.0 ms: the render queue's base stays at 8.4 ms (the block is
        // the order's); the presenting queue's 4 blocked frames are a minority of the window, a transient that
        // stays, so its base follows the cycle to 11.4 ms.
        CHECK(Near(compute(kUnifiedQueueRoleRenderQueue, 12, 8400.0, 0.0, 4).base_us, 8400.0));
        CHECK(Near(compute(kUnifiedQueueRoleFgRenderQueue, 12, 8400.0, 0.0, 4).base_us, 8400.0));
        CHECK(Near(compute(1, 12, 8400.0, 0.0, 4).base_us, 11400.0));

        // The sample: the block is out of the cycle and the sample carries none; need, service and the starve
        // terms are the record's own.
        const PacingGpuQueueRecord record = Record(11400.0, 0.0, 300.0, 3000.0, 100.0, 200.0);
        const UnifiedQueueSample taken = UnifiedQueueSample_FromRecord(RoleRecord(record, kUnifiedQueueRoleRenderQueue));
        const UnifiedQueueSample kept = UnifiedQueueSample_FromRecord(RoleRecord(record, 1));
        CHECK(taken.valid && kept.valid);
        CHECK(Near(taken.cycle_us, 8400.0) && taken.hold_block_us == 0.0);
        CHECK(Near(kept.cycle_us, 11400.0) && Near(kept.hold_block_us, 3000.0));
        CHECK(taken.need_us == kept.need_us && taken.service_us == kept.service_us && Near(taken.need_us, 8100.0));
        CHECK(taken.starve_us == kept.starve_us && taken.s_prime_us == kept.s_prime_us &&
            taken.margin_free_us == kept.margin_free_us);
        // A block longer than the cycle it was measured in (an interval's error) leaves a cycle of 0, never a
        // negative one.
        const UnifiedQueueSample clamped =
            UnifiedQueueSample_FromRecord(RoleRecord(Record(1000.0, 0.0, 10.0, 5000.0), kUnifiedQueueRoleRenderQueue));
        CHECK(clamped.valid && clamped.cycle_us == 0.0 && clamped.hold_block_us == 0.0 && clamped.need_us == 0.0);

        // A cap with headroom (J = 12.0 ms: a slack of 3.6 ms): the block leaves the render queue's base once, in the
        // sample, not again through the slack (that would be 5.4 ms); the presenting queue's 3.0 ms is the cap's.
        const auto sixteen_blocked = [](uint8_t role) {
            UnifiedQueueWindow window;
            for (int i = 0; i < 16; ++i)
                window.Push(UnifiedQueueSample_FromRecord(RoleRecord(Record(11400.0, 0.0, 300.0, 3000.0), role)));
            return window;
        };
        const UnifiedQueueWindow render_window = sixteen_blocked(kUnifiedQueueRoleRenderQueue);
        const UnifiedQueueWindow presenting_window = sixteen_blocked(1);
        CHECK(Near(render_window.Compute(12000.0, params).base_us, 8400.0));
        CHECK(Near(presenting_window.Compute(12000.0, params).base_us, 8400.0));
        // F = 16 is the mean cycle exactly, as the law takes it: 8.4 ms for the render queue (its hold block is not in
        // the cycle), 11.4 ms for the presenting queue, and the short-window bound is not evaluated.
        const UnifiedQueueResult render16 = render_window.Compute(j, Params(2, 4000.0, 0.0, 16));
        CHECK(Near(render16.base_us, 8400.0) && !render16.bound_active);
        CHECK(Near(presenting_window.Compute(j, Params(2, 4000.0, 0.0, 16)).base_us, 11400.0));
    }

    // The capture's loop, closed (Rogue Core, cap 130: J = 7.69 ms, the GPU's service above it, no slack). Every
    // frame is held to its target, so a frame's cycle is the interval planned for it and that interval is the base
    // the law computed before it (P is small and not in the base). The order holds a frame h = cycle - Delta, Delta
    // = 7.67 ms being the render queue's time from the Present's release to the next frame's readiness. The probe
    // does not see all of it: with the render thread ahead, the cycle less its hold block is the stamp of the render
    // queue's last completion minus the gate's release, so the block it sees is max(0, h - lag), lag = 0.3 ms the
    // fixed lag between the gate's readiness and that stamp. A block shorter than the lag is not seen and has gain 1:
    // the interval has a neutral band [Delta, Delta + lag], the cycle the law takes is min(cycle, Delta + lag), and
    // from a latched interval above the band the base is at the top of it after the first full window and stays
    // there. The test pins that bound above the band (the block has no gain there), not a convergence to Delta. The
    // presenting queue's and the frame generation input's samples keep their block (the hold), but a block every
    // frame shares is the window's common level and leaves the base: from the same latch they settle at the top of
    // the band too, where a base that held the block repeated the cycle it was made from and stayed at 11.4 ms.
    void TestRenderQueueBaseHasNoGainOnItsOwnOrderBlock()
    {
        const double j = 7690.0;
        const double delta = 7670.0;
        const double lag = 300.0;
        struct Settled {
            double first = 0.0; // the base after the first full window
            double last = 0.0;  // the base after 64 records
            double low = 0.0;   // the lowest and the highest base from the first one on
            double high = 0.0;
        };
        const auto settle = [&](uint8_t role, double start) {
            UnifiedQueueWindow window;
            Settled out;
            bool computed = false;
            double cycle = start;
            for (int i = 0; i < 64; ++i) {
                const double block = std::max(0.0, cycle - delta - lag);
                window.Push(UnifiedQueueSample_FromRecord(RoleRecord(Record(cycle, 0.0, 300.0, block), role)));
                const UnifiedQueueResult law = window.Compute(j, Params(2, 4000.0, 0.0, 4));
                if (!law.computable)
                    continue; // fewer than four samples: no base yet, the interval stays
                if (!computed) {
                    out.first = out.low = out.high = law.base_us;
                    computed = true;
                }
                out.last = law.base_us;
                out.low = std::min(out.low, law.base_us);
                out.high = std::max(out.high, law.base_us);
                cycle = law.base_us; // the next frame is planned at this base and, target-bound, has it as its cycle
            }
            return out;
        };

        // From the capture's latched 11.4 ms: the first full window puts the base at the top of the band, 7.97 ms,
        // it stays exactly there and never leaves the band (role 4, the same under frame generation).
        for (const uint8_t role : {kUnifiedQueueRoleRenderQueue, kUnifiedQueueRoleFgRenderQueue}) {
            const Settled latched = settle(role, 11400.0);
            CHECK(Near(latched.first, delta + lag, 1e-9) && Near(latched.last, delta + lag, 1e-9));
            CHECK(latched.low >= delta - 1e-9 && latched.high <= delta + lag + 1e-9);
        }
        // Started inside the band no block is seen: the interval has gain 1 on itself and stays where it is (the
        // neutral band).
        const Settled inside = settle(kUnifiedQueueRoleRenderQueue, 7800.0);
        CHECK(Near(inside.first, 7800.0, 1e-9) && Near(inside.last, 7800.0, 1e-9));
        CHECK(inside.low >= delta - 1e-9 && inside.high <= delta + lag + 1e-9);
        // The presenting queue and the frame generation input from the same latch: the block is the common level
        // of their window from the first record on, so they leave the latch with the first full window as well.
        for (const uint8_t role : {uint8_t{1}, uint8_t{2}}) {
            const Settled held = settle(role, 11400.0);
            CHECK(Near(held.first, delta + lag, 1e-9) && Near(held.last, delta + lag, 1e-9));
            CHECK(held.low >= delta - 1e-9 && held.high <= delta + lag + 1e-9);
        }
    }

    void TestRole3CreditFollowsTheReportRule()
    {
        // Role 3 (the adopted queue is a render queue found by dependency, its hold block the order Wait: 0 here): the
        // law is in use (I_gpu, base, q as usual) but the credit and the holds follow the report-based
        // rule: with the hold share 0 the credit and the grants are those of the same stream with the
        // law in fallback (law off here: only the reports), no hold is granted or debited by the law,
        // and the frames carry flag 0x20000000 and are counted.
        const auto run = [](Rig* rig, bool records, std::vector<double>* credits, std::vector<double>* advances) {
            Rig::Planned frame;
            for (int i = 0; i < 4; ++i) {
                rig->Plan(&frame);
                rig->Evidence(frame, 6000.0);
            }
            if (records)
                rig->Records(RoleRecord(Record(8000.0, 0.0, 500.0), 3), 12); // keyless: in use from the 12th
            for (int i = 0; i < 24; ++i) {
                const UnifiedHoldGrant grant = rig->Plan(&frame);
                credits->push_back(grant.credit_us);
                advances->push_back(grant.advance_us);
                rig->Evidence(frame, 6000.0, i % 3 == 0 ? 1500.0 : 0.0);
                if (records)
                    rig->pacer.OnQueueRecord(RoleRecord(Record(8000.0, 0.0, 500.0), 3));
            }
        };
        Rig with_law(true);
        Rig fallback(false);
        std::vector<double> law_credits, law_advances, fallback_credits, fallback_advances;
        run(&with_law, true, &law_credits, &law_advances);
        run(&fallback, false, &fallback_credits, &fallback_advances);
        CHECK(law_credits.size() == fallback_credits.size());
        for (size_t i = 0; i < law_credits.size() && i < fallback_credits.size(); ++i)
            CHECK(law_credits[i] == fallback_credits[i] && law_advances[i] == fallback_advances[i]);
        // The plan's own counting needs the current status (as in the render-limited test): the last
        // frame is counted once more here.
        Rig::Planned counted{with_law.grid, Key(with_law.grid)};
        with_law.pacer.OnPlan(counted.grid, counted.key, kStart);
        const UnifiedQueueLawSummary s = with_law.pacer.QueueLawSummary();
        CHECK(s.frames_in_use > 0 && s.frames_no_hold_credit == 1);
        CHECK(s.grants_law == 0 && s.holds_debited_record == 0 && s.holds_never_debited == 0);
        CHECK((with_law.last_status.flags & kUpQueueLawNoHoldCredit) != 0 && (with_law.last_status.flags & kUpQueueLaw) != 0);
        // Role 1 and role 0 (unknown): the law's own credit rule, no such flag.
        for (const uint8_t role : {uint8_t{0}, uint8_t{1}, uint8_t{2}}) {
            Rig rig(true);
            Rig::Planned frame;
            for (int i = 0; i < 4; ++i) {
                rig.Plan(&frame);
                rig.Evidence(frame, 6000.0);
            }
            rig.Records(RoleRecord(Record(8000.0, 0.0, 500.0), role), 12);
            rig.Plan(&frame);
            CHECK((rig.last_status.flags & kUpQueueLawNoHoldCredit) == 0 && (rig.last_status.flags & kUpQueueLaw) != 0);
            CHECK(rig.pacer.QueueLawSummary().grants_law == 1 && rig.pacer.QueueLawSummary().frames_no_hold_credit == 0);
        }

        // Switching role: the credit stays continuous and no hold is spent twice. A hold granted by the
        // law (role 0) is not debited by the report rule, and a role-3 record pays no grant (its hold block,
        // when it has one, is the order Wait), so no record will ever pay it: the measured hold (3000) settles it ONCE from the
        // report side at the switch (SettleOrphanedLawHolds), and the next grant (by the report rule)
        // sees the credit lower by exactly that hold. The role-3 record itself neither refills nor
        // debits.
        Rig rig(true);
        Rig::Planned frame;
        for (int i = 0; i < 4; ++i) {
            rig.Plan(&frame);
            rig.Evidence(frame, 10000.0);
        }
        rig.Records(Record(8000.0, 0.0, 500.0), 12);
        Rig::Planned first;
        const UnifiedHoldGrant law_grant = rig.Plan(&first);
        rig.Evidence(first, 10000.0, 3000.0);
        CHECK(rig.pacer.QueueLawSummary().grants_law == 1 && rig.pacer.QueueLawSummary().holds_debited_report == 0);
        rig.pacer.OnQueueRecord(RoleRecord(Keyed(Record(8000.0, 0.0, 500.0), 6), 3));
        Rig::Planned second;
        const UnifiedHoldGrant report_grant = rig.Plan(&second);
        CHECK(report_grant.live && law_grant.live &&
            Near(report_grant.credit_us, std::max(-kJ, law_grant.credit_us - 3000.0)));
        CHECK((rig.last_status.flags & kUpQueueLawNoHoldCredit) != 0);
        const UnifiedQueueLawSummary sw = rig.pacer.QueueLawSummary();
        CHECK(sw.grants_law == 1 && sw.holds_debited_report == 0 && sw.holds_debited_record == 0 &&
            sw.holds_settled_orphan == 1);
    }

    // The carry votes (UnifiedCarryVotes): a frame is judged from the bounds of its tail (readiness minus the watched
    // queue's last completion) and of its cycle, and the law is refused only on fresh evidence.
    void TestCarryVotesJudgeAndRefuse()
    {
        using V = UnifiedCarryVotes::Vote;
        // Resident Evil Requiem (N.180): role 4 finished 18.1 to 18.4 ms before the frame was ready, cycle 20.4 ms; the
        // root (role 2) within 30 us of it.
        CHECK(UnifiedCarryVotes::Judge(18100.0, 18400.0, 20300.0, 20500.0) == V::NotCarried);
        CHECK(UnifiedCarryVotes::Judge(-30.0, 30.0, 20300.0, 20500.0) == V::Carried);
        // Carried only when the largest tail is under half the smallest cycle, not carried only when the smallest tail
        // reaches half the largest cycle; a tail that straddles both is undecided.
        CHECK(UnifiedCarryVotes::Judge(9000.0, 9999.0, 20000.0, 20400.0) == V::Carried);
        CHECK(UnifiedCarryVotes::Judge(9000.0, 10000.0, 20000.0, 20400.0) == V::Undecided);
        CHECK(UnifiedCarryVotes::Judge(10200.0, 10300.0, 20000.0, 20400.0) == V::NotCarried);
        CHECK(UnifiedCarryVotes::Judge(10199.0, 10300.0, 20000.0, 20400.0) == V::Undecided);
        // Bounds that say nothing are undecided.
        CHECK(UnifiedCarryVotes::Judge(1.0, 2.0, 0.0, 10.0) == V::Undecided);       // no cycle
        CHECK(UnifiedCarryVotes::Judge(2.0, 1.0, 100.0, 200.0) == V::Undecided);    // tail lo > hi
        CHECK(UnifiedCarryVotes::Judge(1.0, 2.0, 200.0, 100.0) == V::Undecided);    // cycle lo > hi
        CHECK(UnifiedCarryVotes::Judge(NAN, 1.0, 100.0, 200.0) == V::Undecided);

        UnifiedCarryVotes votes;
        for (uint64_t index = 1; index <= 7; ++index)
            votes.Note(V::NotCarried, index);
        CHECK(votes.Decided() == 7 && !votes.Refuses(8, 32)); // fewer than 8 decided: no evidence, the law decides
        votes.Note(V::Undecided, 8);
        CHECK(votes.Decided() == 7 && !votes.Refuses(9, 32)); // an undecided frame abstains
        votes.Note(V::NotCarried, 9);
        CHECK(votes.Refuses(10, 32));
        CHECK(votes.Refuses(9 + 32, 32) && !votes.Refuses(9 + 33, 32)); // stale 32 planned frames after the newest vote
        // 8 carried join: 8 of the 16 not carried, a tie, refuses (a non-carrying queue costs latency); the next carried
        // vote pushes the oldest not-carried one out.
        for (uint64_t index = 10; index < 18; ++index)
            votes.Note(V::Carried, index);
        CHECK(votes.Decided() == 16 && votes.NotCarried() == 8 && votes.Refuses(18, 32));
        votes.Note(V::Carried, 18);
        CHECK(votes.Decided() == 16 && votes.NotCarried() == 7 && !votes.Refuses(19, 32));
        votes.Reset();
        CHECK(votes.Decided() == 0 && votes.NotCarried() == 0 && !votes.Refuses(1, 32));
    }

    void TestRetiredSessionAndRole3HoldsAreSettledOnce()
    {
        // A law grant holds 3 ms and its measured hold is known, but its successor record never
        // comes: the probe session was retired by a re-adoption (or the newest record's queue became a
        // render queue by dependency, whose hold block is the order Wait, no grant's). The measured hold settles it once,
        // from the report side; the record path cannot spend it again; a late record of the retired
        // session is dropped (counted) and does neither.
        const auto prepare = [](Rig* rig, uint64_t session, Rig::Planned* first, UnifiedHoldGrant* grant) {
            Rig::Planned frame;
            for (int i = 0; i < 4; ++i) {
                rig->Plan(&frame);
                rig->Evidence(frame, 10000.0);
            }
            for (int i = 0; i < 12; ++i)
                rig->pacer.OnQueueRecord(Session(Record(8000.0, 0.0, 500.0), session));
            *grant = rig->Plan(first);
        };
        {   // 1. A new session, the hold already measured.
            Rig rig(true);
            Rig::Planned first;
            UnifiedHoldGrant law_grant;
            prepare(&rig, 7, &first, &law_grant);
            rig.Evidence(first, 10000.0, 3000.0); // hold 3000 measured; law-granted: not spent by the report rule
            // (the summary is not read here: it counts a measured, unpaid law grant as never debited at
            // the moment it is taken, which is what the settling below then pays)
            // Session 8: its first record carries frame 5's hold (3000): the bump settles the unpaid
            // grant from the measured hold, then the record finds it spent.
            rig.pacer.OnQueueRecord(Named(Keyed(Record(8000.0, 0.0, 500.0, 3000.0), 6, 8), law_grant.law_sequence));
            UnifiedQueueLawSummary s = rig.pacer.QueueLawSummary();
            CHECK(s.grants_law == 1 && s.holds_debited_report == 0);
            CHECK(s.holds_settled_orphan == 1 && s.holds_debited_record == 0 && s.holds_never_debited == 0);
            // A late record of the retired session 7 (its hold block 3000): dropped, counted, no debit.
            rig.pacer.OnQueueRecord(Named(Keyed(Record(8000.0, 0.0, 500.0, 3000.0), 6, 7), law_grant.law_sequence));
            s = rig.pacer.QueueLawSummary();
            CHECK(s.records_retired == 1 && s.holds_settled_orphan == 1 && s.holds_debited_record == 0);
            Rig::Planned second;
            const UnifiedHoldGrant next = rig.Plan(&second);
            CHECK(next.live); // precondition: a skipped credit assertion must not pass silently
            CHECK(Near(next.credit_us, std::max(-kJ, law_grant.credit_us - 3000.0))); // spent once
        }
        {   // 2. The queue becomes a render queue by dependency (role 3) with the hold already measured.
            Rig rig(true);
            Rig::Planned first;
            UnifiedHoldGrant law_grant;
            prepare(&rig, 0, &first, &law_grant);
            rig.Evidence(first, 10000.0, 3000.0);
            rig.pacer.OnQueueRecord(RoleRecord(Record(8000.0, 0.0, 500.0), 3));
            const UnifiedQueueLawSummary s = rig.pacer.QueueLawSummary();
            CHECK(s.holds_settled_orphan == 1 && s.holds_never_debited == 0 && s.holds_debited_record == 0);
        }
        {   // 3. Orphaned before its hold is measured: the report side settles it when it arrives.
            Rig rig(true);
            Rig::Planned first;
            UnifiedHoldGrant law_grant;
            prepare(&rig, 0, &first, &law_grant);
            rig.pacer.OnQueueRecord(RoleRecord(Record(8000.0, 0.0, 500.0), 3));
            CHECK(rig.pacer.QueueLawSummary().holds_settled_orphan == 0);
            rig.Evidence(first, 10000.0, 3000.0);
            const UnifiedQueueLawSummary s = rig.pacer.QueueLawSummary();
            CHECK(s.holds_settled_orphan == 1 && s.holds_debited_report == 0 && s.holds_never_debited == 0);
        }
    }

    // The law grants of `holds.size()` frames planned with the law in use, each hold (µs) measured by the report
    // side; then one probe record per frame in `arrival_order` whose hold block is its hold when `carry`, naming
    // the grant's sequence when name[i] (else sequence 0), before the measurements when `records_first`; then
    // optionally 64 more planned frames (ring pressure: the first slots are reused). Returns the credit the next
    // grant sees and the summary.
    double RunLawHolds(const std::vector<double>& holds, const std::vector<size_t>& arrival_order,
        const std::vector<bool>& name, bool carry, bool records_first, bool pressure, UnifiedQueueLawSummary* summary)
    {
        Rig rig(true);
        Rig::Planned frame;
        for (int i = 0; i < 4; ++i) {
            rig.Plan(&frame);
            rig.Evidence(frame, 10000.0);
        }
        rig.Records(Record(8000.0, 0.0, 500.0), 12);
        std::vector<UnifiedHoldGrant> grants;
        std::vector<Rig::Planned> planned(holds.size());
        for (size_t i = 0; i < holds.size(); ++i) {
            grants.push_back(rig.Plan(&planned[i]));
            CHECK(grants.back().live && grants.back().law_sequence != 0);
        }
        const auto measure = [&] {
            for (size_t i = 0; i < holds.size(); ++i)
                rig.Evidence(planned[i], 10000.0, holds[i]);
        };
        if (!records_first)
            measure();
        for (const size_t index : arrival_order) {
            const uint64_t sequence = name[index] ? grants[index].law_sequence : 0;
            rig.pacer.OnQueueRecord(Named(Record(8000.0, 0.0, 500.0, carry ? holds[index] : 0.0), sequence));
        }
        if (records_first)
            measure();
        UnifiedHoldGrant next{};
        const int plans = pressure ? 64 : 1;
        for (int i = 0; i < plans; ++i) {
            next = rig.Plan(&frame);
            if (pressure)
                rig.Evidence(frame, 10000.0);
        }
        CHECK(next.live);
        *summary = rig.pacer.QueueLawSummary();
        return next.credit_us;
    }

    // What carrying the holds in the records changes in the credit against records with hold block 0: each named hold is
    // debited beyond the cap's slack J - S^ at its record, and each record's hold also lowers the service S^ (the mean over
    // the records so far: 12 warm-up records of 8 ms, then these in arrival order), which raises that record's refill and
    // its slack by the cumulative hold over the record count.
    double NetOfRecordHolds(const std::vector<double>& holds, const std::vector<size_t>& arrival_order)
    {
        double debited = 0.0, gained = 0.0, cumulative = 0.0;
        int records = 12;
        for (const size_t index : arrival_order) {
            cumulative += holds[index];
            const double lowered = cumulative / static_cast<double>(++records); // S^ below the control's 8 ms
            gained += lowered;
            debited += std::max(0.0, holds[index] - (kJ - 8000.0 + lowered));
        }
        return debited - gained;
    }

    void TestLawHoldIdentity()
    {
        UnifiedQueueLawSummary named{}, control{};
        // Two holds outstanding (3000, 1000), their records arrive out of order, each names its own grant:
        // each debits its own hold beyond the slack, once. The control (the same records with hold block 0) shows the
        // difference is those excesses less what the holds raise the refills by (NetOfRecordHolds).
        const std::vector<double> two = {3000.0, 1000.0};
        double with = RunLawHolds(two, {1, 0}, {true, true}, true, false, false, &named);
        double without = RunLawHolds(two, {1, 0}, {true, true}, false, false, false, &control);
        CHECK(Near(without - with, NetOfRecordHolds(two, {1, 0}), 1e-3));
        CHECK(named.holds_debited_record == 2 && named.holds_settled_orphan == 0 && named.holds_never_debited == 0);
        CHECK(control.holds_debited_record == 0 && control.holds_never_debited == 0);

        // A zero hold between two real ones: its record carries no sequence (the hold was not announced)
        // and debits nothing; the real ones debit their own (beyond the slack, less the refill), never 3 grants' worth.
        const std::vector<double> three = {3000.0, 0.0, 1000.0};
        with = RunLawHolds(three, {0, 1, 2}, {true, false, true}, true, false, false, &named);
        without = RunLawHolds(three, {0, 1, 2}, {true, false, true}, false, false, false, &control);
        CHECK(Near(without - with, NetOfRecordHolds(three, {0, 1, 2}), 1e-3));
        CHECK(named.holds_debited_record == 2 && named.holds_never_debited == 0);

        // The record arrives BEFORE the hold's measurement (the feed was drained first): it names the
        // grant while it is unmeasured and its own hold block beyond the slack is the debit (less the refill, against the
        // control record with hold block 0); the later measurement does not debit again, nor does the ring
        // pressure that follows (the grant is marked paid: never settled, never counted as unpaid).
        const std::vector<double> one = {3000.0};
        with = RunLawHolds(one, {0}, {true}, true, true, false, &named);
        without = RunLawHolds(one, {0}, {true}, false, true, false, &control);
        CHECK(Near(without - with, NetOfRecordHolds(one, {0}), 1e-3));
        CHECK(named.holds_debited_record == 1 && named.holds_settled_orphan == 0 && named.holds_never_debited == 0);
        RunLawHolds(one, {0}, {true}, true, true, true, &named);
        CHECK(named.holds_debited_record == 1 && named.holds_settled_orphan == 0 && named.holds_never_debited == 0);

        // A record with sequence 0 and a measured unpaid grant outstanding debits NOTHING and leaves the
        // grant for settling: before the ring pressure it is unpaid (counted so by a summary read), after
        // it the eviction settles it once. The named record pays the same hold once, early; the settled
        // run can never end LOWER than the early one (that would be a second spend) nor higher by more than
        // the one hold (the early debit is refilled by the pressure frames before the late one can be).
        UnifiedQueueLawSummary before{}, after{};
        RunLawHolds(one, {0}, {false}, true, false, false, &before);
        CHECK(before.holds_debited_record == 0 && before.holds_never_debited == 1);
        const double settled_credit = RunLawHolds(one, {0}, {false}, true, false, true, &after);
        CHECK(after.holds_debited_record == 0 && after.holds_settled_orphan == 1 && after.holds_never_debited == 0);
        const double named_credit = RunLawHolds(one, {0}, {true}, true, false, true, &named);
        CHECK(named.holds_debited_record == 1 && named.holds_settled_orphan == 0);
        CHECK(named_credit >= settled_credit - 1e-6 && named_credit - settled_credit <= 3000.0 + 1e-6);
    }

    void TestUnmeasuredLawGrantLeavingTheRingIsCounted()
    {
        // A law grant that no record names and whose hold no report measures leaves the ring after 64 frames:
        // its hold is never paid, and the summary says so (the credit stays that much too high). A measured
        // grant, or one a record named, is not counted.
        Rig rig(true);
        Rig::Planned frame;
        for (int i = 0; i < 4; ++i) {
            rig.Plan(&frame);
            rig.Evidence(frame, 10000.0);
        }
        rig.Records(Record(8000.0, 0.0, 500.0), 12);
        Rig::Planned lost;
        const UnifiedHoldGrant grant = rig.Plan(&lost);
        CHECK(grant.live && grant.law_sequence != 0);
        for (int i = 0; i < 64; ++i) {
            rig.Plan(&frame);
            rig.Evidence(frame, 10000.0);
        }
        const UnifiedQueueLawSummary s = rig.pacer.QueueLawSummary();
        CHECK(s.holds_lost_unmeasured == 1 && s.holds_never_debited == 0 && s.holds_settled_orphan == 0);
    }

    void TestLawTurnedOffSettlesAnOutstandingLawGrant()
    {
        // A law grant (hold 3000 or, as the control, 0) is outstanding when the law key goes off. Measured
        // before the key goes off: settled at the transition, once. Measured after: marked orphaned and spent
        // by the report side when the measurement arrives, once. Turning the law back on does not pay it again
        // (the credit stays 3000 below the control's), and a record naming it debits nothing.
        for (const bool measured_before : {true, false}) {
            double credit[2] = {0.0, 0.0};
            UnifiedQueueLawSummary summary[2];
            for (const int run : {0, 1}) { // 0: hold 3000, 1: control hold 0
                const double hold = run == 0 ? 3000.0 : 0.0;
                Rig rig(true);
                Rig::Planned frame;
                for (int i = 0; i < 4; ++i) {
                    rig.Plan(&frame);
                    rig.Evidence(frame, 10000.0);
                }
                rig.Records(Record(8000.0, 0.0, 500.0), 12);
                Rig::Planned held;
                const UnifiedHoldGrant grant = rig.Plan(&held);
                CHECK(grant.live && grant.law_sequence != 0);
                if (measured_before)
                    rig.Evidence(held, 10000.0, hold);
                UnifiedOptions options = rig.pacer.Options();
                options.queue_law = false;
                rig.pacer.SetOptions(options);
                if (!measured_before)
                    rig.Evidence(held, 10000.0, hold);
                const UnifiedHoldGrant off_grant = rig.Plan(&frame);
                CHECK(off_grant.live);
                options.queue_law = true;
                rig.pacer.SetOptions(options);
                rig.pacer.OnQueueRecord(Named(Record(8000.0, 0.0, 500.0, hold), grant.law_sequence));
                const UnifiedHoldGrant on_grant = rig.Plan(&frame);
                CHECK(on_grant.live);
                credit[run] = on_grant.credit_us;
                summary[run] = rig.pacer.QueueLawSummary();
            }
            CHECK(Near(credit[1] - credit[0], 3000.0)); // paid exactly once: not zero times, not twice
            CHECK(summary[0].holds_debited_record == 0 && summary[0].holds_never_debited == 0);
            // (counted only while the key was on: the transition settles a measured hold; a measurement that
            // arrives with the key off is spent without a count)
            CHECK(summary[0].holds_settled_orphan == (measured_before ? 1u : 0u));
        }
        // Key never on: the transition code does not run and nothing is settled or counted.
        Rig never(false);
        Rig::Planned frame;
        for (int i = 0; i < 4; ++i) {
            never.Plan(&frame);
            never.Evidence(frame, 10000.0, 1000.0);
        }
        UnifiedOptions options = never.pacer.Options();
        options.queue_rank = 3;
        never.pacer.SetOptions(options);
        CHECK(never.pacer.QueueLawSummary().holds_settled_orphan == 0);
    }

    void TestExplicitSessionRetirement()
    {
        // The probe's adopted queue is destroyed (session 7): the law's window is emptied at once (no
        // waiting for four planned frames), its outstanding law grant (hold 3000, measured) is settled
        // once, a later drained record of session 7 is dropped and counted, an older or repeated
        // retirement changes nothing, and the first record of session 8 starts afresh.
        Rig rig(true);
        Rig::Planned frame;
        for (int i = 0; i < 4; ++i) {
            rig.Plan(&frame);
            rig.Evidence(frame, 10000.0);
        }
        for (int i = 0; i < 12; ++i)
            rig.pacer.OnQueueRecord(Session(Record(8000.0, 0.0, 500.0), 7));
        Rig::Planned held;
        const UnifiedHoldGrant grant = rig.Plan(&held);
        CHECK(grant.live && grant.law_sequence != 0 && rig.pacer.QueueLawInUse());
        rig.Evidence(held, 10000.0, 3000.0);
        rig.pacer.RetireQueueSession(7);
        CHECK(!rig.pacer.QueueLawInUse());
        UnifiedQueueLawSummary s = rig.pacer.QueueLawSummary();
        CHECK(s.holds_settled_orphan == 1 && s.holds_never_debited == 0);
        rig.pacer.OnQueueRecord(Named(Session(Record(8000.0, 0.0, 500.0, 3000.0), 7), grant.law_sequence));
        s = rig.pacer.QueueLawSummary();
        CHECK(s.records_retired == 1 && s.records_accepted == 12 && s.holds_debited_record == 0);
        rig.pacer.RetireQueueSession(6); // older: nothing
        rig.pacer.RetireQueueSession(7); // already retired: nothing (no second settling)
        CHECK(rig.pacer.QueueLawSummary().holds_settled_orphan == 1);
        rig.pacer.OnQueueRecord(Session(Record(8000.0, 0.0, 500.0), 8));
        rig.Plan(&frame);
        CHECK(rig.last_status.ql_valid_frames == 1 && !rig.pacer.QueueLawInUse());
        CHECK(rig.pacer.QueueLawSummary().records_accepted == 13);
        // A retirement of a session the pacer never saw retires it ahead of time.
        rig.pacer.RetireQueueSession(9);
        rig.pacer.OnQueueRecord(Session(Record(8000.0, 0.0, 500.0), 9));
        CHECK(rig.pacer.QueueLawSummary().records_retired == 2);
        // Law off: a retirement does nothing.
        Rig off(false);
        off.pacer.RetireQueueSession(3);
        CHECK(off.pacer.QueueLawSummary().records_retired == 0);

        // Law off then on: the pacer forgot the session, the drain tells it again (the watermark is applied on
        // every active drain, the call is idempotent), and a late record of the retired session is dropped.
        Rig toggled(true);
        for (int i = 0; i < 12; ++i)
            toggled.pacer.OnQueueRecord(Session(Record(8000.0, 0.0, 500.0), 7));
        toggled.pacer.RetireQueueSession(7);
        UnifiedOptions toggle_options = toggled.pacer.Options();
        toggle_options.queue_law = false;
        toggled.pacer.SetOptions(toggle_options);
        toggle_options.queue_law = true;
        toggled.pacer.SetOptions(toggle_options);
        // (a late record before the drain re-applies the watermark would be accepted: the drain applies it first)
        toggled.pacer.RetireQueueSession(7);
        toggled.pacer.RetireQueueSession(7); // every drain: idempotent
        toggled.pacer.OnQueueRecord(Session(Record(8000.0, 0.0, 500.0), 7));
        CHECK(toggled.pacer.QueueLawSummary().records_retired == 1);
        toggled.pacer.OnQueueRecord(Session(Record(8000.0, 0.0, 500.0), 8));
        CHECK(toggled.pacer.QueueLawSummary().records_retired == 1 && !toggled.pacer.QueueLawInUse());

        // The feed's retired-session value only grows, from any thread.
        CHECK(GpuQueueLawFeed_RetiredSession() == 0 || GpuQueueLawFeed_RetiredSession() > 0);
        const uint64_t base = GpuQueueLawFeed_RetiredSession();
        GpuQueueLawFeed_RetireSession(static_cast<uint32_t>(base + 5));
        GpuQueueLawFeed_RetireSession(static_cast<uint32_t>(base + 3));
        CHECK(GpuQueueLawFeed_RetiredSession() == base + 5);
        GpuQueueLawFeed_RetireSession(static_cast<uint32_t>(base + 9));
        CHECK(GpuQueueLawFeed_RetiredSession() == base + 9);
    }

    void TestLateRecordNamingASettledGrantDoesNothing()
    {
        // A law grant (hold 3000, its record never came) is settled at its ring eviction, 64 frames later.
        // A late record that names its sequence finds no entry and debits nothing (the identity is the only
        // correlation); a record naming a sequence that was never granted does nothing either.
        Rig rig(true);
        Rig::Planned frame;
        for (int i = 0; i < 4; ++i) {
            rig.Plan(&frame);
            rig.Evidence(frame, 10000.0);
        }
        rig.Records(Record(8000.0, 0.0, 500.0), 12);
        Rig::Planned first;
        const UnifiedHoldGrant law_grant = rig.Plan(&first);
        CHECK(law_grant.live && law_grant.law_sequence != 0);
        rig.Evidence(first, 10000.0, 3000.0);
        UnifiedHoldGrant grant{};
        for (int i = 0; i < 64; ++i) {
            grant = rig.Plan(&frame);
            rig.Evidence(frame, 10000.0);
        }
        CHECK(grant.live && Near(grant.credit_us, std::max(-kJ, law_grant.credit_us - 3000.0)));
        const UnifiedQueueLawSummary before = rig.pacer.QueueLawSummary();
        CHECK(before.holds_settled_orphan == 1 && before.holds_debited_record == 0 && before.holds_never_debited == 0);
        rig.pacer.OnQueueRecord(Named(Keyed(Record(8000.0, 0.0, 500.0, 3000.0), 1006), law_grant.law_sequence));
        rig.pacer.OnQueueRecord(Named(Record(8000.0, 0.0, 500.0, 3000.0), law_grant.law_sequence));
        rig.pacer.OnQueueRecord(Named(Record(8000.0, 0.0, 500.0, 3000.0), 987654321u));
        const UnifiedQueueLawSummary after = rig.pacer.QueueLawSummary();
        CHECK(after.records_accepted == before.records_accepted + 3);
        CHECK(after.holds_debited_record == 0 && after.holds_settled_orphan == 1 && after.holds_never_debited == 0);
    }

    // ── The feed ──

    void TestFeedRing()
    {
        GpuQueueLawFeed_Reset();
        PacingGpuQueueRecord record{};
        CHECK(!GpuQueueLawFeed_Pop(&record) && !GpuQueueLawFeed_Pop(nullptr));
        // FIFO order, and 64 fit.
        const uint64_t dropped0 = GpuQueueLawFeed_Dropped();
        for (uint64_t i = 1; i <= 64; ++i) {
            PacingGpuQueueRecord in{};
            in.present_sequence = i;
            in.cycle_us = static_cast<double>(i);
            GpuQueueLawFeed_Push(in);
        }
        CHECK(GpuQueueLawFeed_Dropped() == dropped0);
        // The 65th is refused and counted, the ring is not disturbed.
        PacingGpuQueueRecord extra{};
        extra.present_sequence = 999;
        GpuQueueLawFeed_Push(extra);
        CHECK(GpuQueueLawFeed_Dropped() == dropped0 + 1);
        for (uint64_t i = 1; i <= 64; ++i) {
            CHECK(GpuQueueLawFeed_Pop(&record) && record.present_sequence == i && record.cycle_us == static_cast<double>(i));
        }
        CHECK(!GpuQueueLawFeed_Pop(&record));
        // A freed slot is usable again, across the wrap.
        for (uint64_t i = 0; i < 200; ++i) {
            PacingGpuQueueRecord in{};
            in.present_sequence = 1000 + i;
            GpuQueueLawFeed_Push(in);
            CHECK(GpuQueueLawFeed_Pop(&record) && record.present_sequence == 1000 + i);
        }
        // Reset discards what is queued and keeps the drop count.
        for (uint64_t i = 0; i < 10; ++i)
            GpuQueueLawFeed_Push(extra);
        const uint64_t dropped = GpuQueueLawFeed_Dropped();
        GpuQueueLawFeed_Reset();
        CHECK(!GpuQueueLawFeed_Pop(&record) && GpuQueueLawFeed_Dropped() == dropped);
        GpuQueueLawFeed_Push(extra);
        CHECK(GpuQueueLawFeed_Pop(&record) && record.present_sequence == 999);
    }

    void TestFeedProducerConsumer()
    {
        // One producer thread, the consumer here: order is kept, every record is popped or counted.
        GpuQueueLawFeed_Reset();
        constexpr uint64_t kCount = 200000;
        const uint64_t dropped0 = GpuQueueLawFeed_Dropped();
        std::thread producer([] {
            for (uint64_t i = 1; i <= kCount; ++i) {
                PacingGpuQueueRecord in{};
                in.present_sequence = i;
                in.first_submit_qpc = static_cast<int64_t>(i) * 3; // a payload that must arrive intact
                GpuQueueLawFeed_Push(in);
            }
        });
        uint64_t popped = 0;
        uint64_t last = 0;
        bool ordered = true;
        bool intact = true;
        PacingGpuQueueRecord record{};
        while (popped + (GpuQueueLawFeed_Dropped() - dropped0) < kCount) {
            if (GpuQueueLawFeed_Pop(&record)) {
                ordered = ordered && record.present_sequence > last;
                intact = intact && record.first_submit_qpc == static_cast<int64_t>(record.present_sequence) * 3;
                last = record.present_sequence;
                ++popped;
            } else {
                std::this_thread::yield();
            }
        }
        producer.join();
        CHECK(ordered && intact);
        CHECK(popped + (GpuQueueLawFeed_Dropped() - dropped0) == kCount);
        CHECK(!GpuQueueLawFeed_Pop(&record));
    }
}

int main()
{
    std::printf("=== Unified queue law tests ===\n");
    TestSampleFromRecord();
    TestWindowCacheReplay();
    TestOrderStatisticsMedianAndGuard();
    TestPositivePartBoundAndClamp();
    TestGpuBoundPositiveMarginShrinks();
    TestStarvedIntervalBelowBase();
    TestHeldCapWithSlack();
    TestCapRemovedIntervalFallsEveryFrame();
    TestLongerCycleEngagesTheGuard();
    TestSigmaAllowance();
    TestAllowanceUsesStarvationFreeService();
    TestHoldStepAtSaturationIsPaidOnlyByTheShare();
    TestUnpacedStarvationIsNotInTheBase();
    TestPacedStarvationCountsOnlyUpToTheStartWait();
    TestCapRemovedReachesTheNeedWithinFFrames();
    TestPacedNeedFallIsFollowedWithinFFrames();
    TestIntervalExcessLeavesWhicheverFaceItTakes();
    TestNeedMeanIsUnbiasedWhenGpuPaced();
    TestNeedFramesSixteenIsTheMeanCycle();
    TestHeldCapRefillUsesTheService();
    TestInUseRules();
    TestCreditContinuousAcrossRuleSwitches();
    TestLawHoldSpendsComeFromRecords();
    TestRecordDebitAndBounds();
    TestUncomputableWindowRefillsNothingAndDebitsTheWholeHold();
    TestLawShareRefillIsBoundedByFrameGenerationTime();
    TestShareRefillStopsWhenTheReportsExpire();
    TestShareDecreaseDropsTheBankedSurplus();
    TestLawLivesWithoutReports();
    TestKeyOffAndNotInUseAreIdentical();
    TestStatusFlagsAndStatistics();
    TestPlannedFramesAreCountedOncePerIndex();
    TestOptionsAndToggle();
    TestReservationUsesTheLaw();
    TestHoldGrantedUnderFallbackIsDebitedOnceByIt();
    TestHoldGrantedUnderLawIsDebitedOnceByItsRecord();
    TestFreshnessIsMeasuredInFrames();
    TestDelayedBatchOfOldRecordsStaysFallback();
    TestProbeSessionRestartsTheWindowAndKeepsTheCredit();
    TestPacedEntriesGateTheStarvationInTheBase();
    TestGameLimitedFramesNeverBindTheGpuTerm();
    TestNeedFramesOption();
    TestWholeIntervalBound();
    TestNonFrameGenerationRecordsTakeTheAttributionKey();
    TestQueueLawSummaryCounters();
    TestBaseCarriesHoldsOnlyBeyondTheCapsSlack();
    TestRenderQueueOrderBlockIsNotNeed();
    TestRenderQueueBaseHasNoGainOnItsOwnOrderBlock();
    TestRetiredSessionAndRole3HoldsAreSettledOnce();
    TestLawHoldIdentity();
    TestLateRecordNamingASettledGrantDoesNothing();
    TestExplicitSessionRetirement();
    TestLawTurnedOffSettlesAnOutstandingLawGrant();
    TestUnmeasuredLawGrantLeavingTheRingIsCounted();
    TestRole3CreditFollowsTheReportRule();
    TestCarryVotesJudgeAndRefuse();
    TestFeedRing();
    TestFeedProducerConsumer();
    std::printf("=== %d failures ===\n", s_failures);
    return s_failures == 0 ? 0 : 1;
}
