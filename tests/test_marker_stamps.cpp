// Directly tests the production stamp ring of the game's own SIMULATION_START, SIMULATION_END,
// RENDERSUBMIT_START and RENDERSUBMIT_END markers (src/marker_stamps.cpp) and the hooks' gate
// (MarkerStamps_Capture and MarkerStamps_Publish in src/marker_stamps.h): stamping and reading by the complete frame key
// (domain, producer epoch, frame id), a later frame replacing a slot, the reset and a hook that
// resumes after it, the duplicate policy (the latest sample of a marker, whole), the ignored inputs,
// the key-off identity (no clock read, no epoch read, nothing stamped), the CPU time of a span (same
// thread only, known rate only, the conversion), the cycle rate's decision logic
// (src/marker_rate.cpp: paired reads, invariant TSC, agreement of the thread cycle counter) and
// consistent reads against concurrent writers on three threads. On Windows it also prints the cost
// of a marker hook's thread sample (QueryThreadCycleTime + GetCurrentThreadId, no assertion on
// speed) and runs the calibration (src/marker_cycles.cpp). The marker hooks, their threads and the
// marker streams of a game need live validation.

#include "marker_rate.h"
#include "marker_stamps.h"

#ifdef _WIN32
#include "marker_cycles.h"
#endif

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>

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

    constexpr int64_t kBase = 5'000'000'000LL;

    MarkerStampKey Key(uint64_t id, uint8_t domain = kMarkerDomainNvApi, uint32_t epoch = 1)
    {
        return MarkerStampKey{ domain, epoch, id };
    }

    // One marker stamped in the ring's current generation.
    void Rec(MarkerStampKind kind, const MarkerStampKey& key, int64_t qpc, uint64_t cycles = 0, uint32_t tid = 0)
    {
        MarkerStamps_Record(key, MarkerStamps_Generation(), kind, MarkerClockSample{ qpc, cycles, tid });
    }

    void TestRecordAndGet()
    {
        MarkerStamps_Reset();
        MarkerStamps stamps{};
        CHECK(!MarkerStamps_Get(Key(100), &stamps));

        Rec(MarkerStampKind::SimulationEnd, Key(100), kBase + 1);
        CHECK(MarkerStamps_Get(Key(100), &stamps));
        CHECK(stamps.frame_id == 100 && stamps.domain == kMarkerDomainNvApi && stamps.producer_epoch == 1 &&
              stamps.sim_end_qpc == kBase + 1 && stamps.render_start_qpc == 0 && stamps.render_end_qpc == 0 &&
              stamps.sim_start_qpc == 0);

        // The four kinds of one frame are independent, whatever their order.
        Rec(MarkerStampKind::RenderSubmitEnd, Key(100), kBase + 30);
        Rec(MarkerStampKind::RenderSubmitStart, Key(100), kBase + 20);
        Rec(MarkerStampKind::SimulationStart, Key(100), kBase - 10, 5, 77);
        CHECK(MarkerStamps_Get(Key(100), &stamps));
        CHECK(stamps.sim_end_qpc == kBase + 1 && stamps.render_start_qpc == kBase + 20 &&
              stamps.render_end_qpc == kBase + 30 && stamps.sim_start_qpc == kBase - 10);

        // Another frame is untouched; the result is untouched when nothing is held.
        CHECK(!MarkerStamps_Get(Key(101), &stamps));
        MarkerStamps untouched{};
        untouched.frame_id = 77;
        CHECK(!MarkerStamps_Get(Key(555), &untouched) && untouched.frame_id == 77);
        // The PacingFrameKey overload takes the domain as it is.
        PacingFrameKey pacing_key{};
        pacing_key.domain = PacingIdentityDomain::NvApiFrame;
        pacing_key.producer_epoch = 1;
        pacing_key.frame_id = 100;
        CHECK(MarkerStamps_Get(pacing_key, &stamps) && stamps.render_end_qpc == kBase + 30);
        pacing_key.domain = PacingIdentityDomain::PclFrame;
        CHECK(!MarkerStamps_Get(pacing_key, &stamps));
        pacing_key.domain = PacingIdentityDomain::Unknown;
        CHECK(!MarkerStamps_Get(pacing_key, &stamps));
    }

    void TestIgnoredInput()
    {
        MarkerStamps_Reset();
        MarkerStamps stamps{};
        Rec(MarkerStampKind::SimulationEnd, Key(0), kBase);                       // frame 0 is the sentinel
        Rec(MarkerStampKind::SimulationEnd, Key(9, 0), kBase);                    // domain 0 is none
        Rec(MarkerStampKind::SimulationEnd, Key(9), 0);                           // 0 means not seen
        Rec(MarkerStampKind::SimulationEnd, Key(9), -5);
        Rec(MarkerStampKind::SimulationStart, Key(9), kBase, 100, 0);             // no thread id
        CHECK(!MarkerStamps_Get(Key(0), &stamps));
        CHECK(!MarkerStamps_Get(Key(9, 0), &stamps));
        CHECK(!MarkerStamps_Get(Key(9), &stamps));
        CHECK(!MarkerStamps_Get(Key(9), nullptr));
        // A stamp of a different kind with a real value then reads with the others 0.
        Rec(MarkerStampKind::RenderSubmitStart, Key(9), kBase + 4);
        CHECK(MarkerStamps_Get(Key(9), &stamps) && stamps.sim_end_qpc == 0 &&
              stamps.render_start_qpc == kBase + 4 && stamps.render_end_qpc == 0);
    }

    // The complete key: the two hooks' ids and the producer epochs are different frames.
    void TestFullKeyRequired()
    {
        MarkerStamps_Reset();
        MarkerStamps stamps{};
        Rec(MarkerStampKind::SimulationEnd, Key(10, kMarkerDomainNvApi, 1), kBase + 1);
        Rec(MarkerStampKind::SimulationEnd, Key(10, kMarkerDomainPcl, 1), kBase + 2);
        // The same id in two domains: each its own frame, neither replaces the other.
        CHECK(MarkerStamps_Get(Key(10, kMarkerDomainNvApi, 1), &stamps) && stamps.sim_end_qpc == kBase + 1);
        CHECK(MarkerStamps_Get(Key(10, kMarkerDomainPcl, 1), &stamps) && stamps.sim_end_qpc == kBase + 2 &&
              stamps.domain == kMarkerDomainPcl);
        // A producer epoch that was not stamped finds nothing, whatever the id.
        CHECK(!MarkerStamps_Get(Key(10, kMarkerDomainNvApi, 2), &stamps));
        CHECK(!MarkerStamps_Get(Key(10, kMarkerDomainPcl, 0), &stamps));
        // The same id of the next epoch takes the first epoch's slot: the old epoch's frame is gone,
        // and nothing of it shows in the new one.
        Rec(MarkerStampKind::RenderSubmitEnd, Key(10, kMarkerDomainNvApi, 2), kBase + 3);
        CHECK(!MarkerStamps_Get(Key(10, kMarkerDomainNvApi, 1), &stamps));
        CHECK(MarkerStamps_Get(Key(10, kMarkerDomainNvApi, 2), &stamps) && stamps.sim_end_qpc == 0 &&
              stamps.render_end_qpc == kBase + 3 && stamps.producer_epoch == 2);
        CHECK(MarkerStamps_Get(Key(10, kMarkerDomainPcl, 1), &stamps) && stamps.sim_end_qpc == kBase + 2);
    }

    void TestLaterFrameReplacesSlot()
    {
        MarkerStamps_Reset();
        MarkerStamps stamps{};
        const uint64_t first = 40;
        const uint64_t later = first + kMarkerStampSlots; // the same slot
        Rec(MarkerStampKind::SimulationEnd, Key(first), kBase + 1);
        Rec(MarkerStampKind::RenderSubmitEnd, Key(first), kBase + 3);
        Rec(MarkerStampKind::RenderSubmitStart, Key(later), kBase + 2);
        // The older frame is gone, and none of its stamps leaks into the later one.
        CHECK(!MarkerStamps_Get(Key(first), &stamps));
        CHECK(MarkerStamps_Get(Key(later), &stamps));
        CHECK(stamps.frame_id == later && stamps.sim_end_qpc == 0 &&
              stamps.render_start_qpc == kBase + 2 && stamps.render_end_qpc == 0);

        // A whole ring of frames: the newest 256 stay, each with its own stamps.
        MarkerStamps_Reset();
        for (uint64_t id = 1000; id < 1000 + 3 * kMarkerStampSlots; ++id) {
            Rec(MarkerStampKind::SimulationEnd, Key(id), kBase + static_cast<int64_t>(id));
            Rec(MarkerStampKind::RenderSubmitEnd, Key(id), kBase + static_cast<int64_t>(id) + 5);
        }
        bool newest_ok = true;
        for (uint64_t id = 1000 + 2 * kMarkerStampSlots; id < 1000 + 3 * kMarkerStampSlots; ++id) {
            newest_ok = newest_ok && MarkerStamps_Get(Key(id), &stamps) && stamps.frame_id == id &&
                stamps.sim_end_qpc == kBase + static_cast<int64_t>(id) &&
                stamps.render_end_qpc == kBase + static_cast<int64_t>(id) + 5 && stamps.render_start_qpc == 0;
        }
        CHECK(newest_ok);
        CHECK(!MarkerStamps_Get(Key(1000), &stamps) && !MarkerStamps_Get(Key(1000 + kMarkerStampSlots), &stamps));
    }

    // Restarted frame ids after a reset, and a hook that was admitted before it.
    void TestReset()
    {
        MarkerStamps stamps{};
        Rec(MarkerStampKind::SimulationEnd, Key(12), kBase + 1);
        Rec(MarkerStampKind::RenderSubmitStart, Key(13), kBase + 2);
        CHECK(MarkerStamps_Get(Key(12), &stamps) && MarkerStamps_Get(Key(13), &stamps));
        const uint32_t admitted = MarkerStamps_Generation(); // a hook admitted here...
        MarkerStamps_Reset();                                // ...a lifecycle reset runs...
        CHECK(MarkerStamps_Generation() != admitted);
        CHECK(!MarkerStamps_Get(Key(12), &stamps) && !MarkerStamps_Get(Key(13), &stamps));
        // ...and the hook resumes: its stamp is not published into the new generation.
        MarkerStamps_Record(Key(12), admitted, MarkerStampKind::RenderSubmitEnd, MarkerClockSample{ kBase + 8, 9, 3 });
        CHECK(!MarkerStamps_Get(Key(12), &stamps));
        // The ring works again; a frame id that restarted does not see its old stamps.
        Rec(MarkerStampKind::RenderSubmitEnd, Key(12), kBase + 9);
        CHECK(MarkerStamps_Get(Key(12), &stamps) && stamps.sim_end_qpc == 0 && stamps.render_end_qpc == kBase + 9);
        // A late hook of the old generation does not overwrite a stamp of the new one.
        MarkerStamps_Record(Key(12), admitted, MarkerStampKind::RenderSubmitEnd, MarkerClockSample{ kBase + 99, 9, 3 });
        CHECK(MarkerStamps_Get(Key(12), &stamps) && stamps.render_end_qpc == kBase + 9);
        MarkerStamps_Reset();
        MarkerStamps_Reset();
        CHECK(!MarkerStamps_Get(Key(12), &stamps));
    }

    // A marker of one frame stamped twice keeps the sample with the larger QPC, whole: the QPC, the
    // cycles and the thread id of one sample, whatever order the claims came in.
    void TestDuplicateKeepsLatestSample()
    {
        MarkerStamps_Reset();
        MarkerStamps stamps{};
        // Reverse order: the newer sample claimed the slot first, the older (its thread was
        // descheduled between sampling and claiming) arrives after.
        Rec(MarkerStampKind::RenderSubmitEnd, Key(70), kBase + 200, 2000, 7);
        Rec(MarkerStampKind::RenderSubmitEnd, Key(70), kBase + 100, 1000, 9);
        CHECK(MarkerStamps_Get(Key(70), &stamps));
        CHECK(stamps.render_end_qpc == kBase + 200 && stamps.cycles[2] == 2000 && stamps.tid[2] == 7);
        // In order: the later sample replaces the earlier whole.
        Rec(MarkerStampKind::RenderSubmitEnd, Key(70), kBase + 300, 3000, 8);
        CHECK(MarkerStamps_Get(Key(70), &stamps));
        CHECK(stamps.render_end_qpc == kBase + 300 && stamps.cycles[2] == 3000 && stamps.tid[2] == 8);
        // An equal QPC changes nothing (the first of the two stays).
        Rec(MarkerStampKind::RenderSubmitEnd, Key(70), kBase + 300, 4000, 5);
        CHECK(MarkerStamps_Get(Key(70), &stamps));
        CHECK(stamps.render_end_qpc == kBase + 300 && stamps.cycles[2] == 3000 && stamps.tid[2] == 8);
        // SIMULATION_START has a QPC of its own to compare.
        Rec(MarkerStampKind::SimulationStart, Key(70), kBase + 50, 500, 4);
        Rec(MarkerStampKind::SimulationStart, Key(70), kBase + 40, 400, 6);
        CHECK(MarkerStamps_Get(Key(70), &stamps));
        CHECK(stamps.sim_start_qpc == kBase + 50 && stamps.cycles[3] == 500 && stamps.tid[3] == 4);
        // The other kinds of the frame are untouched by all of it.
        CHECK(stamps.sim_end_qpc == 0 && stamps.render_start_qpc == 0);
    }

    // The hooks' gate: key off, nothing captured or stamped, no clock and no epoch read; key on, the
    // four markers only, one epoch read at capture and one at publication, one sample read each.
    void TestGate()
    {
        MarkerStamps_Reset();
        int reads = 0;
        int epoch_reads = 0;
        int64_t clock = kBase;
        const auto sample = [&] {
            ++reads;
            ++clock;
            return MarkerClockSample{ clock, static_cast<uint64_t>(1000 + (clock - kBase)), 42u };
        };
        const auto epoch = [&] { ++epoch_reads; return 3u; };
        MarkerStamps stamps{};

        g_unified_queue_upstream.store(0);
        for (uint32_t type = 0; type < 8; ++type) {
            const MarkerStampCapture capture = MarkerStamps_Capture(type, epoch, sample);
            CHECK(!capture.live);
            MarkerStamps_Publish(capture, kMarkerDomainNvApi, 7, epoch);
        }
        CHECK(reads == 0 && epoch_reads == 0 && !MarkerStamps_Get(Key(7, kMarkerDomainNvApi, 3), &stamps));

        for (const int mode : {1}) { // the key is 0 or 1 (an INI with 2 or 3 loads as 1)
            MarkerStamps_Reset();
            reads = 0;
            epoch_reads = 0;
            g_unified_queue_upstream.store(mode);
            for (uint32_t type = 0; type < 8; ++type) {
                const MarkerStampCapture capture = MarkerStamps_Capture(type, epoch, sample);
                CHECK(capture.live == (type < 4));
                MarkerStamps_Publish(capture, kMarkerDomainNvApi, 7, epoch);
            }
            CHECK(reads == 4 && epoch_reads == 8);
            // The epoch the hook read is part of the frame's identity.
            CHECK(!MarkerStamps_Get(Key(7, kMarkerDomainNvApi, 1), &stamps));
            CHECK(!MarkerStamps_Get(Key(7, kMarkerDomainPcl, 3), &stamps));
            CHECK(MarkerStamps_Get(Key(7, kMarkerDomainNvApi, 3), &stamps));
            // SIMULATION_START 0, SIMULATION_END 1, RENDERSUBMIT_START 2, RENDERSUBMIT_END 3 in call order.
            const int64_t first = kBase + 4 * (mode - 1) + 1;
            CHECK(stamps.sim_start_qpc == first && stamps.sim_end_qpc == first + 1 &&
                  stamps.render_start_qpc == first + 2 && stamps.render_end_qpc == first + 3);
            for (uint32_t kind = 0; kind < kMarkerStampKinds; ++kind) {
                // Kinds 0..3 are SimulationEnd, RenderSubmitStart, RenderSubmitEnd, SimulationStart.
                const int64_t read_at = kind == 3 ? first : first + 1 + kind;
                CHECK(stamps.tid[kind] == 42u &&
                      stamps.cycles[kind] == static_cast<uint64_t>(1000 + (read_at - kBase)));
            }
            // PRESENT_START (4), PRESENT_END (5), 6 and 7 stamped nothing, read no clock and no epoch.
            CHECK(!MarkerStamps_Get(Key(8, kMarkerDomainNvApi, 3), &stamps));
        }
        g_unified_queue_upstream.store(0);
    }

    // The lifetime a hook freezes at entry: a call suspended across a reset or an epoch change
    // before it publishes stamps nothing, and a capture that was not live never publishes.
    void TestCapturedLifetime()
    {
        MarkerStamps stamps{};
        uint32_t epoch_now = 5;
        const auto epoch = [&] { return epoch_now; };
        const auto sample = [] { return MarkerClockSample{ kBase + 1, 100, 7 }; };
        g_unified_queue_upstream.store(1);
        MarkerStamps_Reset();

        // An unchanged lifetime publishes, under the captured epoch.
        MarkerStampCapture capture = MarkerStamps_Capture(1, epoch, sample);
        MarkerStamps_Publish(capture, kMarkerDomainNvApi, 20, epoch);
        CHECK(MarkerStamps_Get(Key(20, kMarkerDomainNvApi, 5), &stamps) && stamps.sim_end_qpc == kBase + 1 &&
              stamps.tid[0] == 7);

        // A reset between capture and publication (generation changed): dropped.
        capture = MarkerStamps_Capture(1, epoch, sample);
        MarkerStamps_Reset();
        MarkerStamps_Publish(capture, kMarkerDomainNvApi, 21, epoch);
        CHECK(!MarkerStamps_Get(Key(21, kMarkerDomainNvApi, 5), &stamps));

        // An epoch change between capture and publication: dropped, under neither epoch.
        capture = MarkerStamps_Capture(1, epoch, sample);
        epoch_now = 6;
        MarkerStamps_Publish(capture, kMarkerDomainNvApi, 22, epoch);
        CHECK(!MarkerStamps_Get(Key(22, kMarkerDomainNvApi, 5), &stamps) &&
              !MarkerStamps_Get(Key(22, kMarkerDomainNvApi, 6), &stamps));

        // A call that captured after the change publishes under the new epoch.
        capture = MarkerStamps_Capture(1, epoch, sample);
        MarkerStamps_Publish(capture, kMarkerDomainNvApi, 23, epoch);
        CHECK(MarkerStamps_Get(Key(23, kMarkerDomainNvApi, 6), &stamps) &&
              !MarkerStamps_Get(Key(23, kMarkerDomainNvApi, 5), &stamps));

        // Both changed: still dropped. A reset and a restart of the same epoch value (generation
        // alone) is dropped too.
        capture = MarkerStamps_Capture(1, epoch, sample);
        MarkerStamps_Reset();
        epoch_now = 7;
        MarkerStamps_Publish(capture, kMarkerDomainNvApi, 24, epoch);
        CHECK(!MarkerStamps_Get(Key(24, kMarkerDomainNvApi, 6), &stamps) &&
              !MarkerStamps_Get(Key(24, kMarkerDomainNvApi, 7), &stamps));

        // The call's own rotation (the PCL hook's GetFrameID can begin an epoch for a synthetic
        // ID) between capture and publication: dropped, under neither epoch. The first marker of a
        // resumed lifetime gets no stamp; its old arrival is never relabelled into the new one.
        epoch_now = 8;
        capture = MarkerStamps_Capture(1, epoch, sample);
        epoch_now = 9; // rotated by this very call, as GetFrameID would
        MarkerStamps_Publish(capture, kMarkerDomainPcl, 30, epoch);
        CHECK(!MarkerStamps_Get(Key(30, kMarkerDomainPcl, 8), &stamps) &&
              !MarkerStamps_Get(Key(30, kMarkerDomainPcl, 9), &stamps));
        // A frame suspended across another thread's rotation and the stamp reset that follows it
        // publishes nothing, so the next frame finds no false same-epoch predecessor.
        capture = MarkerStamps_Capture(1, epoch, sample);  // frame 40 at epoch 9, arrival kBase + 1
        epoch_now = 10;                                      // frame 41's SIMULATION_START rotates
        MarkerStamps_Reset();                                // and the scheduler resets the stamps
        MarkerStamps_Publish(capture, kMarkerDomainPcl, 40, epoch);
        CHECK(!MarkerStamps_Get(Key(40, kMarkerDomainPcl, 9), &stamps) &&
              !MarkerStamps_Get(Key(40, kMarkerDomainPcl, 10), &stamps));
        // A capture taken with the key off never publishes, whatever the key is later.
        g_unified_queue_upstream.store(0);
        capture = MarkerStamps_Capture(1, epoch, sample);
        g_unified_queue_upstream.store(1);
        MarkerStamps_Publish(capture, kMarkerDomainNvApi, 25, epoch);
        CHECK(!MarkerStamps_Get(Key(25, kMarkerDomainNvApi, 7), &stamps));
        g_unified_queue_upstream.store(0);
    }

    // The CPU time of a span: the cycle difference over the rate, only from one thread, with a
    // known rate and both markers' samples.
    void TestSpanCpuTime()
    {
        MarkerStamps_Reset();
        constexpr double kRate = 3.0e9;
        MarkerStamps stamps{};
        double cpu_us = -1.0;

        Rec(MarkerStampKind::SimulationStart, Key(50), kBase, 7'000'000, 4812);
        Rec(MarkerStampKind::SimulationEnd, Key(50), kBase + 1, 7'000'000 + 3'000'000, 4812);
        Rec(MarkerStampKind::RenderSubmitStart, Key(50), kBase + 2, 900'000'000, 5120);
        Rec(MarkerStampKind::RenderSubmitEnd, Key(50), kBase + 3, 900'000'000 + 6'000'000, 5120);
        CHECK(MarkerStamps_Get(Key(50), &stamps));
        CHECK(stamps.tid[3] == 4812 && stamps.tid[0] == 4812 && stamps.tid[1] == 5120 && stamps.tid[2] == 5120);
        CHECK(stamps.sim_end_qpc == kBase + 1 && stamps.cycles[3] == 7'000'000);
        CHECK(MarkerStamps_SpanCpuUs(stamps, MarkerStampKind::SimulationStart, MarkerStampKind::SimulationEnd,
                                     kRate, &cpu_us));
        CHECK(cpu_us > 999.999 && cpu_us < 1000.001); // 3e6 cycles at 3e9 Hz
        CHECK(MarkerStamps_SpanCpuUs(stamps, MarkerStampKind::RenderSubmitStart, MarkerStampKind::RenderSubmitEnd,
                                     kRate, &cpu_us));
        CHECK(cpu_us > 1999.999 && cpu_us < 2000.001);

        // Across threads: two unrelated counters, no value. (A later sample of the end marker, from
        // another thread, replaces the earlier one whole.)
        cpu_us = -1.0;
        Rec(MarkerStampKind::RenderSubmitEnd, Key(50), kBase + 4, 900'000'000 + 6'000'000, 6001);
        CHECK(MarkerStamps_Get(Key(50), &stamps) && stamps.tid[2] == 6001);
        CHECK(!MarkerStamps_SpanCpuUs(stamps, MarkerStampKind::RenderSubmitStart, MarkerStampKind::RenderSubmitEnd,
                                      kRate, &cpu_us) && cpu_us == -1.0);

        // Rate unknown (0), a missing marker (no thread id), a cycle counter running backwards, a
        // missing cycle value.
        CHECK(MarkerStamps_Get(Key(50), &stamps));
        CHECK(!MarkerStamps_SpanCpuUs(stamps, MarkerStampKind::SimulationStart, MarkerStampKind::SimulationEnd,
                                      0.0, &cpu_us));
        MarkerStamps_Reset();
        Rec(MarkerStampKind::SimulationEnd, Key(51), kBase + 1, 5'000'000, 4812); // no start
        CHECK(MarkerStamps_Get(Key(51), &stamps));
        CHECK(!MarkerStamps_SpanCpuUs(stamps, MarkerStampKind::SimulationStart, MarkerStampKind::SimulationEnd,
                                      kRate, &cpu_us));
        Rec(MarkerStampKind::SimulationStart, Key(51), kBase, 6'000'000, 4812); // start after end
        CHECK(MarkerStamps_Get(Key(51), &stamps));
        CHECK(!MarkerStamps_SpanCpuUs(stamps, MarkerStampKind::SimulationStart, MarkerStampKind::SimulationEnd,
                                      kRate, &cpu_us));
        Rec(MarkerStampKind::SimulationStart, Key(51), kBase + 1, 0, 4812); // a later sample, cycles 0: none
        CHECK(MarkerStamps_Get(Key(51), &stamps));
        CHECK(!MarkerStamps_SpanCpuUs(stamps, MarkerStampKind::SimulationStart, MarkerStampKind::SimulationEnd,
                                      kRate, &cpu_us));
        // A start marker without a thread id stores nothing; the end marker's thread id still shows.
        MarkerStamps_Reset();
        Rec(MarkerStampKind::SimulationStart, Key(52), kBase, 100, 0);
        CHECK(!MarkerStamps_Get(Key(52), &stamps));
        Rec(MarkerStampKind::SimulationEnd, Key(52), kBase + 1, 300, 4812);
        CHECK(MarkerStamps_Get(Key(52), &stamps) && stamps.tid[0] == 4812 && stamps.tid[3] == 0);

        // The key off: nothing captured, so nothing to publish and no row has a cell to fill.
        MarkerStamps_Reset();
        g_unified_queue_upstream.store(0);
        const auto epoch = [] { return 1u; };
        const auto sample = [] { return MarkerClockSample{ kBase, 1, 3 }; };
        MarkerStamps_Publish(MarkerStamps_Capture(0, epoch, sample), kMarkerDomainNvApi, 60, epoch);
        MarkerStamps_Publish(MarkerStamps_Capture(1, epoch, sample), kMarkerDomainNvApi, 60, epoch);
        CHECK(!MarkerStamps_Get(Key(60), &stamps));
    }

    // The cycle rate: the TSC's from clean clock pairs (a narrow QPC-TSC-QPC bracket, three pairs 50 ms
    // apart, the two intervals agreeing within 0.1%), only with an invariant TSC and only when the
    // thread cycle counter agrees with the TSC.
    constexpr int64_t kRateFreq = 10'000'000;      // 10 MHz QPC: a tick is 100 ns
    constexpr int64_t kRateT0 = 1'000'000;         // the first pair's QPC time
    constexpr int64_t kRateInterval = 500'000;     // 50 ms in ticks

    // The TSC of a 3 GHz processor at QPC time t (300 ticks per QPC tick).
    uint64_t TscAt(int64_t t)
    {
        return 1'000'000'000ull + static_cast<uint64_t>(300 * (t - kRateT0));
    }

    // One read at QPC time t: the TSC (plus an error in ticks) between two QPC reads `width` ticks apart.
    void Feed(MarkerRateCalibration* calibration, int64_t t, int64_t tsc_error = 0, int64_t width = 2)
    {
        calibration->Pair(t - width / 2, static_cast<uint64_t>(static_cast<int64_t>(TscAt(t)) + tsc_error),
                          t + width / 2, kRateFreq);
    }

    // A calibration with three clean pairs fed: the TSC rate known, the verification next.
    MarkerRateCalibration VerifyingCalibration()
    {
        MarkerRateCalibration calibration;
        calibration.Begin(true);
        Feed(&calibration, kRateT0);
        Feed(&calibration, kRateT0 + kRateInterval);
        Feed(&calibration, kRateT0 + 2 * kRateInterval);
        return calibration;
    }

    void TestRateCalibration()
    {
        constexpr double kTsc = 3.0e9;
        double rate = 0.0;
        MarkerClockPair pair{};

        // A pair: the TSC between two QPC reads at the bracket's middle; a bracket wider than 5 us
        // (50 ticks of 100 ns), a QPC that went backwards or no frequency make no pair.
        CHECK(MarkerRate_MakePair(1'000, 777, 1'010, kRateFreq, &pair) && pair.tsc == 777 && pair.qpc == 1'005);
        CHECK(MarkerRate_MakePair(1'000, 777, 1'040, kRateFreq, &pair) && pair.qpc == 1'020); // 4 us: still clean
        CHECK(!MarkerRate_MakePair(1'000, 777, 1'060, kRateFreq, &pair));                     // 6 us
        CHECK(!MarkerRate_MakePair(1'000, 777, 1'000'000, kRateFreq, &pair));                 // a 100 ms bracket
        CHECK(!MarkerRate_MakePair(1'010, 777, 1'000, kRateFreq, &pair));
        CHECK(!MarkerRate_MakePair(0, 777, 10, kRateFreq, &pair));
        CHECK(!MarkerRate_MakePair(1'000, 777, 1'010, 0, &pair));

        // The rate of two pairs 50 ms apart.
        const MarkerClockPair first{ 1'000'000'000ull, 1'000'000 };
        const MarkerClockPair second{ 1'000'000'000ull + 150'000'000ull, 1'000'000 + 500'000 };
        CHECK(MarkerRate_FromPairs(first, second, kRateFreq, &rate));
        CHECK(rate > kTsc * 0.9999999 && rate < kTsc * 1.0000001);
        // Closer than 50 ms, a backwards TSC or QPC, a rate outside a processor's: no value.
        rate = -1.0;
        CHECK(!MarkerRate_FromPairs(first, { 1'000'000'000ull + 147'000'000ull, 1'000'000 + 490'000 }, kRateFreq, &rate) &&
              rate == -1.0);
        CHECK(!MarkerRate_FromPairs(second, first, kRateFreq, &rate));
        CHECK(!MarkerRate_FromPairs(first, { 999'999'999ull, 1'000'000 + 500'000 }, kRateFreq, &rate));
        CHECK(!MarkerRate_FromPairs(first, second, 0, &rate));
        CHECK(!MarkerRate_FromPairs(first, { 1'000'000'000ull + 20'000'000ull, 1'000'000 + 500'000 }, kRateFreq, &rate));      // 0.4 GHz
        CHECK(!MarkerRate_FromPairs(first, { 1'000'000'000ull + 6'000'000'000ull, 1'000'000 + 500'000 }, kRateFreq, &rate));   // 120 GHz

        // CPUID leaf 0x80000007 EDX bit 8.
        CHECK(MarkerRate_InvariantTsc(0x100u) && MarkerRate_InvariantTsc(0xFFFFFFFFu));
        CHECK(!MarkerRate_InvariantTsc(0u) && !MarkerRate_InvariantTsc(0xFFFFFEFFu));

        // Agreement of thread cycles with TSC ticks: within 5%; low can be lost CPU time, high cannot.
        CHECK(MarkerRate_Compare(15'000'000, 15'000'000) == MarkerRateAgreement::Agrees);
        CHECK(MarkerRate_Compare(14'400'000, 15'000'000) == MarkerRateAgreement::Agrees);
        CHECK(MarkerRate_Compare(12'000'000, 15'000'000) == MarkerRateAgreement::Below);
        CHECK(MarkerRate_Compare(0, 15'000'000) == MarkerRateAgreement::Below);
        CHECK(MarkerRate_Compare(16'000'000, 15'000'000) == MarkerRateAgreement::Above);
        CHECK(MarkerRate_Compare(1, 0) == MarkerRateAgreement::Below);

        // No invariant TSC: no rate, finished, nothing to verify; the CPU cells stay empty.
        MarkerRateCalibration no_invariant;
        CHECK(!no_invariant.Finished() && no_invariant.RateHz() == 0.0);
        no_invariant.Begin(false);
        CHECK(no_invariant.state() == MarkerRateCalibration::State::Failed && no_invariant.Finished());
        Feed(&no_invariant, kRateT0);
        Feed(&no_invariant, kRateT0 + kRateInterval);
        Feed(&no_invariant, kRateT0 + 2 * kRateInterval);
        CHECK(no_invariant.RateHz() == 0.0 && no_invariant.VerifyTicks() == 0 && no_invariant.pairs() == 0);

        // Three clean pairs: a pair too close to the newest is ignored, the rate is the whole span's.
        MarkerRateCalibration calibration;
        calibration.Begin(true);
        CHECK(calibration.state() == MarkerRateCalibration::State::Waiting && calibration.RateHz() == 0.0);
        Feed(&calibration, kRateT0);
        Feed(&calibration, kRateT0 + 100'000); // 10 ms later: read again later
        CHECK(calibration.pairs() == 1);
        Feed(&calibration, kRateT0 + kRateInterval);
        CHECK(calibration.pairs() == 2 && calibration.state() == MarkerRateCalibration::State::Waiting);
        Feed(&calibration, kRateT0 + 2 * kRateInterval - 1); // 49.9999 ms after the second: not yet
        CHECK(calibration.pairs() == 2);
        Feed(&calibration, kRateT0 + 2 * kRateInterval);
        CHECK(calibration.state() == MarkerRateCalibration::State::Verifying && calibration.RateHz() == 0.0);
        CHECK(calibration.VerifyTicks() >= 14'999'999 && calibration.VerifyTicks() <= 15'000'000); // 5 ms of 3 GHz
        calibration.Verified(14'900'000, 15'000'000);
        CHECK(calibration.state() == MarkerRateCalibration::State::Ready && calibration.Finished());
        CHECK(calibration.RateHz() > kTsc * 0.9999999 && calibration.RateHz() < kTsc * 1.0000001);

        // A wide bracket is no pair: the worker was preempted between the reads. The pair of a
        // 20 ms preemption after the first QPC read is such a bracket.
        MarkerRateCalibration interrupted;
        interrupted.Begin(true);
        Feed(&interrupted, kRateT0);
        // The second read: QPC, then 20 ms off the CPU, then the TSC, then QPC: the bracket is 20 ms wide.
        interrupted.Pair(kRateT0 + kRateInterval, TscAt(kRateT0 + kRateInterval + 200'000),
                         kRateT0 + kRateInterval + 200'000, kRateFreq);
        CHECK(interrupted.pairs() == 1 && interrupted.rejects() == 1 &&
              interrupted.state() == MarkerRateCalibration::State::Waiting);
        // Only wide brackets, forever: no rate.
        MarkerRateCalibration wide;
        wide.Begin(true);
        for (int i = 0; i < kMarkerRateMaxRejects; ++i) Feed(&wide, kRateT0 + i * kRateInterval, 0, 200);
        CHECK(wide.state() == MarkerRateCalibration::State::Failed && wide.RateHz() == 0.0 && wide.pairs() == 0);

        // The two intervals disagree by more than 0.1% (the middle pair is 0.2% of an interval off):
        // nothing is published, the oldest pair is dropped, the next pairs are waited for.
        MarkerRateCalibration disagreeing;
        disagreeing.Begin(true);
        Feed(&disagreeing, kRateT0);
        Feed(&disagreeing, kRateT0 + kRateInterval, 300'000);
        Feed(&disagreeing, kRateT0 + 2 * kRateInterval);
        CHECK(disagreeing.state() == MarkerRateCalibration::State::Waiting && disagreeing.RateHz() == 0.0 &&
              disagreeing.pairs() == 2 && disagreeing.retries() == 1);
        Feed(&disagreeing, kRateT0 + 3 * kRateInterval); // the bad pair is still the oldest of three
        CHECK(disagreeing.state() == MarkerRateCalibration::State::Waiting && disagreeing.retries() == 2);
        Feed(&disagreeing, kRateT0 + 4 * kRateInterval); // three clean pairs now
        CHECK(disagreeing.state() == MarkerRateCalibration::State::Verifying && disagreeing.retries() == 2);
        disagreeing.Verified(15'000'000, 15'000'000);
        CHECK(disagreeing.state() == MarkerRateCalibration::State::Ready &&
              disagreeing.RateHz() > kTsc * 0.9999999 && disagreeing.RateHz() < kTsc * 1.0000001);

        // Two intervals that differ by 0.04% agree (within 0.1%): published, the whole span's rate.
        MarkerRateCalibration agreeing;
        agreeing.Begin(true);
        Feed(&agreeing, kRateT0);
        Feed(&agreeing, kRateT0 + kRateInterval, 30'000);
        Feed(&agreeing, kRateT0 + 2 * kRateInterval);
        CHECK(agreeing.state() == MarkerRateCalibration::State::Verifying && agreeing.retries() == 0);
        agreeing.Verified(15'000'000, 15'000'000);
        CHECK(agreeing.RateHz() > kTsc * 0.9999999 && agreeing.RateHz() < kTsc * 1.0000001);

        // Pairs that never agree: no rate after the retries.
        MarkerRateCalibration erratic;
        erratic.Begin(true);
        for (int i = 0; i < 8; ++i) Feed(&erratic, kRateT0 + i * kRateInterval, (i % 2) != 0 ? 300'000 : 0);
        CHECK(erratic.state() == MarkerRateCalibration::State::Failed && erratic.RateHz() == 0.0);

        // Consistent pairs of a rate outside a processor's: Failed.
        MarkerRateCalibration slow;
        slow.Begin(true);
        for (int i = 0; i < 3; ++i)
            slow.Pair(kRateT0 + i * kRateInterval - 1, 1'000'000'000ull + static_cast<uint64_t>(i) * 20'000'000ull,
                      kRateT0 + i * kRateInterval + 1, kRateFreq);
        CHECK(slow.state() == MarkerRateCalibration::State::Failed && slow.RateHz() == 0.0);

        // A preempted verification reads low and is tried again; agreement then is a rate.
        MarkerRateCalibration retried = VerifyingCalibration();
        retried.Verified(9'000'000, 15'000'000);
        CHECK(retried.state() == MarkerRateCalibration::State::Verifying);
        retried.Verified(14'800'000, 15'000'000);
        CHECK(retried.state() == MarkerRateCalibration::State::Ready && retried.RateHz() > 0.0);

        // Low every time: Failed after the attempts.
        MarkerRateCalibration low = VerifyingCalibration();
        for (int i = 0; i < kMarkerRateVerifyAttempts; ++i) low.Verified(9'000'000, 15'000'000);
        CHECK(low.state() == MarkerRateCalibration::State::Failed && low.RateHz() == 0.0);

        // A thread counter above the TSC is not the TSC's rate: Failed at once.
        MarkerRateCalibration above = VerifyingCalibration();
        above.Verified(17'000'000, 15'000'000);
        CHECK(above.state() == MarkerRateCalibration::State::Failed && above.RateHz() == 0.0);
    }

#ifdef _WIN32
    // Windows only: the cost of a marker hook's thread sample, no assertion on speed, and the
    // one-shot rate calibration the telemetry worker runs.
    void BenchmarkThreadSample()
    {
        constexpr int kCalls = 2'000'000;
        LARGE_INTEGER frequency{}, begin{}, end{};
        QueryPerformanceFrequency(&frequency);
        uint64_t sink = 0;
        for (int i = 0; i < 1000; ++i) { // warm up
            ULONG64 cycles = 0;
            QueryThreadCycleTime(GetCurrentThread(), &cycles);
            sink += cycles + GetCurrentThreadId();
        }
        QueryPerformanceCounter(&begin);
        for (int i = 0; i < kCalls; ++i) {
            ULONG64 cycles = 0;
            QueryThreadCycleTime(GetCurrentThread(), &cycles);
            sink += cycles + GetCurrentThreadId();
        }
        QueryPerformanceCounter(&end);
        const double ns = static_cast<double>(end.QuadPart - begin.QuadPart) * 1.0e9 /
            static_cast<double>(frequency.QuadPart) / kCalls;
        std::printf("QueryThreadCycleTime(GetCurrentThread()) + GetCurrentThreadId(): %.1f ns/call (%d calls, sink %llu)\n",
                    ns, kCalls, static_cast<unsigned long long>(sink & 1));

        // The whole hook sample (QPC, thread cycles, thread id).
        QueryPerformanceCounter(&begin);
        for (int i = 0; i < kCalls; ++i) sink += static_cast<uint64_t>(MarkerCycles_Sample().qpc);
        QueryPerformanceCounter(&end);
        const double sample_ns = static_cast<double>(end.QuadPart - begin.QuadPart) * 1.0e9 /
            static_cast<double>(frequency.QuadPart) / kCalls;
        std::printf("MarkerCycles_Sample() (QPC + cycles + id): %.1f ns/call (sink %llu)\n",
                    sample_ns, static_cast<unsigned long long>(sink & 1));

        // The rate calibration as the telemetry worker drives it: one step per cycle (1 ms apart
        // here), the second pair due after 50 ms; printed, not asserted (a machine without an
        // invariant TSC, or whose thread cycle counter is not the TSC's, ends with no rate).
        bool done = false;
        for (int i = 0; i < 2000 && !done; ++i) {
            done = MarkerCycles_CalibrateStep();
            if (!done) Sleep(1);
        }
        std::printf("cycle rate: %.0f Hz (0 = unknown), calibration %s\n", MarkerCycles_RateHz(),
                    done ? "finished" : "unfinished");
        CHECK(done);
    }
#endif

    // Three threads write the three kinds of the same frames (every slot is hit by all three and
    // is replaced every 256 frames) while a reader checks every copy: the key is the one asked
    // for and each stamp is 0 or that frame's own value, never another frame's.
    void TestConcurrentConsistency()
    {
        MarkerStamps_Reset();
        constexpr uint64_t kFrames = 200000;
        std::atomic<uint64_t> newest{0};
        std::atomic<bool> stop{false};
        const auto value = [](uint64_t id, int kind) { return kBase + static_cast<int64_t>(id) * 10 + kind; };

        // A game's threads are within a few frames of each other: no writer runs more than 64
        // frames ahead of the slowest, so a frame is never replaced before the others stamped it.
        std::atomic<uint64_t> progress[3] = {{0}, {0}, {0}};
        const auto slowest = [&] {
            uint64_t low = progress[0].load(std::memory_order_relaxed);
            for (int i = 1; i < 3; ++i) {
                const uint64_t other = progress[i].load(std::memory_order_relaxed);
                if (other < low) low = other;
            }
            return low;
        };
        const auto writer = [&](MarkerStampKind kind, int index, bool publish) {
            for (uint64_t id = 1; id <= kFrames; ++id) {
                while (id > slowest() + 64) std::this_thread::yield();
                MarkerStamps_Record(Key(id), MarkerStamps_Generation(), kind,
                                    MarkerClockSample{ value(id, index), 1, 1u });
                progress[index - 1].store(id, std::memory_order_relaxed);
                if (publish) newest.store(id, std::memory_order_relaxed);
            }
        };
        std::thread sim(writer, MarkerStampKind::SimulationEnd, 1, true);
        std::thread render_start(writer, MarkerStampKind::RenderSubmitStart, 2, false);
        std::thread render_end(writer, MarkerStampKind::RenderSubmitEnd, 3, false);

        uint64_t reads = 0, hits = 0, bad = 0;
        std::thread reader([&] {
            uint64_t seed = 0x9E3779B97F4A7C15ull;
            while (!stop.load(std::memory_order_relaxed)) {
                seed = seed * 6364136223846793005ull + 1442695040888963407ull;
                const uint64_t top = newest.load(std::memory_order_relaxed);
                if (top == 0) continue;
                const uint64_t id = 1 + (seed >> 33) % top;
                MarkerStamps stamps{};
                ++reads;
                if (!MarkerStamps_Get(Key(id), &stamps)) continue;
                ++hits;
                const bool ok = stamps.frame_id == id && stamps.domain == kMarkerDomainNvApi &&
                    stamps.producer_epoch == 1 &&
                    (stamps.sim_end_qpc == 0 || stamps.sim_end_qpc == value(id, 1)) &&
                    (stamps.render_start_qpc == 0 || stamps.render_start_qpc == value(id, 2)) &&
                    (stamps.render_end_qpc == 0 || stamps.render_end_qpc == value(id, 3));
                if (!ok) ++bad;
            }
        });

        sim.join();
        render_start.join();
        render_end.join();
        stop.store(true);
        reader.join();
        CHECK(bad == 0);
        std::printf("concurrent: %llu reads, %llu consistent copies, %llu torn\n",
                    static_cast<unsigned long long>(reads), static_cast<unsigned long long>(hits),
                    static_cast<unsigned long long>(bad));

        // Quiet again: the newest frames hold all three of their own stamps (a stamp is dropped
        // only when a descheduled writer holds the slot through 256 paused claim attempts: a few at most).
        int settled = 0;
        for (uint64_t id = kFrames - 100; id <= kFrames; ++id) {
            MarkerStamps stamps{};
            if (MarkerStamps_Get(Key(id), &stamps) && stamps.sim_end_qpc == value(id, 1) &&
                stamps.render_start_qpc == value(id, 2) && stamps.render_end_qpc == value(id, 3))
                ++settled;
        }
        CHECK(settled >= 96);
    }
}

int main()
{
    std::printf("=== Marker stamps tests ===\n");
    TestRecordAndGet();
    TestIgnoredInput();
    TestFullKeyRequired();
    TestLaterFrameReplacesSlot();
    TestReset();
    TestDuplicateKeepsLatestSample();
    TestGate();
    TestCapturedLifetime();
    TestSpanCpuTime();
    TestRateCalibration();
    TestConcurrentConsistency();
#ifdef _WIN32
    BenchmarkThreadSample();
#endif
    std::printf("=== %d failures ===\n", s_failures);
    return s_failures == 0 ? 0 : 1;
}
