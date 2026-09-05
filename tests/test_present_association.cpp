#include <atomic>
#include <cmath>
#include <iostream>
#include <thread>

#include "bounded_mpsc_queue.h"
#include "present_association.h"

namespace
{
    constexpr uint64_t kQpcFrequency = 1'000'000;
    PresentAssociationIndex g_index;
    PendingPresentObservations g_pending;

    bool Check(bool condition, const char* message)
    {
        if (!condition) std::cerr << "FAIL: " << message << '\n';
        return condition;
    }

    PresentTimelineSpan MakeSpan(
        uint32_t capture_epoch,
        uint64_t span_sequence,
        uint64_t present_sequence,
        uint64_t native_swapchain,
        uint32_t thread_id,
        int64_t p_qpc,
        int64_t e_qpc)
    {
        PresentTimelineSpan span{};
        span.sequence = span_sequence;
        span.capture_epoch = capture_epoch;
        span.key.swapchain_generation = 11;
        span.key.present_sequence = present_sequence;
        span.api_call_sequence = present_sequence + 100;
        span.native_swapchain = native_swapchain;
        span.thread_id = thread_id;
        span.api = PresentTimelineApi::D3D12;
        span.entry_point = PresentTimelineEntryPoint::DxgiPresent;
        span.p_qpc = p_qpc;
        span.e_qpc = e_qpc;
        return span;
    }

    PresentTimelineObservation MakeObservation(
        uint32_t capture_epoch,
        uint64_t native_swapchain,
        uint32_t thread_id,
        uint64_t present_start_qpc,
        uint64_t present_end_qpc,
        uint64_t report_qpc)
    {
        PresentTimelineObservation observation{};
        observation.capture_epoch = capture_epoch;
        observation.observer_epoch = 7;
        observation.runtime = PresentObservationRuntime::Dxgi;
        observation.final_state = PresentObservationFinalState::Presented;
        observation.native_swapchain = native_swapchain;
        observation.thread_id = thread_id;
        observation.present_start_qpc = present_start_qpc;
        observation.present_end_qpc = present_end_qpc;
        observation.report_qpc = report_qpc;
        observation.qpc_frequency = kQpcFrequency;
        observation.displayed_instance_count = 1;
        return observation;
    }

    bool TestUniqueStrictAssociation()
    {
        g_index.Reset();
        const PresentTimelineSpan span =
            MakeSpan(3, 41, 91, 0x1234, 71, 1'000, 1'100);
        g_index.Retain(span, kQpcFrequency);

        const PresentTimelineObservation observation =
            MakeObservation(3, 0x1234, 71, 1'005, 1'095, 1'001'100);
        const PresentAssociationResult result =
            g_index.Associate(observation, kQpcFrequency);
        const PresentAssociationDiagnostics diagnostics = g_index.Diagnostics();
        return Check(result.status == PresentAssociationStatus::Unique,
                     "strict constraints produce one association") &&
            Check(result.candidate_count == 1,
                  "unique association reports one candidate") &&
            Check(result.source.key == span.key,
                  "association preserves the canonical local Present key") &&
            Check(std::fabs(result.report_age_us - 1'000'000.0) < 0.01,
                  "association reports delayed arrival age") &&
            Check(diagnostics.unique_associations == 1,
                  "unique association is diagnosed");
    }

    bool TestRejectedCandidates()
    {
        g_index.Reset();
        g_index.Retain(
            MakeSpan(3, 1, 1, 0x1234, 71, 1'000, 1'100),
            kQpcFrequency);

        PresentTimelineObservation observation =
            MakeObservation(3, 0x9999, 71, 1'005, 1'095, 2'000);
        if (!Check(
                g_index.Associate(observation, kQpcFrequency).status ==
                    PresentAssociationStatus::NotFound,
                "wrong swapchain remains unassociated")) {
            return false;
        }

        observation = MakeObservation(3, 0x1234, 72, 1'005, 1'095, 2'000);
        if (!Check(
                g_index.Associate(observation, kQpcFrequency).status ==
                    PresentAssociationStatus::NotFound,
                "wrong Present thread remains unassociated")) {
            return false;
        }

        observation = MakeObservation(3, 0x1234, 71, 999, 1'095, 2'000);
        if (!Check(
                g_index.Associate(observation, kQpcFrequency).status ==
                    PresentAssociationStatus::NotFound,
                "runtime interval outside local P/E remains unassociated")) {
            return false;
        }

        observation = MakeObservation(4, 0x1234, 71, 1'005, 1'095, 2'000);
        if (!Check(
                g_index.Associate(observation, kQpcFrequency).status ==
                    PresentAssociationStatus::NotFound,
                "different capture epochs never join")) {
            return false;
        }

        observation = MakeObservation(3, 0x1234, 71, 1'005, 1'095, 2'000);
        observation.runtime = PresentObservationRuntime::D3D9;
        if (!Check(
                g_index.Associate(observation, kQpcFrequency).status ==
                    PresentAssociationStatus::UnsupportedRuntime,
                "non-DXGI runtime is rejected before candidate search")) {
            return false;
        }

        observation = MakeObservation(3, 0x1234, 71, 1'005, 1'095, 2'000);
        observation.qpc_frequency = kQpcFrequency + 1;
        return Check(
            g_index.Associate(observation, kQpcFrequency).status ==
                PresentAssociationStatus::ClockMismatch,
            "incompatible QPC frequency is rejected");
    }

    bool TestAmbiguousAssociation()
    {
        g_index.Reset();
        g_index.Retain(
            MakeSpan(3, 1, 10, 0x1234, 71, 1'000, 1'100),
            kQpcFrequency);
        g_index.Retain(
            MakeSpan(3, 2, 20, 0x1234, 71, 1'002, 1'098),
            kQpcFrequency);

        const PresentAssociationResult result = g_index.Associate(
            MakeObservation(3, 0x1234, 71, 1'005, 1'095, 2'000),
            kQpcFrequency);
        return Check(result.status == PresentAssociationStatus::Ambiguous,
                     "multiple strict candidates are rejected") &&
            Check(result.candidate_count == 2,
                  "ambiguous association reports every candidate") &&
            Check(result.source.key.swapchain_generation == 0,
                  "ambiguous association does not fabricate a source key");
    }

    bool TestTimeRetentionExpiry()
    {
        g_index.Reset();
        g_index.Retain(
            MakeSpan(3, 1, 10, 0x1234, 71, 1'000'000, 1'000'100),
            kQpcFrequency);
        const PresentAssociationResult result = g_index.Associate(
            MakeObservation(
                3,
                0x1234,
                71,
                1'000'005,
                1'000'095,
                6'000'101),
            kQpcFrequency);
        const PresentAssociationDiagnostics diagnostics = g_index.Diagnostics();
        return Check(result.status == PresentAssociationStatus::NotFound,
                     "observation older than five seconds cannot join") &&
            Check(g_index.RetainedSpanCount() == 0,
                  "expired local span is removed") &&
            Check(diagnostics.retention_age_evictions == 1,
                  "age eviction is diagnosed");
    }

    bool TestOutOfOrderTimeRetention()
    {
        g_index.Reset();
        g_index.Retain(
            MakeSpan(3, 1, 10, 0x1234, 71, 6'000'000, 6'000'100),
            kQpcFrequency);
        g_index.Retain(
            MakeSpan(3, 2, 20, 0x5678, 72, 1'000'000, 1'000'100),
            kQpcFrequency);

        const PresentAssociationResult result = g_index.Associate(
            MakeObservation(
                3,
                0x1234,
                71,
                6'000'005,
                6'000'095,
                6'000'200),
            kQpcFrequency);
        const PresentAssociationDiagnostics diagnostics = g_index.Diagnostics();
        return Check(result.status == PresentAssociationStatus::Unique,
                     "fresh span survives out-of-order retention") &&
            Check(g_index.RetainedSpanCount() == 1,
                  "stale span behind a fresh span is still evicted") &&
            Check(diagnostics.retention_age_evictions == 1,
                  "out-of-order age eviction is diagnosed");
    }

    bool TestFgBurstAndClassificationEvidence()
    {
        g_index.Reset();
        const PresentTimelineSpan real_span =
            MakeSpan(8, 50, 40, 0xABCD, 99, 10'000, 10'100);
        const PresentTimelineSpan generated_span =
            MakeSpan(8, 51, 99, 0xABCD, 99, 10'300, 10'400);
        g_index.Retain(real_span, kQpcFrequency);
        g_index.Retain(generated_span, kQpcFrequency);

        PresentTimelineObservation real_observation =
            MakeObservation(8, 0xABCD, 99, 10'005, 10'095, 1'000'000);
        real_observation.classification_evidence =
            PRESENT_CLASSIFICATION_EVIDENCE_PCL_SIMULATION_MARKER;
        real_observation.frame_classification = PresentAssociation_ClassifyFrame(
            real_observation.classification_evidence);
        real_observation.displayed_instance_count = 2;

        PresentTimelineObservation generated_observation =
            MakeObservation(8, 0xABCD, 99, 10'305, 10'395, 1'000'000);
        generated_observation.classification_evidence =
            PRESENT_CLASSIFICATION_EVIDENCE_SIMULATION_FALLBACK;
        generated_observation.frame_classification =
            PresentAssociation_ClassifyFrame(
                generated_observation.classification_evidence);

        const PresentAssociationResult real_result =
            g_index.Associate(real_observation, kQpcFrequency);
        const PresentAssociationResult generated_result =
            g_index.Associate(generated_observation, kQpcFrequency);
        const PresentAssociationDiagnostics diagnostics = g_index.Diagnostics();
        return Check(real_result.status == PresentAssociationStatus::Unique &&
                         real_result.source.key.present_sequence == 40,
                     "FG burst real observation joins by strict timing") &&
            Check(generated_result.status == PresentAssociationStatus::Unique &&
                      generated_result.source.key.present_sequence == 99,
                  "FG burst generated observation joins without parity") &&
            Check(real_observation.frame_classification ==
                      PresentFrameClassification::Real,
                  "PCL simulation marker classifies real evidence") &&
            Check(generated_observation.frame_classification ==
                      PresentFrameClassification::Generated,
                  "simulation fallback classifies generated evidence") &&
            Check(diagnostics.unique_associations == 2,
                  "one multi-display Present observation creates one association") &&
            Check(PresentAssociation_ClassifyFrame(
                      PRESENT_CLASSIFICATION_EVIDENCE_PCL_SIMULATION_MARKER |
                      PRESENT_CLASSIFICATION_EVIDENCE_SIMULATION_FALLBACK) ==
                      PresentFrameClassification::Conflicting,
                  "conflicting real/generated evidence is not forced");
    }

    bool TestObservationBeforeSpanDrain()
    {
        g_index.Reset();
        g_pending.Reset();
        const auto observation = MakeObservation(3, 0x1234, 71, 1005, 1095, 2000);
        PresentTimelineObservation emitted{};
        PresentAssociationResult result{};
        if (!Check(g_pending.Push(observation, 1, 2000), "defer observation after empty span snapshot") ||
            !Check(!g_pending.TryFinalize({ 1, 200'000, kQpcFrequency, 0 },
                       &g_index, &emitted, &result),
                   "elapsed grace alone is not a later span drain") ||
            !Check(g_index.Diagnostics().not_found_associations == 0,
                   "pending observation does not increment final diagnostics")) return false;

        g_index.Retain(MakeSpan(3, 1, 10, 0x1234, 71, 1000, 1100), kQpcFrequency);
        if (!Check(!g_pending.TryFinalize({ 2, 3000, kQpcFrequency, 0 },
                       &g_index, &emitted, &result),
                   "provisional unique waits for other delayed spans")) return false;
        return Check(g_pending.TryFinalize({ 3, 102'000, kQpcFrequency, 0 },
                         &g_index, &emitted, &result) &&
                         result.status == PresentAssociationStatus::Unique,
                     "span arriving on the next drain supplies the canonical key") &&
            Check(result.source.key.present_sequence == 10 &&
                      g_index.Diagnostics().unique_associations == 1,
                  "only the final association is counted") &&
            Check(!g_pending.TryFinalize({ 4, 202'000, kQpcFrequency, 0 },
                      &g_index, &emitted, &result), "observation is emitted exactly once");
    }

    // BoundedMpscQueue reserves a cell before assigning Value. Pausing that
    // assignment deterministically exercises its real unpublished-head behavior.
    struct PausedSpan {
        PresentTimelineSpan span{};
        std::atomic<bool>* entered = nullptr;
        std::atomic<bool>* resume = nullptr;

        PausedSpan& operator=(const PausedSpan& other) noexcept
        {
            span = other.span;
            if (other.entered != nullptr) {
                other.entered->store(true, std::memory_order_release);
                while (!other.resume->load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
            }
            // Queue cells intentionally do not inherit the test pause hook.
            return *this;
        }
    };

    bool TestMpscHeadDelayAndLateAmbiguity()
    {
        g_index.Reset();
        g_pending.Reset();
        relimiter::BoundedMpscQueue<PausedSpan, 4> queue;
        relimiter::CaptureLifecycle<4> lifecycle;
        lifecycle.Transition(true);
        std::atomic<bool> entered{ false };
        std::atomic<bool> resume{ false };
        PausedSpan first{};
        first.span = MakeSpan(1, 1, 10, 0x1234, 71, 1000, 1100);
        first.entered = &entered;
        first.resume = &resume;
        std::thread producer([&] {
            lifecycle.Acquire();
            queue.Push(first);
            lifecycle.Release();
        });
        while (!entered.load(std::memory_order_acquire)) std::this_thread::yield();

        PausedSpan second{};
        second.span = MakeSpan(1, 2, 20, 0x1234, 71, 1002, 1098);
        queue.Push(second);
        g_pending.Push(MakeObservation(1, 0x1234, 71, 1005, 1095, 2000), 1, 2000);
        PausedSpan popped{};
        FrameTimelineCaptureState checkpoint{};
        PresentTimelineObservation emitted{};
        PresentAssociationResult result{};
        bool ok = Check(!queue.Pop(&popped), "MPSC head hides the completed second span") &&
            Check(!lifecycle.TryCheckpoint(&checkpoint),
                  "inflight producer prevents declaring the span prefix drained") &&
            Check(!g_pending.TryFinalize({ 0, 1'000'000, kQpcFrequency, 0 },
                      &g_index, &emitted, &result),
                  "head delay longer than grace does not prematurely finalize not_found");
        resume.store(true, std::memory_order_release);
        producer.join();
        ok = Check(lifecycle.TryCheckpoint(&checkpoint), "released MPSC head permits checkpoint") && ok;
        size_t drained = 0;
        while (queue.Pop(&popped)) {
            g_index.Retain(popped.span, kQpcFrequency);
            ++drained;
        }
        return Check(drained == 2, "both accepted spans are retained after head publication") &&
            Check(g_pending.TryFinalize({ 2, 1'000'000, kQpcFrequency, 0 },
                      &g_index, &emitted, &result) &&
                      result.status == PresentAssociationStatus::Ambiguous &&
                      result.candidate_count == 2 && result.source.key.present_sequence == 0,
                  "late second candidate prevents a false unique association") &&
            Check(g_index.Diagnostics().ambiguous_associations == 1 &&
                      g_index.Diagnostics().not_found_associations == 0,
                  "no provisional diagnostics leak into final results") && ok;
    }

    bool TestCheckpointMustBeFresh()
    {
        g_index.Reset();
        g_pending.Reset();
        g_pending.Push(MakeObservation(1, 0x1234, 71, 1005, 1095, 2000), 1, 2000);
        PresentTimelineObservation emitted{};
        PresentAssociationResult result{};
        if (!Check(!g_pending.TryFinalize({ 2, 3000, kQpcFrequency, 0 },
                       &g_index, &emitted, &result),
                   "idle checkpoint before grace retains observation") ||
            !Check(!g_pending.TryFinalize({ 0, 1'000'000, kQpcFrequency, 0 },
                       &g_index, &emitted, &result),
                   "a later inflight publisher invalidates earlier drain evidence")) return false;
        g_index.Retain(MakeSpan(1, 1, 10, 0x1234, 71, 1000, 1100), kQpcFrequency);
        return Check(g_pending.TryFinalize({ 4, 1'000'000, kQpcFrequency, 0 },
                         &g_index, &emitted, &result) &&
                         result.status == PresentAssociationStatus::Unique,
                     "fresh checkpoint drains the late publisher before finalization");
    }

    bool TestPendingCapacityAndFinalDrain()
    {
        g_index.Reset();
        g_pending.Reset();
        const auto observation = MakeObservation(3, 0x1234, 71, 1005, 1095, 2000);
        for (size_t index = 0; index < PRESENT_ASSOCIATION_PENDING_CAPACITY; ++index) {
            if (!Check(g_pending.Push(observation, 1, 2000), "fill bounded pending observations")) {
                return false;
            }
        }
        if (!Check(!g_pending.Push(observation, 1, 2000), "pending overflow rejects the newest observation") ||
            !Check(g_pending.HasCapture(3) && !g_pending.HasCapture(4),
                   "pending capture prevents its retirement")) return false;
        PresentTimelineObservation emitted{};
        PresentAssociationResult result{};
        if (!Check(!g_pending.TryFinalize({ 1, 2000, kQpcFrequency, 3 },
                       &g_index, &emitted, &result),
                   "shutdown still requires a post-admission span drain")) return false;
        size_t emitted_count = 0;
        while (g_pending.TryFinalize({ 2, 2000, kQpcFrequency, 3 },
            &g_index, &emitted, &result)) {
            ++emitted_count;
        }
        return Check(emitted_count == PRESENT_ASSOCIATION_PENDING_CAPACITY &&
                         !g_pending.HasCapture(3),
                     "closed-epoch final drain bypasses grace and emits every accepted observation") &&
            Check(g_index.Diagnostics().not_found_associations == emitted_count,
                  "genuine final misses are counted once, overflow is not a miss");
    }

    bool TestDeferredCaptureIsolationAndRetention()
    {
        g_index.Reset();
        g_pending.Reset();
        g_index.Retain(MakeSpan(3, 1, 10, 0x1234, 71, 1000, 1100), kQpcFrequency);
        g_index.Retain(MakeSpan(4, 2, 20, 0x1234, 71, 1000, 1100), kQpcFrequency);
        g_pending.Push(MakeObservation(3, 0x1234, 71, 1005, 1095, 2000), 1, 2000);
        g_pending.Push(MakeObservation(4, 0x1234, 71, 1005, 1095, 2000), 1, 2000);
        PresentTimelineObservation emitted{};
        PresentAssociationResult result{};
        if (!Check(g_pending.TryFinalize({ 2, 2000, kQpcFrequency, 3 },
                       &g_index, &emitted, &result) &&
                       result.status == PresentAssociationStatus::Unique &&
                       result.source.capture_epoch == 3,
                   "closed capture matches only its own retained spans")) return false;
        g_index.ClearCaptureSpans(3);
        if (!Check(g_index.RetainedSpanCount() == 1 && g_pending.HasCapture(4),
                   "retiring one capture preserves the next capture's span and observation") ||
            !Check(!g_pending.TryFinalize({ 2, 2000, kQpcFrequency, 3 },
                       &g_index, &emitted, &result),
                   "closed earlier epoch does not bypass current capture's grace") ||
            !Check(g_pending.TryFinalize({ 3, 102'000, kQpcFrequency, 3 },
                       &g_index, &emitted, &result) &&
                       result.status == PresentAssociationStatus::Unique &&
                       result.source.capture_epoch == 4,
                   "next capture still finalizes with its own key")) return false;

        g_index.Reset();
        g_pending.Push(MakeObservation(4, 0x1234, 71, 1005, 1095, 5'001'101),
            4, 5'001'101);
        g_index.Retain(MakeSpan(4, 2, 20, 0x1234, 71, 1000, 1100), kQpcFrequency);
        return Check(g_pending.TryFinalize({ 5, 5'101'101, kQpcFrequency, 0 },
                         &g_index, &emitted, &result) &&
                         result.status == PresentAssociationStatus::NotFound &&
                         g_index.Diagnostics().retention_age_evictions == 1,
                     "retry does not resurrect a candidate outside five-second retention");
    }
}

int main()
{
    bool ok = true;
    ok = TestUniqueStrictAssociation() && ok;
    ok = TestRejectedCandidates() && ok;
    ok = TestAmbiguousAssociation() && ok;
    ok = TestTimeRetentionExpiry() && ok;
    ok = TestOutOfOrderTimeRetention() && ok;
    ok = TestFgBurstAndClassificationEvidence() && ok;
    ok = TestObservationBeforeSpanDrain() && ok;
    ok = TestMpscHeadDelayAndLateAmbiguity() && ok;
    ok = TestCheckpointMustBeFresh() && ok;
    ok = TestPendingCapacityAndFinalDrain() && ok;
    ok = TestDeferredCaptureIsolationAndRetention() && ok;

    if (!ok) return 1;
    std::cout << "Present association tests passed\n";
    return 0;
}
