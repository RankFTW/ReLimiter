#include "native_pcl_etw_cleanup.h"

#include <Windows.h>
#include <evntrace.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <vector>

#include "logger.h"

namespace
{
    constexpr wchar_t kSessionNamePrefix[] = L"ReLimiter.NativePCL.";
    constexpr size_t kSessionNamePrefixLength =
        sizeof(kSessionNamePrefix) / sizeof(kSessionNamePrefix[0]) - 1;
    constexpr size_t kMaximumSessionNameCharacters = 1024;
    constexpr size_t kMaximumLogFilePathCharacters = 1024;
    constexpr ULONG kInitialTraceSessionCapacity = 64;
    constexpr ULONG kMaximumTraceSessionCapacity = 1024;
    constexpr size_t kTracePropertiesSize =
        sizeof(EVENT_TRACE_PROPERTIES) +
        kMaximumSessionNameCharacters * sizeof(wchar_t) +
        kMaximumLogFilePathCharacters * sizeof(wchar_t);

    struct TraceControlProperties : EVENT_TRACE_PROPERTIES
    {
        wchar_t session_name[kMaximumSessionNameCharacters];
        wchar_t log_file_path[kMaximumLogFilePathCharacters];
    };

    enum class ProcessState
    {
        Running,
        Exited,
        Unknown,
    };

    struct ProcessProbe
    {
        ProcessState state = ProcessState::Unknown;
        DWORD error = ERROR_SUCCESS;
    };

    bool TryParseDecimal(
        std::wstring_view text,
        uint64_t maximum_value,
        uint64_t& value) noexcept
    {
        if (text.empty()) {
            return false;
        }

        uint64_t parsed_value = 0;
        for (const wchar_t character : text) {
            if (character < L'0' || character > L'9') {
                return false;
            }

            const uint64_t digit = static_cast<uint64_t>(character - L'0');
            if (parsed_value > (maximum_value - digit) / 10) {
                return false;
            }
            parsed_value = parsed_value * 10 + digit;
        }

        value = parsed_value;
        return true;
    }

    bool TryParseSessionOwner(
        std::wstring_view session_name,
        DWORD& process_id) noexcept
    {
        if (session_name.size() <= kSessionNamePrefixLength ||
            session_name.compare(
                0,
                kSessionNamePrefixLength,
                kSessionNamePrefix,
                kSessionNamePrefixLength) != 0) {
            return false;
        }

        const std::wstring_view suffix = session_name.substr(
            kSessionNamePrefixLength);
        const size_t process_id_end = suffix.find(L'.');
        if (process_id_end == std::wstring_view::npos) {
            return false;
        }

        const size_t tick_count_end = suffix.find(L'.', process_id_end + 1);
        if (tick_count_end == std::wstring_view::npos ||
            suffix.find(L'.', tick_count_end + 1) != std::wstring_view::npos) {
            return false;
        }

        uint64_t parsed_process_id = 0;
        uint64_t parsed_tick_count = 0;
        uint64_t parsed_nonce = 0;
        if (!TryParseDecimal(
                suffix.substr(0, process_id_end),
                std::numeric_limits<DWORD>::max(),
                parsed_process_id) ||
            !TryParseDecimal(
                suffix.substr(
                    process_id_end + 1,
                    tick_count_end - process_id_end - 1),
                std::numeric_limits<uint64_t>::max(),
                parsed_tick_count) ||
            !TryParseDecimal(
                suffix.substr(tick_count_end + 1),
                std::numeric_limits<uint32_t>::max(),
                parsed_nonce) ||
            parsed_process_id == 0 || parsed_nonce == 0) {
            return false;
        }

        process_id = static_cast<DWORD>(parsed_process_id);
        return true;
    }

    ProcessProbe ProbeProcess(DWORD process_id) noexcept
    {
        const HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, process_id);
        if (process == nullptr) {
            const DWORD error = GetLastError();
            return {
                error == ERROR_INVALID_PARAMETER
                    ? ProcessState::Exited
                    : ProcessState::Unknown,
                error,
            };
        }

        const DWORD wait_status = WaitForSingleObject(process, 0);
        const DWORD wait_error = wait_status == WAIT_FAILED
            ? GetLastError()
            : ERROR_SUCCESS;
        CloseHandle(process);

        if (wait_status == WAIT_OBJECT_0) {
            return { ProcessState::Exited, ERROR_SUCCESS };
        }
        if (wait_status == WAIT_TIMEOUT) {
            return { ProcessState::Running, ERROR_SUCCESS };
        }
        return { ProcessState::Unknown, wait_error };
    }

    void PrepareTraceProperties(
        ULONG capacity,
        std::vector<unsigned char>& storage,
        std::vector<EVENT_TRACE_PROPERTIES*>& properties)
    {
        storage.assign(kTracePropertiesSize * capacity, 0);
        properties.resize(capacity);

        for (ULONG index = 0; index < capacity; ++index) {
            auto* trace_properties = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(
                storage.data() + kTracePropertiesSize * index);
            trace_properties->Wnode.BufferSize =
                static_cast<ULONG>(kTracePropertiesSize);
            trace_properties->LoggerNameOffset =
                sizeof(EVENT_TRACE_PROPERTIES);
            trace_properties->LogFileNameOffset =
                sizeof(EVENT_TRACE_PROPERTIES) +
                static_cast<ULONG>(
                    kMaximumSessionNameCharacters * sizeof(wchar_t));
            properties[index] = trace_properties;
        }
    }

    ULONG QueryTraceSessions(
        std::vector<unsigned char>& storage,
        std::vector<EVENT_TRACE_PROPERTIES*>& properties,
        ULONG& session_count)
    {
        ULONG capacity = kInitialTraceSessionCapacity;
        for (;;) {
            PrepareTraceProperties(capacity, storage, properties);
            session_count = capacity;
            const ULONG status = QueryAllTracesW(
                properties.data(), capacity, &session_count);
            if (status != ERROR_MORE_DATA) {
                return status;
            }

            const ULONG expanded_capacity = std::max(
                session_count,
                capacity <= kMaximumTraceSessionCapacity / 2
                    ? capacity * 2
                    : kMaximumTraceSessionCapacity);
            if (expanded_capacity <= capacity ||
                expanded_capacity > kMaximumTraceSessionCapacity) {
                return ERROR_MORE_DATA;
            }
            capacity = expanded_capacity;
        }
    }

    std::wstring_view GetSessionName(
        const EVENT_TRACE_PROPERTIES& properties) noexcept
    {
        if (properties.LoggerNameOffset < sizeof(EVENT_TRACE_PROPERTIES) ||
            properties.LoggerNameOffset >= properties.Wnode.BufferSize) {
            return {};
        }

        const auto* session_name = reinterpret_cast<const wchar_t*>(
            reinterpret_cast<const unsigned char*>(&properties) +
            properties.LoggerNameOffset);
        const size_t available_character_count = std::min(
            kMaximumSessionNameCharacters,
            static_cast<size_t>(
                properties.Wnode.BufferSize - properties.LoggerNameOffset) /
                sizeof(wchar_t));
        size_t length = 0;
        while (length < available_character_count &&
               session_name[length] != L'\0') {
            ++length;
        }
        if (length == available_character_count) {
            return {};
        }
        return { session_name, length };
    }

    ULONG StopTraceSession(const wchar_t* session_name) noexcept
    {
        TraceControlProperties properties{};
        properties.Wnode.BufferSize = sizeof(properties);
        properties.LoggerNameOffset =
            offsetof(TraceControlProperties, session_name);
        properties.LogFileNameOffset =
            offsetof(TraceControlProperties, log_file_path);
        return ControlTraceW(
            0,
            session_name,
            &properties,
            EVENT_TRACE_CONTROL_STOP);
    }
}

void NativePclEtw_ReclaimStaleSessions() noexcept
{
    try {
        std::vector<unsigned char> trace_storage;
        std::vector<EVENT_TRACE_PROPERTIES*> trace_properties;
        ULONG session_count = 0;
        const ULONG query_status = QueryTraceSessions(
            trace_storage, trace_properties, session_count);
        if (query_status != ERROR_SUCCESS) {
            LOG_WARN(
                "Native PCL: stale ETW session scan failed "
                "(status=%lu, isolation_remains_enabled=1)",
                query_status);
            return;
        }

        ULONG matching_session_count = 0;
        ULONG reclaimed_session_count = 0;
        ULONG live_owner_count = 0;
        ULONG unverified_owner_count = 0;
        ULONG reclaim_failure_count = 0;

        for (ULONG index = 0; index < session_count; ++index) {
            const std::wstring_view session_name_view = GetSessionName(
                *trace_properties[index]);
            DWORD process_id = 0;
            if (!TryParseSessionOwner(session_name_view, process_id)) {
                continue;
            }
            ++matching_session_count;

            const ProcessProbe process = ProbeProcess(process_id);
            if (process.state == ProcessState::Running) {
                ++live_owner_count;
                continue;
            }
            if (process.state == ProcessState::Unknown) {
                ++unverified_owner_count;
                LOG_WARN(
                    "Native PCL: preserved ETW session with unverified owner "
                    "(session=%.*ls, owner_pid=%lu, error=%lu)",
                    static_cast<int>(session_name_view.size()),
                    session_name_view.data(),
                    process_id,
                    process.error);
                continue;
            }

            const std::wstring session_name(session_name_view);
            const ULONG stop_status = StopTraceSession(session_name.c_str());
            if (stop_status == ERROR_SUCCESS ||
                stop_status == ERROR_WMI_INSTANCE_NOT_FOUND) {
                ++reclaimed_session_count;
                LOG_WARN(
                    "Native PCL: reclaimed stale ETW session "
                    "(session=%ls, owner_pid=%lu, status=%lu)",
                    session_name.c_str(),
                    process_id,
                    stop_status);
            }
            else {
                ++reclaim_failure_count;
                LOG_WARN(
                    "Native PCL: failed to reclaim stale ETW session "
                    "(session=%ls, owner_pid=%lu, status=%lu, "
                    "isolation_remains_enabled=1)",
                    session_name.c_str(),
                    process_id,
                    stop_status);
            }
        }

        LOG_INFO(
            "Native PCL: stale ETW session scan complete "
            "(visible=%lu, matching=%lu, reclaimed=%lu, live=%lu, "
            "unverified=%lu, failed=%lu)",
            session_count,
            matching_session_count,
            reclaimed_session_count,
            live_owner_count,
            unverified_owner_count,
            reclaim_failure_count);
    }
    catch (const std::bad_alloc&) {
        LOG_WARN(
            "Native PCL: insufficient memory while scanning stale ETW "
            "sessions (isolation_remains_enabled=1)");
    }
    catch (...) {
        LOG_WARN(
            "Native PCL: unexpected stale ETW session cleanup failure "
            "(isolation_remains_enabled=1)");
    }
}
