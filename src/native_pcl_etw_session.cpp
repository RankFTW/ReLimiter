#include "native_pcl_etw_session.h"

#include <evntrace.h>

#include <atomic>
#include <cwchar>

#include "Debug.hpp"
#include "ETW/Intel_PresentMon.h"
#include "ETW/Microsoft_Windows_D3D9.h"
#include "ETW/Microsoft_Windows_Dwm_Core.h"
#include "ETW/Microsoft_Windows_DXGI.h"
#include "ETW/Microsoft_Windows_DxgKrnl.h"
#include "ETW/Microsoft_Windows_EventMetadata.h"
#include "ETW/Microsoft_Windows_Kernel_Process.h"
#include "ETW/Microsoft_Windows_Win32k.h"
#include "ETW/NV_DD.h"
#include "ETW/Nvidia_PCL.h"
#include "PresentMonTraceConsumer.hpp"
#include "PresentMonTraceSession.hpp"
#include "native_pcl_measurement_internal.h"
#include "logger.h"

namespace
{
    constexpr ULONG kBufferSizeKilobytes = 128;
    constexpr ULONG kMinimumBufferCount = 256;
    constexpr ULONG kMaximumBufferCount = 512;

    struct NativePclTraceProperties : EVENT_TRACE_PROPERTIES
    {
        wchar_t session_name[MAX_PATH];
    };

    bool IsProvider(const GUID& candidate, const GUID& expected) noexcept
    {
        return IsEqualGUID(candidate, expected) != FALSE;
    }

    std::atomic<bool> s_callback_poisoned{false};

    bool DispatchOpaquePresentMonHandlerCpp(
        PMTraceConsumer* consumer,
        EVENT_RECORD* event_record)
    {
        const GUID& provider_guid = event_record->EventHeader.ProviderId;
        if (IsProvider(provider_guid, Microsoft_Windows_DxgKrnl::GUID))
            consumer->HandleDXGKEvent(event_record);
        else if (IsProvider(provider_guid, Microsoft_Windows_DXGI::GUID))
            consumer->HandleDXGIEvent(event_record);
        else if (IsProvider(provider_guid, Microsoft_Windows_Win32k::GUID))
            consumer->HandleWin32kEvent(event_record);
        else if (IsProvider(provider_guid, Microsoft_Windows_Dwm_Core::GUID))
            consumer->HandleDWMEvent(event_record);
        else if (IsProvider(provider_guid, Microsoft_Windows_D3D9::GUID))
            consumer->HandleD3D9Event(event_record);
        else if (IsProvider(provider_guid, Microsoft_Windows_Kernel_Process::GUID))
            consumer->HandleProcessEvent(event_record);
        else if (IsProvider(provider_guid, Microsoft_Windows_EventMetadata::GUID))
            consumer->HandleMetadataEvent(event_record);
        else if (IsProvider(provider_guid, NvidiaDisplayDriver_Events::GUID))
            consumer->HandleNvidiaDisplayDriverEvent(event_record);
        else if (IsProvider(provider_guid, Intel_PresentMon::GUID))
            consumer->HandleIntelPresentMonEvent(event_record);
        else if (IsProvider(provider_guid, Nvidia_PCL::GUID))
            consumer->HandlePclEvent(event_record);
        return true;
    }

    bool DispatchOpaquePresentMonHandlerCppNoexcept(
        PMTraceConsumer* consumer,
        EVENT_RECORD* event_record) noexcept
    {
        try {
            return DispatchOpaquePresentMonHandlerCpp(consumer, event_record);
        }
        catch (...) {
            return false;
        }
    }

    bool DispatchOpaquePresentMonHandler(
        PMTraceConsumer* consumer,
        EVENT_RECORD* event_record) noexcept
    {
        __try {
            return DispatchOpaquePresentMonHandlerCppNoexcept(
                consumer, event_record);
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    bool DispatchNativePclEventRecord(EVENT_RECORD* event_record)
    {
        if (!event_record || !event_record->UserContext) return true;
        auto* session = static_cast<PMTraceSession*>(event_record->UserContext);
        PMTraceConsumer* consumer = session->mPMConsumer;
        if (!consumer) return true;

        PMTraceConsumer::EventProcessingScope processing_scope(*consumer);
        if (!processing_scope) return true;
        // Opaque SEH is converted to a normal false return while the scope is
        // still alive, so its destructor always runs on this ordinary path.
        return DispatchOpaquePresentMonHandler(consumer, event_record);
    }

    bool DispatchNativePclEventRecordNoexcept(EVENT_RECORD* event_record) noexcept
    {
        try {
            return DispatchNativePclEventRecord(event_record);
        }
        catch (...) {
            return false;
        }
    }

    void PoisonNativePclCallback() noexcept
    {
        if (!s_callback_poisoned.exchange(true, std::memory_order_acq_rel)) {
            OutputDebugStringA(
                "ReLimiter: native PCL callback poisoned; stopping ETW session.\n");
            NativePclInternal_RequestStopFromCallback();
        }
    }

    void CALLBACK NativePclEventRecordCallback(EVENT_RECORD* event_record)
    {
        if (s_callback_poisoned.load(std::memory_order_acquire)) return;
        bool processed = false;
        __try {
            processed = DispatchNativePclEventRecordNoexcept(event_record);
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {
            processed = false;
        }
        if (!processed) PoisonNativePclCallback();
    }

    void InitializeRealtimeTimestampState(
        PMTraceSession& session,
        const EVENT_TRACE_LOGFILEW& trace_properties) noexcept
    {
        session.mTimestampType = static_cast<PMTraceSession::TimestampType>(
            trace_properties.LogfileHeader.ReservedFlags);
        switch (session.mTimestampType) {
        case PMTraceSession::TIMESTAMP_TYPE_SYSTEM_TIME:
            session.mTimestampFrequency.QuadPart = 10'000'000ull;
            break;
        case PMTraceSession::TIMESTAMP_TYPE_CPU_CYCLE_COUNTER:
            session.mTimestampFrequency.QuadPart =
                1'000'000ull * trace_properties.LogfileHeader.CpuSpeedInMHz;
            break;
        case PMTraceSession::TIMESTAMP_TYPE_QPC:
        default:
            session.mTimestampFrequency =
                trace_properties.LogfileHeader.PerfFreq;
            break;
        }

        if (session.mTimestampFrequency.QuadPart == 0) {
            session.mTimestampFrequency.QuadPart = 10'000'000ull;
        }

        LARGE_INTEGER first_qpc{};
        LARGE_INTEGER second_qpc{};
        FILETIME system_file_time{};
        QueryPerformanceCounter(&first_qpc);
        GetSystemTimeAsFileTime(&system_file_time);
        QueryPerformanceCounter(&second_qpc);
        FileTimeToLocalFileTime(
            &system_file_time,
            reinterpret_cast<FILETIME*>(&session.mStartFileTime));
        session.mStartTimestamp.QuadPart =
            (first_qpc.QuadPart + second_qpc.QuadPart) / 2;

        InitializeTimestampInfo(
            &session.mStartTimestamp,
            session.mTimestampFrequency);
    }
}

ULONG NativePclEtw_StartSession(
    PMTraceSession& session,
    const wchar_t* session_name) noexcept
{
    s_callback_poisoned.store(false, std::memory_order_release);
    if (session.mPMConsumer == nullptr || session_name == nullptr ||
        session_name[0] == L'\0' ||
        std::wcslen(session_name) >= MAX_PATH ||
        session.mSessionHandle != 0 ||
        session.mTraceHandle != INVALID_PROCESSTRACE_HANDLE) {
        return ERROR_INVALID_PARAMETER;
    }

    session.mStartTimestamp.QuadPart = 0;
    session.mContinueProcessingBuffers = TRUE;
    session.mIsRealtimeSession = true;
    session.mNumEventsLost = 0;
    session.mNumBuffersLost = 0;
    session.mCachedEtwStatus = {};
    session.ResetEtwEventLatencyStats();
    session.mPMConsumer->mIsRealtimeSession = true;

    NativePclTraceProperties session_properties{};
    session_properties.Wnode.BufferSize = sizeof(session_properties);
    session_properties.Wnode.ClientContext = session.mTimestampType;
    session_properties.Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    session_properties.LogFileMode =
        EVENT_TRACE_REAL_TIME_MODE |
        EVENT_TRACE_INDEPENDENT_SESSION_MODE;
    session_properties.LoggerNameOffset =
        offsetof(NativePclTraceProperties, session_name);
    session_properties.BufferSize = kBufferSizeKilobytes;
    session_properties.MinimumBuffers = kMinimumBufferCount;
    session_properties.MaximumBuffers = kMaximumBufferCount;

    ULONG status = StartTraceW(
        &session.mSessionHandle,
        session_name,
        &session_properties);
    if (status != ERROR_SUCCESS) {
        session.mSessionHandle = 0;
        return status;
    }
    session.mSessionGuid = session_properties.Wnode.Guid;

    EVENT_TRACE_LOGFILEW trace_properties{};
    trace_properties.LoggerName = const_cast<wchar_t*>(session_name);
    trace_properties.ProcessTraceMode =
        PROCESS_TRACE_MODE_EVENT_RECORD |
        PROCESS_TRACE_MODE_RAW_TIMESTAMP |
        PROCESS_TRACE_MODE_REAL_TIME;
    trace_properties.Context = &session;
    trace_properties.EventRecordCallback = NativePclEventRecordCallback;

    session.mTraceHandle = OpenTraceW(&trace_properties);
    if (session.mTraceHandle == INVALID_PROCESSTRACE_HANDLE) {
        status = GetLastError();
        session.Stop();
        return status;
    }

    InitializeRealtimeTimestampState(session, trace_properties);
    LOG_INFO(
        "Native PCL: source-owned ETW session active "
        "(buffer_kb=%lu, minimum_buffers=%lu, maximum_buffers=%lu, "
        "initial_pool_mib=%lu, maximum_pool_mib=%lu, "
        "callback=modern_only, independent_session=enabled)",
        kBufferSizeKilobytes,
        kMinimumBufferCount,
        kMaximumBufferCount,
        kBufferSizeKilobytes * kMinimumBufferCount / 1024,
        kBufferSizeKilobytes * kMaximumBufferCount / 1024);
    return ERROR_SUCCESS;
}
