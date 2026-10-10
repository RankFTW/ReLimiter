#include "native_pcl_measurement_internal.h"

#include <Windows.h>
#include <evntrace.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "PresentMonTraceConsumer.hpp"
#include "PresentMonTraceSession.hpp"
#include "logger.h"
#include "native_pcl_etw_cleanup.h"
#include "native_pcl_etw_diagnostics.h"
#include "native_pcl_etw_provider_setup.h"
#include "native_pcl_etw_session.h"
#include "native_pcl_loss_window.h"
#include "present_association.h"

namespace
{
    constexpr double kInputToFrameStartEmaAlpha = 0.1;
    constexpr double kLowFpsIntervalMilliseconds = 50.0;
    constexpr double kNoPclStatsWarmupMilliseconds = 5000.0;
    constexpr double kPresentDeferralMilliseconds = 1000.0;
    constexpr DWORD kDrainWaitMilliseconds = 100;
    constexpr DWORD kProcessTraceReadyTimeoutMilliseconds = 2000;
    constexpr ULONGLONG kEtwStatusQueryMilliseconds = 1000;

    std::atomic<uint32_t> g_session_nonce{ 0 };

    // Raw EtwStatus counters, in order: ETW events lost, ETW buffers lost,
    // PresentMon completed-ring overflow. Per-present lost records stay
    // recoverable and never open a window.
    using CaptureIntegrityLossWindow = NativePclLossWindow<3>;

    uint64_t QueryQpcFrequency() noexcept
    {
        LARGE_INTEGER frequency{};
        return QueryPerformanceFrequency(&frequency)
            ? static_cast<uint64_t>(frequency.QuadPart)
            : 0;
    }

    uint64_t QueryQpc() noexcept
    {
        LARGE_INTEGER timestamp{};
        return QueryPerformanceCounter(&timestamp)
            ? static_cast<uint64_t>(timestamp.QuadPart)
            : 0;
    }

    double QpcDeltaToMicroseconds(
        uint64_t start,
        uint64_t end,
        uint64_t frequency) noexcept
    {
        if (start == 0 || end < start || frequency == 0) {
            return std::numeric_limits<double>::quiet_NaN();
        }
        return static_cast<double>(end - start) * 1'000'000.0 /
            static_cast<double>(frequency);
    }

    double QpcSignedDeltaToMicroseconds(
        uint64_t start,
        uint64_t end,
        uint64_t frequency) noexcept
    {
        if (start == 0 || end == 0 || frequency == 0) {
            return std::numeric_limits<double>::quiet_NaN();
        }

        const double magnitude = static_cast<double>(
            end >= start ? end - start : start - end) * 1'000'000.0 /
            static_cast<double>(frequency);
        return end >= start ? magnitude : -magnitude;
    }

    struct SwapchainPclState
    {
        uint64_t last_display_qpc = 0;
        uint64_t last_simulation_qpc = 0;
        double input_to_frame_start_ema_us = 0.0;
        bool input_to_frame_start_initialized = false;
        bool logged_simulation_fallback = false;

        bool dropped_input_chain_active = false;
        double dropped_input_to_frame_start_us = 0.0;
        uint64_t last_dropped_simulation_qpc = 0;
        uint32_t dropped_frame_count = 0;
    };

    class MeasurementAccumulator
    {
    public:
        MeasurementAccumulator(
            uint32_t process_id,
            uint64_t qpc_frequency,
            uint32_t observer_epoch) noexcept
            : process_id_(process_id)
            , qpc_frequency_(qpc_frequency)
            , observer_epoch_(observer_epoch)
            , collection_start_qpc_(QueryQpc())
        {
        }

        // One accumulator is one collector session. Its final loss counters
        // join the process totals on every exit path, after the final ETW
        // status and drain have updated them.
        ~MeasurementAccumulator()
        {
            NativePclInternal_EndLossSession(loss_counters_);
        }

        MeasurementAccumulator(const MeasurementAccumulator&) = delete;
        MeasurementAccumulator& operator=(const MeasurementAccumulator&) = delete;

        void ProcessPresent(const std::shared_ptr<PresentEvent>& present)
        {
            if (!present || present->ProcessId != process_id_) {
                return;
            }

            // PresentMon skips incomplete individual presents and continues the
            // capture. These records do not prove ETW-wide integrity loss.
            if (present->IsLost) {
                if (tracking_synchronized_) {
                    ++loss_counters_.lost_present_count;
                    if (ShouldLogOccurrence(loss_counters_.lost_present_count)) {
                        LOG_WARN(
                            "Native PCL: skipped incomplete PresentEvent "
                            "(count=%u, present_qpc=%llu, swapchain=%llu)",
                            loss_counters_.lost_present_count,
                            static_cast<unsigned long long>(present->PresentStartTime),
                            static_cast<unsigned long long>(present->SwapChainAddress));
                    }
                }
                return;
            }

            if (present->PresentFailed || present->PresentStartTime == 0 ||
                present->FinalState == PresentResult::Unknown) {
                return;
            }

            tracking_synchronized_ = true;
            saw_process_present_ = true;
            if (present->PclSimStartTime != 0) {
                saw_pcl_timing_ = true;
            }

            SwapchainPclState& chain = swapchain_states_[present->SwapChainAddress];
            display_latencies_.clear();
            if (present->FinalState == PresentResult::Discarded) {
                ProcessNotDisplayedPresent(*present, chain);
                PublishPresentRecords(*present, nullptr, chain);
                UpdateLastSimulationStart(*present, chain);
                return;
            }

            if (present->FinalState != PresentResult::Presented) {
                return;
            }

            const auto displayed_entries = CopyAndSanitizeDisplayedEntries(*present);
            bool has_valid_display_timestamp = false;
            for (const auto& displayed : displayed_entries) {
                if (displayed.second == 0) {
                    continue;
                }

                has_valid_display_timestamp = true;
                NativePclDisplayLatency latency;
                if (ProcessDisplayedPresent(
                        *present,
                        displayed.second,
                        displayed.first,
                        chain,
                        &latency)) {
                    display_latencies_.push_back(latency);
                }
            }

            if (!has_valid_display_timestamp) {
                ProcessNotDisplayedPresent(*present, chain);
            }
            PublishPresentRecords(*present, &displayed_entries, chain);
            UpdateLastSimulationStart(*present, chain);
        }

        void UpdateEtwStatus(const EtwStatus& status) noexcept
        {
            const uint32_t previous_etw_events_lost = loss_counters_.etw_events_lost;
            const uint32_t previous_etw_buffers_lost = loss_counters_.etw_buffers_lost;
            const uint32_t previous_overflowed_present_count =
                loss_counters_.overflowed_present_count;

            // Session maxima; the published snapshot adds the totals of
            // earlier sessions (NativePclInternal_AccumulateLossCounters).
            loss_counters_.etw_events_lost = std::max(
                loss_counters_.etw_events_lost,
                static_cast<uint32_t>(status.mEtwEventsLost));
            loss_counters_.etw_buffers_lost = std::max(
                loss_counters_.etw_buffers_lost,
                static_cast<uint32_t>(status.mEtwBuffersLost));
            loss_counters_.overflowed_present_count = std::max(
                loss_counters_.overflowed_present_count,
                static_cast<uint32_t>(status.mNumOverflowedPresents));

            const bool counters_changed =
                loss_counters_.etw_events_lost != previous_etw_events_lost ||
                loss_counters_.etw_buffers_lost != previous_etw_buffers_lost ||
                loss_counters_.overflowed_present_count !=
                    previous_overflowed_present_count;
            if (counters_changed) {
                LOG_ERROR(
                    "Native PCL: capture integrity loss "
                    "(etw_events=%u, etw_buffers=%u, present_overflow=%u)",
                    loss_counters_.etw_events_lost,
                    loss_counters_.etw_buffers_lost,
                    loss_counters_.overflowed_present_count);
                LOG_ERROR(
                    "Native PCL: ETW buffer snapshot at integrity query "
                    "(in_use=%lu, allocated=%lu, occupancy=%.1f%%, "
                    "instantaneous=1)",
                    status.mEtwBuffersInUse,
                    status.mEtwTotalBuffers,
                    status.mEtwBufferFillPct);
            }

            // The window sees raw counters: a PresentMon tracking reset lowers
            // the overflow count, and later loss below the session maximum
            // must still open a window.
            const CaptureIntegrityLossWindow::Counters raw_counters{
                status.mEtwEventsLost,
                status.mEtwBuffersLost,
                status.mNumOverflowedPresents };
            const uint64_t now_qpc = QueryQpc();
            ReportLossWindowUpdate(
                loss_window_.Update(raw_counters, now_qpc, qpc_frequency_),
                now_qpc);
            if (loss_window_.IsActive()) {
                NativePclInternal_PublishLossStatus(loss_counters_);
            }
        }

        // Closes an elapsed loss window before a drained batch is published,
        // so samples recover without waiting for the next ETW status query.
        void AdvanceLossWindow(uint64_t now_qpc) noexcept
        {
            ReportLossWindowUpdate(
                loss_window_.Advance(now_qpc, qpc_frequency_),
                now_qpc);
        }

        void EvaluateAvailability(uint64_t now_qpc) noexcept
        {
            if (loss_window_.IsActive()) {
                NativePclInternal_PublishLossStatus(loss_counters_);
                return;
            }
            if (has_published_sample_) {
                return;
            }

            const double elapsed_ms = QpcDeltaToMicroseconds(
                collection_start_qpc_, now_qpc, qpc_frequency_) / 1000.0;
            if (saw_process_present_ && !saw_pcl_timing_ &&
                std::isfinite(elapsed_ms) &&
                elapsed_ms >= kNoPclStatsWarmupMilliseconds) {
                NativePclInternal_PublishStatus(
                    NativePclStatus::NoPclStats, true, false, false);
            }
            else {
                NativePclInternal_PublishStatus(
                    NativePclStatus::Warming, true, false, false);
            }
        }

    private:
        using DisplayEntry = std::pair<FrameType, uint64_t>;

        // Logs occurrences 1, 2, 4, 8, ... so repeated conditions stay sparse.
        static bool ShouldLogOccurrence(uint64_t count) noexcept
        {
            return count == 1 || (count & (count - 1)) == 0;
        }

        void ReportLossWindowUpdate(
            const NativePclLossWindowUpdate& update,
            uint64_t now_qpc) noexcept
        {
            if (update.ended) {
                const uint64_t ended_window =
                    loss_window_.WindowCount() - (update.began ? 1 : 0);
                if (ShouldLogOccurrence(ended_window)) {
                    LOG_INFO(
                        "Native PCL: ETW loss window %llu closed after %llu ms "
                        "without new loss; latency samples recover with the "
                        "next displayed frame",
                        static_cast<unsigned long long>(ended_window),
                        static_cast<unsigned long long>(
                            NATIVE_PCL_LOSS_WINDOW_QUIET_PERIOD_MILLISECONDS));
                }
                // Availability is re-established only by a sample published
                // after the window, never by values from inside it.
                has_published_sample_ = false;
            }
            if (update.began &&
                ShouldLogOccurrence(loss_window_.WindowCount())) {
                LOG_WARN(
                    "Native PCL: ETW loss window %llu opened; latency and "
                    "display samples are flagged until %llu ms pass without "
                    "new loss",
                    static_cast<unsigned long long>(loss_window_.WindowCount()),
                    static_cast<unsigned long long>(
                        NATIVE_PCL_LOSS_WINDOW_QUIET_PERIOD_MILLISECONDS));
            }
            if (update.ended && !update.active) {
                EvaluateAvailability(now_qpc);
            }
        }

        // Display-instance sanitization shared by latency samples, present
        // observations and pacing display samples:
        // 1. Adjacent Application/Repeated pairs collapse to the Application
        //    entry (below, PresentMon V2 duplicate sanitization).
        // 2. Callers then skip entries with a zero screen time: they are
        //    frame-type placeholders, not display instances.
        // Remaining screen times are forwarded unchanged and in order. Equal or
        // non-monotonic times are not dropped here; pacing telemetry counts
        // them as out-of-order chain breaks.
        static std::vector<DisplayEntry> CopyAndSanitizeDisplayedEntries(
            const PresentEvent& present)
        {
            std::vector<DisplayEntry> displayed_entries(
                present.Displayed.begin(), present.Displayed.end());

            // Match PresentMon's V2 duplicate sanitization. Adjacent app/repeated
            // pairs describe one display instance, so retain the application row.
            for (size_t index = 0; index + 1 < displayed_entries.size();) {
                const FrameType current_type = displayed_entries[index].first;
                const FrameType next_type = displayed_entries[index + 1].first;
                if (current_type == FrameType::Application &&
                    next_type == FrameType::Repeated) {
                    displayed_entries.erase(displayed_entries.begin() + index + 1);
                }
                else if (current_type == FrameType::Repeated &&
                         next_type == FrameType::Application) {
                    displayed_entries.erase(displayed_entries.begin() + index);
                }
                else {
                    ++index;
                }
            }

            return displayed_entries;
        }

        static bool HasInputPing(const PresentEvent& present) noexcept
        {
            return present.PclInputPingTime != 0;
        }

        static PresentObservationFinalState NormalizeFinalState(
            PresentResult final_state) noexcept
        {
            switch (final_state) {
                case PresentResult::Presented:
                    return PresentObservationFinalState::Presented;
                case PresentResult::Discarded:
                    return PresentObservationFinalState::Discarded;
                case PresentResult::Unknown:
                    return PresentObservationFinalState::Unknown;
                default:
                    return PresentObservationFinalState::Unknown;
            }
        }

        static PacingDisplayFrameType NormalizeDisplayFrameType(
            FrameType frame_type) noexcept
        {
            switch (frame_type) {
                case FrameType::Application:
                    return PacingDisplayFrameType::Application;
                case FrameType::Repeated:
                    return PacingDisplayFrameType::Repeated;
                case FrameType::Intel_XEFG:
                case FrameType::AMD_AFMF:
                    return PacingDisplayFrameType::VendorGenerated;
                case FrameType::NotSet:
                case FrameType::Unspecified:
                    return PacingDisplayFrameType::Unknown;
                default:
                    return PacingDisplayFrameType::Unknown;
            }
        }

        static PacingFrameClassification NormalizeFrameClassification(
            PresentFrameClassification classification) noexcept
        {
            switch (classification) {
                case PresentFrameClassification::Real:
                    return PacingFrameClassification::Real;
                case PresentFrameClassification::Generated:
                    return PacingFrameClassification::Generated;
                case PresentFrameClassification::Conflicting:
                    return PacingFrameClassification::Conflicting;
                case PresentFrameClassification::Unknown:
                    return PacingFrameClassification::Unknown;
                default:
                    return PacingFrameClassification::Unknown;
            }
        }

        // One observation per present feeds its pacing display samples: they
        // share its present-level fields and sanitized entries.
        void PublishPresentRecords(
            const PresentEvent& present,
            const std::vector<DisplayEntry>* displayed_entries,
            const SwapchainPclState& chain) noexcept
        {
            const PresentTimelineObservation observation =
                BuildPresentObservation(present, displayed_entries, chain);
            PublishDisplaySamples(present, displayed_entries, observation);
        }

        // One sample per sanitized display instance (non-zero screen time after
        // CopyAndSanitizeDisplayedEntries), in list order, carrying the PC
        // Latency this present published for that display time. A discarded
        // present, or one without such an instance, yields one sample with
        // display_qpc = 0, display_count = 0, frame_type Unknown and no PC
        // Latency.
        void PublishDisplaySamples(
            const PresentEvent& present,
            const std::vector<DisplayEntry>* displayed_entries,
            const PresentTimelineObservation& observation) noexcept
        {
            PacingDisplaySample sample{};
            sample.observer_epoch = observation.observer_epoch;
            sample.thread_id = observation.thread_id;
            sample.native_swapchain = observation.native_swapchain;
            sample.present_start_qpc = observation.present_start_qpc;
            sample.present_end_qpc = observation.present_end_qpc;
            sample.ready_qpc = observation.gpu_ready_qpc;
            sample.pcl_frame_id = observation.pcl_frame_id;
            sample.pcl_sim_start_qpc = observation.pcl_sim_start_qpc;
            sample.pcl_present_start_qpc = present.PclPresentStartTime;
            sample.final_state = static_cast<uint8_t>(observation.final_state);
            sample.presentation_mode =
                static_cast<uint8_t>(observation.presentation_mode);
            sample.frame_classification =
                NormalizeFrameClassification(observation.frame_classification);
            sample.supports_tearing =
                static_cast<uint8_t>(observation.supports_tearing ? 1 : 0);
            sample.loss_window =
                static_cast<uint8_t>(loss_window_.IsActive() ? 1 : 0);
            sample.sync_interval = observation.observed_sync_interval;
            sample.present_flags = observation.observed_present_flags;
            sample.flip_delay_qpc = observation.nvidia_flip_delay_qpc;
            sample.report_qpc = observation.report_qpc;
            sample.qpc_frequency = observation.qpc_frequency;

            constexpr uint32_t kMaximumDisplayInstances =
                (std::numeric_limits<uint16_t>::max)();
            const uint32_t display_count = std::min(
                observation.displayed_instance_count, kMaximumDisplayInstances);
            if (displayed_entries == nullptr || display_count == 0) {
                NativePclInternal_PublishDisplaySample(sample);
                return;
            }

            sample.display_count = static_cast<uint16_t>(display_count);
            uint32_t display_index = 0;
            for (const DisplayEntry& displayed : *displayed_entries) {
                if (displayed.second == 0) continue;
                if (display_index == display_count) break;

                PacingDisplaySample instance = sample;
                instance.display_qpc = displayed.second;
                instance.display_index = static_cast<uint16_t>(display_index);
                instance.frame_type = NormalizeDisplayFrameType(displayed.first);
                NativePclInternal_AttachDisplayLatency(
                    display_latencies_.data(), display_latencies_.size(), &instance);
                NativePclInternal_PublishDisplaySample(instance);
                ++display_index;
            }
        }

        PresentTimelineObservation BuildPresentObservation(
            const PresentEvent& present,
            const std::vector<DisplayEntry>* displayed_entries,
            const SwapchainPclState& chain) const noexcept
        {
            PresentTimelineObservation observation{};
            observation.observer_epoch = observer_epoch_;
            observation.final_state = NormalizeFinalState(present.FinalState);
            observation.native_swapchain = present.SwapChainAddress;
            observation.thread_id = present.ThreadId;
            observation.present_start_qpc = present.PresentStartTime;
            observation.present_end_qpc =
                present.TimeInPresent <=
                    (std::numeric_limits<uint64_t>::max)() -
                        present.PresentStartTime
                ? present.PresentStartTime + present.TimeInPresent
                : 0;
            observation.report_qpc = QueryQpc();
            observation.qpc_frequency = qpc_frequency_;
            observation.pcl_frame_id = present.PclFrameId;
            observation.pcl_sim_start_qpc = present.PclSimStartTime;
            observation.gpu_ready_qpc = present.ReadyTime;
            switch (present.PresentMode) {
            case PresentMode::Hardware_Legacy_Flip: observation.presentation_mode = PresentObservationMode::HardwareLegacyFlip; break;
            case PresentMode::Hardware_Legacy_Copy_To_Front_Buffer: observation.presentation_mode = PresentObservationMode::HardwareLegacyCopy; break;
            case PresentMode::Hardware_Independent_Flip: observation.presentation_mode = PresentObservationMode::HardwareIndependentFlip; break;
            case PresentMode::Composed_Flip: observation.presentation_mode = PresentObservationMode::ComposedFlip; break;
            case PresentMode::Composed_Copy_GPU_GDI: observation.presentation_mode = PresentObservationMode::ComposedCopyGpuGdi; break;
            case PresentMode::Composed_Copy_CPU_GDI: observation.presentation_mode = PresentObservationMode::ComposedCopyCpuGdi; break;
            case PresentMode::Hardware_Composed_Independent_Flip: observation.presentation_mode = PresentObservationMode::HardwareComposedIndependentFlip; break;
            default: break;
            }
            observation.observed_sync_interval = present.SyncInterval;
            observation.observed_present_flags = present.PresentFlags;
            observation.supports_tearing = present.SupportsTearing;
            observation.nvidia_flip_delay_qpc = present.FlipDelay;

            uint32_t evidence = PRESENT_CLASSIFICATION_EVIDENCE_NONE;
            if (present.PclSimStartTime != 0) {
                evidence |=
                    PRESENT_CLASSIFICATION_EVIDENCE_PCL_SIMULATION_MARKER;
            }

            bool has_reported_frame_type = false;
            int32_t reported_frame_type = 0;
            if (displayed_entries != nullptr) {
                for (const DisplayEntry& displayed : *displayed_entries) {
                    if (displayed.second == 0) continue;

                    ++observation.displayed_instance_count;
                    const int32_t frame_type =
                        static_cast<int32_t>(displayed.first);
                    if (!has_reported_frame_type) {
                        reported_frame_type = frame_type;
                        has_reported_frame_type = true;
                    }
                    else if (reported_frame_type != frame_type) {
                        reported_frame_type = -1;
                        evidence |=
                            PRESENT_CLASSIFICATION_EVIDENCE_MIXED_FRAME_TYPES;
                    }

                    switch (displayed.first) {
                        case FrameType::Application:
                            evidence |=
                                PRESENT_CLASSIFICATION_EVIDENCE_PRESENTMON_APPLICATION;
                            break;
                        case FrameType::Repeated:
                            evidence |=
                                PRESENT_CLASSIFICATION_EVIDENCE_PRESENTMON_REPEATED;
                            break;
                        case FrameType::Intel_XEFG:
                        case FrameType::AMD_AFMF:
                            evidence |=
                                PRESENT_CLASSIFICATION_EVIDENCE_PRESENTMON_GENERATED;
                            break;
                        default:
                            break;
                    }
                }
            }

            const bool used_simulation_fallback =
                observation.final_state == PresentObservationFinalState::Presented &&
                observation.displayed_instance_count != 0 &&
                present.PclSimStartTime == 0 &&
                chain.last_simulation_qpc != 0;
            if (used_simulation_fallback) {
                evidence |=
                    PRESENT_CLASSIFICATION_EVIDENCE_SIMULATION_FALLBACK;
            }
            observation.frame_classification =
                PresentAssociation_ClassifyFrame(evidence);
            return observation;
        }

        static void UpdateInputToFrameStartEma(
            SwapchainPclState& chain,
            double sample_us) noexcept
        {
            if (!std::isfinite(sample_us)) {
                return;
            }

            // Match PresentMon CalculateEma exactly, including its zero seed:
            // first EMA = 0 + 0.1 * (sample - 0).
            chain.input_to_frame_start_ema_us += kInputToFrameStartEmaAlpha *
                (sample_us - chain.input_to_frame_start_ema_us);
            chain.input_to_frame_start_initialized =
                chain.input_to_frame_start_ema_us != 0.0;
        }

        static void ClearDroppedInputChain(SwapchainPclState& chain) noexcept
        {
            chain.dropped_input_chain_active = false;
            chain.dropped_input_to_frame_start_us = 0.0;
            chain.last_dropped_simulation_qpc = 0;
            chain.dropped_frame_count = 0;
        }

        static void UpdateLastSimulationStart(
            const PresentEvent& present,
            SwapchainPclState& chain) noexcept
        {
            if (present.PclSimStartTime != 0) {
                chain.last_simulation_qpc = present.PclSimStartTime;
            }
            else if (present.AppSimStartTime != 0) {
                chain.last_simulation_qpc = present.AppSimStartTime;
            }
        }

        void FlagMalformedDroppedChain(SwapchainPclState& chain) noexcept
        {
            ++loss_counters_.lost_present_count;
            if (ShouldLogOccurrence(loss_counters_.lost_present_count)) {
                LOG_WARN(
                    "Native PCL: reset out-of-order dropped-present chain "
                    "(recoverable_count=%u, last_sim_qpc=%llu)",
                    loss_counters_.lost_present_count,
                    static_cast<unsigned long long>(
                        chain.last_dropped_simulation_qpc));
            }
            ClearDroppedInputChain(chain);
        }

        void ProcessNotDisplayedPresent(
            const PresentEvent& present,
            SwapchainPclState& chain) noexcept
        {
            if (present.PclSimStartTime == 0) {
                return;
            }

            if (HasInputPing(present)) {
                const double input_to_frame_start_us = QpcDeltaToMicroseconds(
                    present.PclInputPingTime,
                    present.PclSimStartTime,
                    qpc_frequency_);
                if (!std::isfinite(input_to_frame_start_us)) {
                    return;
                }

                chain.dropped_input_chain_active = true;
                chain.dropped_input_to_frame_start_us = input_to_frame_start_us;
                chain.last_dropped_simulation_qpc = present.PclSimStartTime;
                chain.dropped_frame_count = 1;
                return;
            }

            if (chain.dropped_input_chain_active) {
                const double simulation_delta_us = QpcDeltaToMicroseconds(
                    chain.last_dropped_simulation_qpc,
                    present.PclSimStartTime,
                    qpc_frequency_);
                if (!std::isfinite(simulation_delta_us)) {
                    FlagMalformedDroppedChain(chain);
                    return;
                }

                chain.dropped_input_to_frame_start_us += simulation_delta_us;
                ++chain.dropped_frame_count;
            }
            chain.last_dropped_simulation_qpc = present.PclSimStartTime;
        }

        // Returns true when a latency sample was published for this display
        // instance; *latency then holds that sample and status for the
        // instance's pacing display sample.
        bool ProcessDisplayedPresent(
            const PresentEvent& present,
            uint64_t display_qpc,
            FrameType frame_type,
            SwapchainPclState& chain,
            NativePclDisplayLatency* latency) noexcept
        {
            uint32_t accumulated_dropped_frames = 0;
            double dropped_accumulation_us = 0.0;

            // PCL input state changes only when this present carries a new PCL
            // simulation marker. Markerless generated displays reuse the last
            // simulation start without consuming or corrupting a dropped chain.
            if (present.PclSimStartTime != 0) {
                if (HasInputPing(present)) {
                    UpdateInputToFrameStartEma(
                        chain,
                        QpcDeltaToMicroseconds(
                            present.PclInputPingTime,
                            present.PclSimStartTime,
                            qpc_frequency_));
                    ClearDroppedInputChain(chain);
                }
                else if (chain.dropped_input_chain_active) {
                    const double final_simulation_delta_us = QpcDeltaToMicroseconds(
                        chain.last_dropped_simulation_qpc,
                        present.PclSimStartTime,
                        qpc_frequency_);
                    if (!std::isfinite(final_simulation_delta_us)) {
                        FlagMalformedDroppedChain(chain);
                        return false;
                    }

                    chain.dropped_input_to_frame_start_us += final_simulation_delta_us;
                    dropped_accumulation_us = chain.dropped_input_to_frame_start_us;
                    accumulated_dropped_frames = chain.dropped_frame_count;
                    UpdateInputToFrameStartEma(
                        chain, chain.dropped_input_to_frame_start_us);
                    ClearDroppedInputChain(chain);
                }
            }

            const double displayed_interval_ms = chain.last_display_qpc == 0
                ? 0.0
                : QpcDeltaToMicroseconds(
                      chain.last_display_qpc, display_qpc, qpc_frequency_) / 1000.0;
            chain.last_display_qpc = display_qpc;

            const uint64_t simulation_start_qpc = present.PclSimStartTime != 0
                ? present.PclSimStartTime
                : chain.last_simulation_qpc;
            if (!chain.input_to_frame_start_initialized ||
                simulation_start_qpc == 0) {
                return false;
            }

            const double frame_start_to_present_us = QpcSignedDeltaToMicroseconds(
                simulation_start_qpc,
                present.PresentStartTime,
                qpc_frequency_);
            const double present_to_display_us = QpcSignedDeltaToMicroseconds(
                present.PresentStartTime,
                display_qpc,
                qpc_frequency_);
            if (!std::isfinite(frame_start_to_present_us) ||
                !std::isfinite(present_to_display_us)) {
                return false;
            }

            if (present.PclSimStartTime == 0 &&
                !chain.logged_simulation_fallback) {
                LOG_INFO(
                    "Native PCL: using prior simulation start for markerless "
                    "display instances (swapchain=%llu, frame_type=%d)",
                    static_cast<unsigned long long>(present.SwapChainAddress),
                    static_cast<int>(frame_type));
                chain.logged_simulation_fallback = true;
            }

            NativePclInternalSample sample;
            sample.pcl_frame_id = present.PclFrameId;
            sample.present_start_qpc = present.PresentStartTime;
            sample.display_qpc = display_qpc;
            sample.sample_qpc = QueryQpc();
            sample.qpc_frequency = qpc_frequency_;
            sample.input_to_frame_start_us = chain.input_to_frame_start_ema_us;
            sample.frame_start_to_present_us = frame_start_to_present_us;
            sample.present_to_display_us = present_to_display_us;
            sample.total_us = chain.input_to_frame_start_ema_us +
                frame_start_to_present_us + present_to_display_us;
            sample.dropped_accumulation_us = dropped_accumulation_us;
            sample.dropped_frame_count = accumulated_dropped_frames;
            sample.effective_sim_start_qpc = simulation_start_qpc;
            sample.pcl_input_ping_qpc = present.PclInputPingTime;
            sample.used_simulation_fallback = present.PclSimStartTime == 0;

            NativePclStatus status = NativePclStatus::Native;
            if (loss_window_.IsActive()) {
                status = NativePclStatus::EventLoss;
            }
            else if (std::isfinite(displayed_interval_ms) &&
                     displayed_interval_ms > kLowFpsIntervalMilliseconds) {
                status = NativePclStatus::LowFpsUnreliable;
            }

            NativePclInternal_PublishSample(sample, status, loss_counters_);
            has_published_sample_ = true;
            latency->sample = sample;
            latency->status = status;
            latency->claimed = false;
            return true;
        }

        uint32_t process_id_ = 0;
        uint64_t qpc_frequency_ = 0;
        uint32_t observer_epoch_ = 0;
        uint64_t collection_start_qpc_ = 0;
        std::unordered_map<uint64_t, SwapchainPclState> swapchain_states_;

        bool tracking_synchronized_ = false;
        bool saw_process_present_ = false;
        bool saw_pcl_timing_ = false;
        // Cleared when a loss window closes: the next sample must re-establish
        // availability before EvaluateAvailability stops publishing warm-up.
        bool has_published_sample_ = false;
        CaptureIntegrityLossWindow loss_window_{};
        NativePclInternalLossCounters loss_counters_{};
        // Latency samples of the present being processed, one per display
        // instance that published one. Cleared per present; the capacity is
        // reused, so steady state does not allocate.
        std::vector<NativePclDisplayLatency> display_latencies_;
    };

    std::wstring MakeSessionName()
    {
        wchar_t buffer[96]{};
        const uint32_t nonce = g_session_nonce.fetch_add(1, std::memory_order_relaxed) + 1;
        std::swprintf(
            buffer,
            sizeof(buffer) / sizeof(buffer[0]),
            L"ReLimiter.NativePCL.%lu.%llu.%u",
            GetCurrentProcessId(),
            static_cast<unsigned long long>(GetTickCount64()),
            nonce);
        return buffer;
    }

    void DrainPresents(
        PMTraceConsumer& consumer,
        MeasurementAccumulator& accumulator,
        std::vector<std::shared_ptr<PresentEvent>>& presents) noexcept
    {
        try {
            consumer.DequeuePresentEvents(presents);
            accumulator.AdvanceLossWindow(QueryQpc());
            for (const auto& present : presents) {
                accumulator.ProcessPresent(present);
            }
        }
        catch (...) {
            LOG_ERROR("Native PCL: failed while draining PresentMon events");
        }
    }
}

void NativePclCollector_Run() noexcept
{
    try {
        if (NativePclInternal_StopRequested()) {
            NativePclInternal_PublishStatus(
                NativePclInternal_IsEnabled()
                    ? NativePclStatus::Stopped
                    : NativePclStatus::Disabled,
                false,
                false,
                false);
            return;
        }

        const uint32_t process_id = GetCurrentProcessId();
        NativePclEtw_ReclaimStaleSessions();

        PMTraceConsumer consumer;
        consumer.mTrackDisplay = true;
        consumer.mTrackPcLatency = true;
        consumer.mTrackFrameType = true;
        consumer.mFilteredProcessIds = true;
        consumer.AddTrackedProcessForFiltering(process_id);
        consumer.SetProviderToggleMode(true);

        PMTraceSession session;
        session.mPMConsumer = &consumer;
        NativePclDxgkrnlEventIsolation dxgkrnl_event_isolation(
            session,
            consumer);
        const std::wstring session_name = MakeSessionName();

        const ULONG start_status = NativePclEtw_StartSession(
            session,
            session_name.c_str());
        if (start_status != ERROR_SUCCESS) {
            const NativePclStatus status = start_status == ERROR_ACCESS_DENIED
                ? NativePclStatus::EtwAccessDenied
                : NativePclStatus::StartFailed;
            NativePclInternal_PublishStatus(status, false, false, false);
            LOG_ERROR(
                "Native PCL: ETW session start failed (status=%lu%s)",
                start_status,
                start_status == ERROR_ACCESS_DENIED ? ", access denied" : "");
            return;
        }

        consumer.mDeferralTimeLimit = session.MilliSecondsDeltaToTimestamp(
            kPresentDeferralMilliseconds);
        NativePclInternal_SetDrainEvent(consumer.hEventsReadyEvent);
        NativePclInternal_SetRunning(true);
        NativePclInternal_PublishStatus(
            NativePclStatus::Warming, true, false, false);

        HANDLE process_trace_ready_event = CreateEventW(
            nullptr,
            TRUE,
            FALSE,
            nullptr);
        if (process_trace_ready_event == nullptr) {
            const DWORD event_error = GetLastError();
            session.Stop();
            NativePclInternal_SetDrainEvent(nullptr);
            NativePclInternal_SetRunning(false);
            NativePclInternal_PublishStatus(
                NativePclStatus::StartFailed, false, false, false);
            LOG_ERROR(
                "Native PCL: failed to create ProcessTrace readiness event "
                "(error=%lu)",
                event_error);
            return;
        }

        std::atomic<bool> trace_finished{ false };
        std::atomic<ULONG> trace_result{ ERROR_SUCCESS };
        std::thread process_trace_thread;
        try {
            process_trace_thread = std::thread([&]() noexcept {
                // Diagnostic collection must yield to game work. Missing ETW
                // events is preferable to delaying a paced frame; existing loss
                // and continuity reporting remains authoritative.
                if (!SetThreadPriority(
                        GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL)) {
                    const DWORD priority_error = GetLastError();
                    LOG_WARN(
                        "Native PCL: failed to lower ProcessTrace thread priority "
                        "(error=%lu)",
                        priority_error);
                }

                // Signal immediately before entering ProcessTrace. The controller
                // waits for this handoff before it enables any provider; this is
                // a startup handshake, not a scheduling-priority requirement.
                SetEvent(process_trace_ready_event);
                TRACEHANDLE trace_handle = session.mTraceHandle;
                trace_result.store(
                    ProcessTrace(&trace_handle, 1, nullptr, nullptr),
                    std::memory_order_release);
                trace_finished.store(true, std::memory_order_release);
                SetEvent(consumer.hEventsReadyEvent);
            });
        }
        catch (...) {
            CloseHandle(process_trace_ready_event);
            session.Stop();
            NativePclInternal_SetDrainEvent(nullptr);
            NativePclInternal_SetRunning(false);
            NativePclInternal_PublishStatus(
                NativePclStatus::StartFailed, false, false, false);
            LOG_ERROR("Native PCL: failed to create ProcessTrace thread");
            return;
        }

        const DWORD process_trace_ready_status = WaitForSingleObject(
            process_trace_ready_event,
            kProcessTraceReadyTimeoutMilliseconds);
        const DWORD process_trace_ready_error =
            process_trace_ready_status == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
        if (process_trace_ready_status != WAIT_OBJECT_0 ||
            trace_finished.load(std::memory_order_acquire)) {
            session.Stop();
            if (process_trace_thread.joinable()) {
                process_trace_thread.join();
            }
            CloseHandle(process_trace_ready_event);
            NativePclInternal_SetDrainEvent(nullptr);
            NativePclInternal_SetRunning(false);
            NativePclInternal_PublishStatus(
                NativePclStatus::StartFailed, false, false, false);
            LOG_ERROR(
                "Native PCL: ProcessTrace readiness failed "
                "(wait_status=%lu, error=%lu, trace_status=%lu)",
                process_trace_ready_status,
                process_trace_ready_error,
                trace_result.load(std::memory_order_acquire));
            return;
        }
        CloseHandle(process_trace_ready_event);

        const char* provider_stage = "modern filtered providers";
        ULONG provider_status = NativePclEtw_StartModernFilteredProviders(
            session,
            consumer);
        if (provider_status == ERROR_SUCCESS) {
            // NVIDIA PCL is omitted from the general provider plan and admitted
            // exactly once with the current process filter.
            provider_stage = "NVIDIA PCL PID scope";
            provider_status = NativePclEtw_ConfigureNvidiaPclProcessScope(
                session, process_id);
        }
        if (provider_status == ERROR_SUCCESS) {
            provider_stage = "DxgKrnl event isolation";
            provider_status = dxgkrnl_event_isolation.Initialize();
        }

        if (provider_status != ERROR_SUCCESS) {
            session.Stop();
            if (process_trace_thread.joinable()) {
                process_trace_thread.join();
            }
            NativePclInternal_SetDrainEvent(nullptr);
            NativePclInternal_SetRunning(false);
            NativePclInternal_PublishStatus(
                NativePclStatus::StartFailed, false, false, false);
            LOG_ERROR(
                "Native PCL: ETW provider setup failed (stage=%s, status=%lu)",
                provider_stage,
                provider_status);
            return;
        }

        LOG_INFO(
            "Native PCL: ETW collection started "
            "(session=%ls, pid=%lu, process_trace_ready=1, "
            "pcl_pid_scope=enabled, pcl_unfiltered_window=eliminated, "
            "provider_plan=modern_filtered, legacy_graphics=excluded, "
            "dxgkrnl_wait_packets=excluded, event_isolation=armed)",
            session_name.c_str(),
            process_id);

        EtwStatus startup_etw_status{};
        if (session.QueryEtwStatus(&startup_etw_status)) {
            LOG_INFO(
                "Native PCL: ETW buffer snapshot "
                "(in_use=%lu, allocated=%lu, occupancy=%.1f%%, "
                "instantaneous=1)",
                startup_etw_status.mEtwBuffersInUse,
                startup_etw_status.mEtwTotalBuffers,
                startup_etw_status.mEtwBufferFillPct);
        }
        else {
            LOG_WARN("Native PCL: initial ETW buffer status query failed");
        }

        const uint64_t qpc_frequency = session.mTimestampFrequency.QuadPart > 0
            ? static_cast<uint64_t>(session.mTimestampFrequency.QuadPart)
            : QueryQpcFrequency();
        MeasurementAccumulator accumulator(
            process_id,
            qpc_frequency,
            NativePclInternal_BeginObserverEpoch());
        std::vector<std::shared_ptr<PresentEvent>> presents;
        constexpr uint32_t kMaximumEtwStatusQueryFailures = 3;
        uint32_t consecutive_status_query_failures = 0;
        ULONGLONG last_status_query_ms = GetTickCount64();
        bool drain_failed = false;

        try {
            while (!NativePclInternal_StopRequested() &&
                   !trace_finished.load(std::memory_order_acquire)) {
                const DWORD wait_status = WaitForSingleObject(
                    consumer.hEventsReadyEvent, kDrainWaitMilliseconds);
                if (wait_status == WAIT_OBJECT_0 || wait_status == WAIT_TIMEOUT) {
                    DrainPresents(consumer, accumulator, presents);
                }
                else {
                    LOG_ERROR(
                        "Native PCL: event wait failed (status=%lu, error=%lu)",
                        wait_status,
                        GetLastError());
                    drain_failed = true;
                    break;
                }

                const ULONGLONG now_ms = GetTickCount64();
                if (now_ms - last_status_query_ms >= kEtwStatusQueryMilliseconds) {
                    EtwStatus etw_status{};
                    if (!session.QueryEtwStatus(&etw_status)) {
                        ++consecutive_status_query_failures;
                        LOG_WARN(
                            "Native PCL: ETW status query failed "
                            "(attempt=%u/%u)",
                            consecutive_status_query_failures,
                            kMaximumEtwStatusQueryFailures);
                        if (consecutive_status_query_failures >=
                            kMaximumEtwStatusQueryFailures) {
                            const bool providers_restored =
                                dxgkrnl_event_isolation.AbortAndRestore();
                            LOG_ERROR(
                                "Native PCL: ETW status query failed repeatedly "
                                "(attempts=%u, providers_restored=%d)",
                                consecutive_status_query_failures,
                                providers_restored ? 1 : 0);
                            drain_failed = true;
                            break;
                        }
                        continue;
                    }

                    consecutive_status_query_failures = 0;
                    if (!dxgkrnl_event_isolation.Observe(
                            etw_status,
                            now_ms)) {
                        const bool providers_restored =
                            dxgkrnl_event_isolation.AbortAndRestore();
                        LOG_ERROR(
                            "Native PCL: DxgKrnl event isolation failed "
                            "(providers_restored=%d)",
                            providers_restored ? 1 : 0);
                        drain_failed = true;
                        break;
                    }
                    accumulator.UpdateEtwStatus(etw_status);
                    accumulator.EvaluateAvailability(QueryQpc());
                    last_status_query_ms = now_ms;
                }
            }
        }
        catch (...) {
            const bool providers_restored =
                dxgkrnl_event_isolation.AbortAndRestore();
            drain_failed = true;
            LOG_ERROR(
                "Native PCL: collector loop raised an exception "
                "(providers_restored=%d)",
                providers_restored ? 1 : 0);
        }

        EtwStatus final_status{};
        if (session.QueryEtwStatus(&final_status)) {
            accumulator.UpdateEtwStatus(final_status);
        }

        // Required shutdown order: stop ETW, wait for ProcessTrace to finish
        // consuming buffered events, then perform one final dequeue.
        session.Stop();
        if (process_trace_thread.joinable()) {
            process_trace_thread.join();
        }
        DrainPresents(consumer, accumulator, presents);

        if (session.mNumEventsLost != 0 || session.mNumBuffersLost != 0) {
            final_status.mEtwEventsLost = std::max(
                final_status.mEtwEventsLost, session.mNumEventsLost);
            final_status.mEtwBuffersLost = std::max(
                final_status.mEtwBuffersLost, session.mNumBuffersLost);
            accumulator.UpdateEtwStatus(final_status);
        }

        NativePclInternal_SetDrainEvent(nullptr);
        NativePclInternal_SetRunning(false);

        const bool stop_was_requested = NativePclInternal_StopRequested();
        const ULONG process_trace_status = trace_result.load(std::memory_order_acquire);
        if (!stop_was_requested) {
            NativePclInternal_PublishStatus(
                NativePclStatus::StartFailed, false, false, false);
            LOG_ERROR(
                "Native PCL: ETW processing stopped unexpectedly "
                "(trace_status=%lu, drain_failed=%d)",
                process_trace_status,
                drain_failed ? 1 : 0);
        }
        else {
            NativePclInternal_PublishStatus(
                NativePclInternal_IsEnabled()
                    ? NativePclStatus::Stopped
                    : NativePclStatus::Disabled,
                false,
                false,
                false);
            LOG_INFO("Native PCL: ETW collection stopped cleanly");
        }
    }
    catch (...) {
        NativePclInternal_SetDrainEvent(nullptr);
        NativePclInternal_SetRunning(false);
        NativePclInternal_PublishStatus(
            NativePclStatus::StartFailed, false, false, false);
        LOG_ERROR("Native PCL: collector initialization raised an exception");
    }
}
