#include "native_pcl_etw_diagnostics.h"

#include <evntrace.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <vector>

#include "ETW/Microsoft_Windows_DxgKrnl.h"
#include "IFilterBuildListener.h"
#include "PresentMonTraceConsumer.hpp"
#include "PresentMonTraceSession.hpp"
#include "logger.h"
#include "native_pcl_etw_provider_setup.h"

namespace
{
    constexpr ULONGLONG kIsolationSettleMilliseconds = 1000;
    constexpr ULONGLONG kIsolationMeasureMilliseconds = 2000;

    struct DxgkrnlEventDefinition
    {
        uint16_t event_id;
        const char* event_name;
    };

    // Put the highest-volume candidates first so a truncated capture still
    // provides useful evidence. Every retained event is measured exactly once.
    const std::array<DxgkrnlEventDefinition, 17> kDxgkrnlEventDefinitions = {{
        { Microsoft_Windows_DxgKrnl::QueuePacket_Stop::Id, "queue_packet_stop" },
        { Microsoft_Windows_DxgKrnl::QueuePacket_Start::Id, "queue_packet_start" },
        { Microsoft_Windows_DxgKrnl::VSyncDPCMultiPlane_Info::Id, "vsync_dpc_multiplane" },
        { Microsoft_Windows_DxgKrnl::HSyncDPCMultiPlane_Info::Id, "hsync_dpc_multiplane" },
        { Microsoft_Windows_DxgKrnl::VSyncDPC_Info::Id, "vsync_dpc" },
        { Microsoft_Windows_DxgKrnl::MMIOFlipMultiPlaneOverlay3_Info::Id, "mmio_flip_multiplane3" },
        { Microsoft_Windows_DxgKrnl::MMIOFlipMultiPlaneOverlay_Info::Id, "mmio_flip_multiplane" },
        { Microsoft_Windows_DxgKrnl::MMIOFlip_Info::Id, "mmio_flip" },
        { Microsoft_Windows_DxgKrnl::PresentHistoryDetailed_Start::Id, "present_history_detailed" },
        { Microsoft_Windows_DxgKrnl::PresentHistory_Start::Id, "present_history_start" },
        { Microsoft_Windows_DxgKrnl::PresentHistory_Info::Id, "present_history_info" },
        { Microsoft_Windows_DxgKrnl::Present_Info::Id, "present_info" },
        { Microsoft_Windows_DxgKrnl::FlipMultiPlaneOverlay_Info::Id, "flip_multiplane" },
        { Microsoft_Windows_DxgKrnl::IndependentFlip_Info::Id, "independent_flip" },
        { Microsoft_Windows_DxgKrnl::Flip_Info::Id, "flip" },
        { Microsoft_Windows_DxgKrnl::Blit_Info::Id, "blit" },
        { Microsoft_Windows_DxgKrnl::BlitCancel_Info::Id, "blit_cancel" },
    }};

    struct DxgkrnlProviderConfiguration
    {
        std::vector<uint16_t> event_ids;
        uint64_t any_keyword_mask = 0;
        uint64_t all_keyword_mask = 0;
        uint8_t maximum_level = 0;
    };

    class DxgkrnlFilterCapture final : public IFilterBuildListener
    {
    public:
        void EventAdded(uint16_t event_id) override
        {
            pending_event_ids_.push_back(event_id);
        }

        void ProviderEnabled(
            const GUID& provider_guid,
            uint64_t any_keyword_mask,
            uint64_t all_keyword_mask,
            uint8_t maximum_level,
            uint32_t control_code) override
        {
            if (!captured_ &&
                IsEqualGUID(provider_guid, Microsoft_Windows_DxgKrnl::GUID) &&
                control_code == EVENT_CONTROL_CODE_ENABLE_PROVIDER) {
                configuration_.event_ids = pending_event_ids_;
                configuration_.any_keyword_mask = any_keyword_mask;
                configuration_.all_keyword_mask = all_keyword_mask;
                configuration_.maximum_level = maximum_level;
                captured_ = true;
            }

            pending_event_ids_.clear();
        }

        void ClearEvents() override
        {
            pending_event_ids_.clear();
        }

        bool HasConfiguration() const noexcept
        {
            return captured_ && !configuration_.event_ids.empty();
        }

        const DxgkrnlProviderConfiguration& Configuration() const noexcept
        {
            return configuration_;
        }

    private:
        std::vector<uint16_t> pending_event_ids_;
        DxgkrnlProviderConfiguration configuration_;
        bool captured_ = false;
    };

    bool IsWindows11OrGreater() noexcept
    {
        using RtlGetVersionFunction = LONG(WINAPI*)(OSVERSIONINFOW*);

        const HMODULE ntdll_module = GetModuleHandleW(L"ntdll.dll");
        if (ntdll_module == nullptr) {
            return false;
        }

        const auto rtl_get_version = reinterpret_cast<RtlGetVersionFunction>(
            GetProcAddress(ntdll_module, "RtlGetVersion"));
        if (rtl_get_version == nullptr) {
            return false;
        }

        OSVERSIONINFOW version_information{};
        version_information.dwOSVersionInfoSize = sizeof(version_information);
        if (rtl_get_version(&version_information) != 0) {
            return false;
        }

        return version_information.dwMajorVersion > 10 ||
            (version_information.dwMajorVersion == 10 &&
             version_information.dwBuildNumber >= 22000);
    }

    ULONG CaptureDxgkrnlProviderConfiguration(
        const PMTraceConsumer& consumer,
        DxgkrnlProviderConfiguration& configuration)
    {
        auto capture = std::make_shared<DxgkrnlFilterCapture>();
        const ULONG status = EnableProvidersListing(
            0,
            nullptr,
            &consumer,
            true,
            IsWindows11OrGreater(),
            capture);
        if (status != ERROR_SUCCESS) {
            return status;
        }
        if (!capture->HasConfiguration()) {
            return ERROR_INVALID_DATA;
        }

        configuration = capture->Configuration();
        return ERROR_SUCCESS;
    }

    bool PrepareDxgkrnlProviderConfiguration(
        DxgkrnlProviderConfiguration& configuration) noexcept
    {
        const uint16_t unused_wait_packet_event_id =
            Microsoft_Windows_DxgKrnl::QueuePacket_Start_2::Id;
        if (std::count(
                configuration.event_ids.begin(),
                configuration.event_ids.end(),
                unused_wait_packet_event_id) != 1) {
            return false;
        }

        configuration.event_ids.erase(
            std::remove(
                configuration.event_ids.begin(),
                configuration.event_ids.end(),
                unused_wait_packet_event_id),
            configuration.event_ids.end());
        if (configuration.event_ids.size() !=
            kDxgkrnlEventDefinitions.size()) {
            return false;
        }

        for (const DxgkrnlEventDefinition& definition :
             kDxgkrnlEventDefinitions) {
            if (std::count(
                    configuration.event_ids.begin(),
                    configuration.event_ids.end(),
                    definition.event_id) != 1) {
                return false;
            }
        }
        return true;
    }

    ULONG EnableDxgkrnlProvider(
        PMTraceSession& session,
        const DxgkrnlProviderConfiguration& configuration)
    {
        return NativePclEtw_EnableFilteredProvider(
            session,
            Microsoft_Windows_DxgKrnl::GUID,
            configuration.event_ids,
            configuration.any_keyword_mask,
            configuration.all_keyword_mask,
            configuration.maximum_level);
    }

    ULONG DisableProvider(PMTraceSession& session, const GUID& provider_guid)
    {
        return EnableTraceEx2(
            session.mSessionHandle,
            &provider_guid,
            EVENT_CONTROL_CODE_DISABLE_PROVIDER,
            0,
            0,
            0,
            0,
            nullptr);
    }

    bool ProviderDisableSucceeded(ULONG status) noexcept
    {
        return status == ERROR_SUCCESS || status == ERROR_NOT_FOUND;
    }

    uint64_t CounterDelta(ULONG current, ULONG previous) noexcept
    {
        return current >= previous
            ? static_cast<uint64_t>(current - previous)
            : static_cast<uint64_t>(current);
    }

    double EventsPerSecond(
        uint64_t event_count,
        ULONGLONG duration_ms) noexcept
    {
        if (duration_ms == 0) {
            return 0.0;
        }
        return static_cast<double>(event_count) * 1000.0 /
            static_cast<double>(duration_ms);
    }
}

struct NativePclDxgkrnlEventIsolation::Implementation
{
    enum class Phase : uint8_t
    {
        Monitoring,
        Settling,
        Measuring,
        Complete,
    };

    Implementation(
        PMTraceSession& trace_session,
        PMTraceConsumer& trace_consumer) noexcept
        : session(trace_session)
        , consumer(trace_consumer)
    {
    }

    ULONG Initialize()
    {
        const ULONG capture_status = CaptureDxgkrnlProviderConfiguration(
            consumer,
            full_configuration);
        if (capture_status != ERROR_SUCCESS) {
            LOG_ERROR(
                "Native PCL: failed to capture DxgKrnl provider configuration "
                "(status=%lu)",
                capture_status);
            return capture_status;
        }
        if (!PrepareDxgkrnlProviderConfiguration(full_configuration)) {
            LOG_ERROR(
                "Native PCL: DxgKrnl provider configuration did not match "
                "the expected 17 unique retained events");
            return ERROR_INVALID_DATA;
        }

        const ULONG enable_status = EnableDxgkrnlProvider(
            session,
            full_configuration);
        if (enable_status != ERROR_SUCCESS) {
            LOG_ERROR(
                "Native PCL: failed to narrow DxgKrnl provider filter "
                "(status=%lu)",
                enable_status);
            return enable_status;
        }

        initialized = true;
        providers_in_normal_configuration = true;
        restore_attempted = false;
        restore_succeeded = false;
        observation_baseline_established = false;
        LOG_INFO(
            "Native PCL: DxgKrnl admission filter active "
            "(excluded=queue_packet_start_2, retained_events=%lu, "
            "event_isolation=armed)",
            static_cast<unsigned long>(full_configuration.event_ids.size()));
        return ERROR_SUCCESS;
    }

    bool Observe(const EtwStatus& status, ULONGLONG now_ms)
    {
        if (!initialized) {
            LOG_ERROR("Native PCL: DxgKrnl event isolation was not initialized");
            return false;
        }
        if (phase == Phase::Complete) {
            return true;
        }
        if (!observation_baseline_established) {
            previous_events_lost = status.mEtwEventsLost;
            previous_observation_ms = now_ms;
            observation_baseline_established = true;
            return true;
        }

        if (phase == Phase::Monitoring) {
            const uint64_t event_delta = CounterDelta(
                status.mEtwEventsLost,
                previous_events_lost);
            const ULONGLONG duration_ms =
                now_ms - previous_observation_ms;
            if (event_delta != 0 && duration_ms != 0) {
                baseline_events_per_second = EventsPerSecond(
                    event_delta,
                    duration_ms);
                LOG_INFO(
                    "Native PCL: DxgKrnl event isolation started "
                    "(baseline_rate=%.1f events/s, lost=%lu, "
                    "candidates=%lu)",
                    baseline_events_per_second,
                    status.mEtwEventsLost,
                    static_cast<unsigned long>(
                        kDxgkrnlEventDefinitions.size()));
                if (!ApplyExclusion(0)) {
                    phase = Phase::Complete;
                    return false;
                }
            }
        }
        else if (phase == Phase::Settling) {
            if (now_ms - phase_started_ms >=
                kIsolationSettleMilliseconds) {
                measurement_started_ms = now_ms;
                measurement_started_events_lost = status.mEtwEventsLost;
                phase = Phase::Measuring;
                if (control_stage) {
                    LOG_INFO(
                        "Native PCL: DxgKrnl event isolation measuring "
                        "(stage=dxgkrnl_off, starting_lost=%lu)",
                        measurement_started_events_lost);
                }
                else {
                    const auto& definition =
                        kDxgkrnlEventDefinitions[current_exclusion_index];
                    LOG_INFO(
                        "Native PCL: DxgKrnl event isolation measuring "
                        "(excluded=%s, event_id=%u, starting_lost=%lu)",
                        definition.event_name,
                        definition.event_id,
                        measurement_started_events_lost);
                }
            }
        }
        else if (phase == Phase::Measuring &&
                 now_ms - measurement_started_ms >=
                     kIsolationMeasureMilliseconds) {
            FinishMeasurement(status, now_ms);

            if (control_stage) {
                if (!RestoreProviders()) {
                    phase = Phase::Complete;
                    return false;
                }
                phase = Phase::Complete;
                LOG_INFO(
                    "Native PCL: DxgKrnl event isolation complete "
                    "(baseline_rate=%.1f events/s, lowest_excluded=%s, "
                    "lowest_rate=%.1f events/s, dxgkrnl_off_rate=%.1f "
                    "events/s, providers=restored)",
                    baseline_events_per_second,
                    kDxgkrnlEventDefinitions[
                        lowest_exclusion_index].event_name,
                    lowest_exclusion_rate,
                    control_events_per_second);
            }
            else if (current_exclusion_index + 1 <
                     kDxgkrnlEventDefinitions.size()) {
                if (!ApplyExclusion(current_exclusion_index + 1)) {
                    phase = Phase::Complete;
                    return false;
                }
            }
            else if (!ApplyDxgkrnlOffControl()) {
                phase = Phase::Complete;
                return false;
            }
        }

        previous_events_lost = status.mEtwEventsLost;
        previous_observation_ms = now_ms;
        return true;
    }

    bool ApplyExclusion(size_t exclusion_index)
    {
        DxgkrnlProviderConfiguration configuration = full_configuration;
        const uint16_t excluded_event_id =
            kDxgkrnlEventDefinitions[exclusion_index].event_id;
        const auto excluded_event = std::find(
            configuration.event_ids.begin(),
            configuration.event_ids.end(),
            excluded_event_id);
        if (excluded_event == configuration.event_ids.end()) {
            LOG_ERROR(
                "Native PCL: DxgKrnl isolation event was absent "
                "(event_id=%u)",
                excluded_event_id);
            return false;
        }
        configuration.event_ids.erase(excluded_event);

        if (!TransitionToFilter(configuration)) {
            return false;
        }

        current_exclusion_index = exclusion_index;
        control_stage = false;
        phase = Phase::Settling;
        phase_started_ms = GetTickCount64();
        LOG_INFO(
            "Native PCL: DxgKrnl event isolation stage started "
            "(excluded=%s, event_id=%u)",
            kDxgkrnlEventDefinitions[exclusion_index].event_name,
            excluded_event_id);
        return true;
    }

    bool ApplyDxgkrnlOffControl()
    {
        if (!QuiesceConsumer("dxgkrnl_off")) {
            return false;
        }

        const ULONG disable_status = DisableProvider(
            session,
            Microsoft_Windows_DxgKrnl::GUID);
        if (!ProviderDisableSucceeded(disable_status)) {
            consumer.SetEventProcessingEnabled(true);
            LOG_ERROR(
                "Native PCL: failed to disable modern DxgKrnl provider for "
                "control (status=%lu)",
                disable_status);
            return false;
        }

        providers_in_normal_configuration = false;
        consumer.ResetPresentTrackingData(false);
        consumer.SetEventProcessingEnabled(true);
        control_stage = true;
        phase = Phase::Settling;
        phase_started_ms = GetTickCount64();
        LOG_INFO(
            "Native PCL: DxgKrnl event isolation stage started "
            "(stage=dxgkrnl_off)");
        return true;
    }

    bool TransitionToFilter(
        const DxgkrnlProviderConfiguration& configuration)
    {
        if (!QuiesceConsumer("event_filter")) {
            return false;
        }

        const ULONG disable_status = DisableProvider(
            session,
            Microsoft_Windows_DxgKrnl::GUID);
        if (!ProviderDisableSucceeded(disable_status)) {
            consumer.SetEventProcessingEnabled(true);
            LOG_ERROR(
                "Native PCL: failed to disable modern DxgKrnl provider "
                "during event isolation (status=%lu)",
                disable_status);
            return false;
        }

        providers_in_normal_configuration = false;
        consumer.ResetPresentTrackingData(false);
        const ULONG enable_status = EnableDxgkrnlProvider(
            session,
            configuration);
        consumer.SetEventProcessingEnabled(true);
        if (enable_status != ERROR_SUCCESS) {
            LOG_ERROR(
                "Native PCL: failed to apply DxgKrnl event isolation filter "
                "(status=%lu)",
                enable_status);
            return false;
        }
        return true;
    }

    bool RestoreProviders()
    {
        if (providers_in_normal_configuration) {
            restore_attempted = true;
            restore_succeeded = true;
            return true;
        }
        if (restore_attempted) {
            return restore_succeeded;
        }
        restore_attempted = true;

        if (!QuiesceConsumer("provider_restore")) {
            return false;
        }

        const ULONG modern_disable_status = DisableProvider(
            session,
            Microsoft_Windows_DxgKrnl::GUID);
        if (!ProviderDisableSucceeded(modern_disable_status)) {
            consumer.SetEventProcessingEnabled(true);
            LOG_ERROR(
                "Native PCL: failed to prepare modern DxgKrnl provider "
                "restoration (status=%lu)",
                modern_disable_status);
            return false;
        }

        consumer.ResetPresentTrackingData(false);
        const ULONG modern_enable_status = EnableDxgkrnlProvider(
            session,
            full_configuration);
        consumer.SetEventProcessingEnabled(true);
        if (modern_enable_status != ERROR_SUCCESS) {
            LOG_ERROR(
                "Native PCL: failed to restore modern DxgKrnl provider "
                "(status=%lu)",
                modern_enable_status);
            return false;
        }

        providers_in_normal_configuration = true;
        restore_succeeded = true;
        return true;
    }

    bool QuiesceConsumer(const char* transition_name)
    {
        constexpr ULONGLONG kQuiesceTimeoutMilliseconds = 2000;

        consumer.SetEventProcessingEnabled(false);
        const ULONGLONG started_ms = GetTickCount64();
        LONG in_flight = InterlockedCompareExchange(
            &consumer.mEventProcessingInFlight,
            0,
            0);
        while (in_flight != 0) {
            if (GetTickCount64() - started_ms >=
                kQuiesceTimeoutMilliseconds) {
                consumer.SetEventProcessingEnabled(true);
                LOG_ERROR(
                    "Native PCL: timed out quiescing ETW consumer "
                    "(transition=%s, in_flight=%ld)",
                    transition_name,
                    in_flight);
                return false;
            }

            Sleep(1);
            in_flight = InterlockedCompareExchange(
                &consumer.mEventProcessingInFlight,
                0,
                0);
        }
        return true;
    }

    bool AbortAndRestore()
    {
        if (!initialized || providers_in_normal_configuration) {
            phase = Phase::Complete;
            return true;
        }
        if (restore_attempted) {
            return restore_succeeded;
        }
        if (!RestoreProviders()) {
            return false;
        }

        phase = Phase::Complete;
        LOG_WARN(
            "Native PCL: DxgKrnl event isolation aborted "
            "(providers=restored)");
        return true;
    }

    void FinishMeasurement(const EtwStatus& status, ULONGLONG now_ms)
    {
        const uint64_t event_delta = CounterDelta(
            status.mEtwEventsLost,
            measurement_started_events_lost);
        const ULONGLONG duration_ms = now_ms - measurement_started_ms;
        const double events_per_second = EventsPerSecond(
            event_delta,
            duration_ms);

        if (control_stage) {
            control_events_per_second = events_per_second;
            LOG_INFO(
                "Native PCL: DxgKrnl event isolation result "
                "(stage=dxgkrnl_off, lost_delta=%llu, duration_ms=%llu, "
                "rate=%.1f events/s, buffers=%lu/%lu, fill=%.1f%%)",
                static_cast<unsigned long long>(event_delta),
                static_cast<unsigned long long>(duration_ms),
                events_per_second,
                status.mEtwBuffersInUse,
                status.mEtwTotalBuffers,
                status.mEtwBufferFillPct);
            return;
        }

        if (events_per_second < lowest_exclusion_rate) {
            lowest_exclusion_rate = events_per_second;
            lowest_exclusion_index = current_exclusion_index;
        }
        const auto& definition =
            kDxgkrnlEventDefinitions[current_exclusion_index];
        LOG_INFO(
            "Native PCL: DxgKrnl event isolation result "
            "(excluded=%s, event_id=%u, lost_delta=%llu, "
            "duration_ms=%llu, rate=%.1f events/s, buffers=%lu/%lu, "
            "fill=%.1f%%)",
            definition.event_name,
            definition.event_id,
            static_cast<unsigned long long>(event_delta),
            static_cast<unsigned long long>(duration_ms),
            events_per_second,
            status.mEtwBuffersInUse,
            status.mEtwTotalBuffers,
            status.mEtwBufferFillPct);
    }

    PMTraceSession& session;
    PMTraceConsumer& consumer;
    DxgkrnlProviderConfiguration full_configuration;

    Phase phase = Phase::Monitoring;
    size_t current_exclusion_index = 0;
    size_t lowest_exclusion_index = 0;
    bool initialized = false;
    bool observation_baseline_established = false;
    bool control_stage = false;
    bool providers_in_normal_configuration = false;
    bool restore_attempted = false;
    bool restore_succeeded = false;
    ULONGLONG previous_observation_ms = 0;
    ULONGLONG phase_started_ms = 0;
    ULONGLONG measurement_started_ms = 0;
    ULONG previous_events_lost = 0;
    ULONG measurement_started_events_lost = 0;
    double baseline_events_per_second = 0.0;
    double lowest_exclusion_rate = std::numeric_limits<double>::infinity();
    double control_events_per_second = 0.0;
};

NativePclDxgkrnlEventIsolation::NativePclDxgkrnlEventIsolation(
    PMTraceSession& session,
    PMTraceConsumer& consumer) noexcept
    : implementation_(new (std::nothrow) Implementation(session, consumer))
{
}

NativePclDxgkrnlEventIsolation::~NativePclDxgkrnlEventIsolation() = default;

ULONG NativePclDxgkrnlEventIsolation::Initialize() noexcept
{
    if (!implementation_) {
        LOG_ERROR(
            "Native PCL: insufficient memory for DxgKrnl event isolation");
        return ERROR_OUTOFMEMORY;
    }

    try {
        return implementation_->Initialize();
    }
    catch (const std::bad_alloc&) {
        LOG_ERROR(
            "Native PCL: insufficient memory while configuring DxgKrnl "
            "event isolation");
        return ERROR_OUTOFMEMORY;
    }
    catch (...) {
        LOG_ERROR(
            "Native PCL: unexpected failure while configuring DxgKrnl "
            "event isolation");
        return ERROR_INVALID_DATA;
    }
}

bool NativePclDxgkrnlEventIsolation::Observe(
    const EtwStatus& status,
    ULONGLONG now_ms) noexcept
{
    if (!implementation_) {
        return false;
    }

    try {
        return implementation_->Observe(status, now_ms);
    }
    catch (const std::bad_alloc&) {
        LOG_ERROR(
            "Native PCL: insufficient memory during DxgKrnl event isolation");
        return false;
    }
    catch (...) {
        LOG_ERROR(
            "Native PCL: unexpected failure during DxgKrnl event isolation");
        return false;
    }
}

bool NativePclDxgkrnlEventIsolation::AbortAndRestore() noexcept
{
    if (!implementation_) {
        return false;
    }

    try {
        return implementation_->AbortAndRestore();
    }
    catch (...) {
        LOG_ERROR(
            "Native PCL: unexpected failure restoring DxgKrnl providers");
        return false;
    }
}
