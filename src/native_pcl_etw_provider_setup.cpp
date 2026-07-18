#include "native_pcl_etw_provider_setup.h"

#include <evntrace.h>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <memory>
#include <new>
#include <utility>
#include <vector>

#include "ETW/Microsoft_Windows_Dwm_Core_Win7.h"
#include "ETW/Microsoft_Windows_DxgKrnl.h"
#include "ETW/Microsoft_Windows_DxgKrnl_Win7.h"
#include "ETW/Microsoft_Windows_Kernel_Process.h"
#include "ETW/Nvidia_PCL.h"
#include "IFilterBuildListener.h"
#include "PresentMonTraceConsumer.hpp"
#include "PresentMonTraceSession.hpp"
#include "logger.h"

namespace
{
    constexpr size_t kExpectedRetainedDxgkrnlEventCount = 17;

    struct WindowsProviderCapabilities
    {
        bool supports_event_id_filters = false;
        bool is_windows_11_or_greater = false;
    };

    struct ProviderConfiguration
    {
        GUID provider_guid{};
        std::vector<uint16_t> event_ids;
        uint64_t any_keyword_mask = 0;
        uint64_t all_keyword_mask = 0;
        uint8_t maximum_level = 0;
        uint32_t control_code = EVENT_CONTROL_CODE_ENABLE_PROVIDER;
    };

    class ProviderConfigurationCapture final : public IFilterBuildListener
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
            ProviderConfiguration configuration;
            configuration.provider_guid = provider_guid;
            configuration.event_ids = pending_event_ids_;
            configuration.any_keyword_mask = any_keyword_mask;
            configuration.all_keyword_mask = all_keyword_mask;
            configuration.maximum_level = maximum_level;
            configuration.control_code = control_code;
            configurations_.push_back(std::move(configuration));
            pending_event_ids_.clear();
        }

        void ClearEvents() override
        {
            pending_event_ids_.clear();
        }

        const std::vector<ProviderConfiguration>& Configurations() const noexcept
        {
            return configurations_;
        }

    private:
        std::vector<uint16_t> pending_event_ids_;
        std::vector<ProviderConfiguration> configurations_;
    };

    bool QueryWindowsProviderCapabilities(
        WindowsProviderCapabilities& capabilities) noexcept
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

        capabilities.supports_event_id_filters =
            version_information.dwMajorVersion > 6 ||
            (version_information.dwMajorVersion == 6 &&
             version_information.dwMinorVersion >= 3);
        capabilities.is_windows_11_or_greater =
            version_information.dwMajorVersion > 10 ||
            (version_information.dwMajorVersion == 10 &&
             version_information.dwBuildNumber >= 22000);
        return true;
    }

    ULONG EnableFilteredProvider(
        PMTraceSession& session,
        const GUID& provider_guid,
        const std::vector<uint16_t>& event_ids,
        uint64_t any_keyword_mask,
        uint64_t all_keyword_mask,
        uint8_t maximum_level,
        uint32_t control_code)
    {
        if (event_ids.empty() ||
            event_ids.size() > MAX_EVENT_FILTER_EVENT_ID_COUNT) {
            return ERROR_INVALID_DATA;
        }

        const size_t filter_size =
            offsetof(EVENT_FILTER_EVENT_ID, Events) +
            event_ids.size() * sizeof(USHORT);
        if (filter_size > std::numeric_limits<ULONG>::max()) {
            return ERROR_ARITHMETIC_OVERFLOW;
        }

        const size_t filter_word_count =
            (filter_size + sizeof(USHORT) - 1) / sizeof(USHORT);
        std::vector<USHORT> filter_storage(filter_word_count, 0);
        auto* event_filter = reinterpret_cast<EVENT_FILTER_EVENT_ID*>(
            filter_storage.data());
        event_filter->FilterIn = TRUE;
        event_filter->Reserved = 0;
        event_filter->Count = static_cast<USHORT>(event_ids.size());
        std::copy(event_ids.begin(), event_ids.end(), event_filter->Events);

        EVENT_FILTER_DESCRIPTOR filter_descriptor{};
        filter_descriptor.Ptr = reinterpret_cast<ULONGLONG>(event_filter);
        filter_descriptor.Size = static_cast<ULONG>(filter_size);
        filter_descriptor.Type = EVENT_FILTER_TYPE_EVENT_ID;

        ENABLE_TRACE_PARAMETERS enable_parameters{};
        enable_parameters.Version = ENABLE_TRACE_PARAMETERS_VERSION_2;
        enable_parameters.EnableProperty =
            EVENT_ENABLE_PROPERTY_IGNORE_KEYWORD_0;
        enable_parameters.SourceId = session.mSessionGuid;
        enable_parameters.EnableFilterDesc = &filter_descriptor;
        enable_parameters.FilterDescCount = 1;

        return EnableTraceEx2(
            session.mSessionHandle,
            &provider_guid,
            control_code,
            maximum_level,
            any_keyword_mask,
            all_keyword_mask,
            0,
            &enable_parameters);
    }

    bool IsProvider(const GUID& candidate, const GUID& expected) noexcept
    {
        return IsEqualGUID(candidate, expected) != FALSE;
    }
}

ULONG NativePclEtw_StartModernFilteredProviders(
    PMTraceSession& session,
    PMTraceConsumer& consumer) noexcept
{
    try {
        WindowsProviderCapabilities capabilities;
        if (!QueryWindowsProviderCapabilities(capabilities)) {
            LOG_ERROR(
                "Native PCL: failed to determine ETW provider capabilities");
            return ERROR_OLD_WIN_VERSION;
        }
        if (!capabilities.supports_event_id_filters) {
            LOG_ERROR(
                "Native PCL: modern EventId-filtered ETW providers require "
                "Windows 8.1 or newer");
            return ERROR_OLD_WIN_VERSION;
        }

        auto capture = std::make_shared<ProviderConfigurationCapture>();
        const ULONG capture_status = EnableProvidersListing(
            0,
            nullptr,
            &consumer,
            true,
            capabilities.is_windows_11_or_greater,
            capture);
        if (capture_status != ERROR_SUCCESS) {
            LOG_ERROR(
                "Native PCL: failed to build modern ETW provider admission "
                "plan (status=%lu)",
                capture_status);
            return capture_status;
        }

        bool modern_dxgkrnl_enabled = false;
        bool legacy_dxgkrnl_skipped = false;
        bool legacy_dwm_skipped = false;
        bool unscoped_nvidia_pcl_skipped = false;
        size_t enabled_provider_count = 0;
        size_t enabled_event_count = 0;

        // EventRecordCallback was selected while display and PCL tracking were
        // enabled. Mark incoming modern-provider records as EventId-filtered
        // before enabling the first provider.
        consumer.mFilteredEvents = true;

        for (const ProviderConfiguration& captured_configuration :
             capture->Configurations()) {
            if (IsProvider(
                    captured_configuration.provider_guid,
                    Microsoft_Windows_DxgKrnl::Win7::GUID)) {
                legacy_dxgkrnl_skipped = true;
                continue;
            }
            if (IsProvider(
                    captured_configuration.provider_guid,
                    Microsoft_Windows_Dwm_Core::Win7::GUID)) {
                legacy_dwm_skipped = true;
                continue;
            }
            if (IsProvider(
                    captured_configuration.provider_guid,
                    Nvidia_PCL::GUID)) {
                unscoped_nvidia_pcl_skipped = true;
                continue;
            }

            std::vector<uint16_t> event_ids =
                captured_configuration.event_ids;
            if (IsProvider(
                    captured_configuration.provider_guid,
                    Microsoft_Windows_DxgKrnl::GUID)) {
                const uint16_t wait_packet_event_id =
                    Microsoft_Windows_DxgKrnl::QueuePacket_Start_2::Id;
                if (std::count(
                        event_ids.begin(),
                        event_ids.end(),
                        wait_packet_event_id) != 1) {
                    LOG_ERROR(
                        "Native PCL: modern DxgKrnl admission plan did not "
                        "contain one wait-packet event");
                    return ERROR_INVALID_DATA;
                }
                event_ids.erase(
                    std::remove(
                        event_ids.begin(),
                        event_ids.end(),
                        wait_packet_event_id),
                    event_ids.end());
                if (event_ids.size() !=
                    kExpectedRetainedDxgkrnlEventCount) {
                    LOG_ERROR(
                        "Native PCL: modern DxgKrnl admission plan retained "
                        "an unexpected event count (count=%lu)",
                        static_cast<unsigned long>(event_ids.size()));
                    return ERROR_INVALID_DATA;
                }
                modern_dxgkrnl_enabled = true;
            }

            const ULONG enable_status = EnableFilteredProvider(
                session,
                captured_configuration.provider_guid,
                event_ids,
                captured_configuration.any_keyword_mask,
                captured_configuration.all_keyword_mask,
                captured_configuration.maximum_level,
                captured_configuration.control_code);
            if (enable_status == ERROR_ACCESS_DENIED &&
                IsProvider(
                    captured_configuration.provider_guid,
                    Microsoft_Windows_Kernel_Process::GUID)) {
                LOG_WARN(
                    "Native PCL: kernel process provider access denied; "
                    "continuing without process lifecycle events");
                continue;
            }
            if (enable_status != ERROR_SUCCESS) {
                LOG_ERROR(
                    "Native PCL: filtered provider admission failed "
                    "(provider_data1=0x%08lX, status=%lu)",
                    captured_configuration.provider_guid.Data1,
                    enable_status);
                return enable_status;
            }

            ++enabled_provider_count;
            enabled_event_count += event_ids.size();
        }

        if (!modern_dxgkrnl_enabled ||
            !legacy_dxgkrnl_skipped ||
            (consumer.mTrackDisplay && !legacy_dwm_skipped) ||
            (consumer.mTrackPcLatency && !unscoped_nvidia_pcl_skipped)) {
            LOG_ERROR(
                "Native PCL: provider admission plan was incomplete "
                "(modern_dxgkrnl=%d, legacy_dxgkrnl_skipped=%d, "
                "legacy_dwm_skipped=%d, unscoped_pcl_skipped=%d)",
                modern_dxgkrnl_enabled ? 1 : 0,
                legacy_dxgkrnl_skipped ? 1 : 0,
                legacy_dwm_skipped ? 1 : 0,
                unscoped_nvidia_pcl_skipped ? 1 : 0);
            return ERROR_INVALID_DATA;
        }

        LOG_WARN(
            "Native PCL: modern filtered provider admission active "
            "(providers=%lu, events=%lu, legacy_dxgkrnl=excluded, "
            "legacy_dwm=excluded, unscoped_pcl=excluded)",
            static_cast<unsigned long>(enabled_provider_count),
            static_cast<unsigned long>(enabled_event_count));
        return ERROR_SUCCESS;
    }
    catch (const std::bad_alloc&) {
        LOG_ERROR(
            "Native PCL: insufficient memory while configuring filtered ETW "
            "providers");
        return ERROR_OUTOFMEMORY;
    }
    catch (...) {
        LOG_ERROR(
            "Native PCL: unexpected failure while configuring filtered ETW "
            "providers");
        return ERROR_INVALID_DATA;
    }
}

ULONG NativePclEtw_EnableFilteredProvider(
    PMTraceSession& session,
    const GUID& provider_guid,
    const std::vector<uint16_t>& event_ids,
    uint64_t any_keyword_mask,
    uint64_t all_keyword_mask,
    uint8_t maximum_level) noexcept
{
    try {
        return EnableFilteredProvider(
            session,
            provider_guid,
            event_ids,
            any_keyword_mask,
            all_keyword_mask,
            maximum_level,
            EVENT_CONTROL_CODE_ENABLE_PROVIDER);
    }
    catch (const std::bad_alloc&) {
        return ERROR_OUTOFMEMORY;
    }
    catch (...) {
        return ERROR_INVALID_DATA;
    }
}

ULONG NativePclEtw_ConfigureNvidiaPclProcessScope(
    PMTraceSession& session,
    uint32_t process_id) noexcept
{
    if (process_id == 0) {
        return ERROR_INVALID_PARAMETER;
    }

    EVENT_FILTER_DESCRIPTOR process_filter{};
    process_filter.Ptr = reinterpret_cast<ULONGLONG>(&process_id);
    process_filter.Size = static_cast<ULONG>(sizeof(process_id));
    process_filter.Type = EVENT_FILTER_TYPE_PID;

    ENABLE_TRACE_PARAMETERS enable_parameters{};
    enable_parameters.Version = ENABLE_TRACE_PARAMETERS_VERSION_2;
    enable_parameters.SourceId = session.mSessionGuid;
    enable_parameters.EnableFilterDesc = &process_filter;
    enable_parameters.FilterDescCount = 1;

    return EnableTraceEx2(
        session.mSessionHandle,
        &Nvidia_PCL::GUID,
        EVENT_CONTROL_CODE_ENABLE_PROVIDER,
        TRACE_LEVEL_VERBOSE,
        0,
        0,
        0,
        &enable_parameters);
}
