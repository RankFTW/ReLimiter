#include "live_debug.h"

#include <Windows.h>
#include <sddl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

#include "enforcement_dispatcher.h"
#include "live_debug_buffer.h"
#include "live_debug_protocol.h"
#include "logger.h"
#include "native_pcl_measurement.h"
#include "relimiter_version.h"
#include "scheduler.h"
#include "swapchain_manager.h"

namespace
{
    using namespace relimiter::live_debug;

    constexpr size_t kProducerCapacity = 1024;
    constexpr size_t kSchedulerRetention = 4096;
    constexpr size_t kNativePclRetention = 1024;
    constexpr DWORD kClientIoTimeoutMilliseconds = 500;

    ProducerQueue<FrameRow, kProducerCapacity> g_scheduler_queue;
    ProducerQueue<NativePclRow, kProducerCapacity> g_native_pcl_queue;
    RetainedRows<SchedulerRowWire, kSchedulerRetention> g_scheduler_rows;
    RetainedRows<NativePclRowWire, kNativePclRetention> g_native_pcl_rows;

    std::atomic<bool> g_enabled{ false };
    std::atomic<bool> g_stopping{ false };
    SRWLOCK g_lifecycle_lock = SRWLOCK_INIT;
    HANDLE g_stop_event = nullptr;
    HANDLE g_worker_thread = nullptr;
    HMODULE g_module_pin = nullptr;
    uint64_t g_server_started_qpc = 0;
    wchar_t g_pipe_name[128] = {};

    SchedulerRowWire ToWire(const FrameRow& source) noexcept
    {
        SchedulerRowWire row;
        row.frame_id = source.frame_id;
        row.timestamp_us = source.timestamp_us;
        row.predicted_us = source.predicted_us;
        row.effective_interval_us = source.effective_interval_us;
        row.actual_frame_time_us = source.actual_frame_time_us;
        row.sleep_duration_us = source.sleep_duration_us;
        row.wake_error_us = source.wake_error_us;
        row.ceiling_margin_us = source.ceiling_margin_us;
        row.stress_level = source.stress_level;
        row.cv = source.cv;
        row.damping_correction_us = source.damping_correction_us;
        row.tier = source.tier;
        row.overload = source.overload;
        row.fg_divisor = source.fg_divisor;
        row.mode = source.mode;
        row.scanout_error_us = source.scanout_error_us;
        row.queue_depth = source.queue_depth;
        row.api = source.api;
        row.pqi = source.pqi;
        row.cadence_score = source.cadence_score;
        row.stutter_score = source.stutter_score;
        row.deadline_score = source.deadline_score;
        row.jitter_us = source.jitter_us;
        row.own_sleep_us = source.own_sleep_us;
        row.driver_sleep_us = source.driver_sleep_us;
        row.gate_sleep_us = source.gate_sleep_us;
        row.deadline_drift_us = source.deadline_drift_us;
        row.predictor_warm = source.predictor_warm;
        row.smoothness_us = source.smoothness_us;
        row.reflex_injected = source.reflex_injected;
        row.present_interval_us = source.present_interval_us;
        row.present_cadence_smoothness_us = source.present_cadence_smoothness_us;
        row.present_bias_us = source.present_bias_us;
        row.feedback_rate = source.feedback_rate;
        row.feedback_alpha = source.feedback_alpha;
        row.reflex_present_to_gpu_end_us = source.reflex_present_to_gpu_end_us;
        row.reflex_queue_trend_us = source.reflex_queue_trend_us;
        row.reflex_present_duration_us = source.reflex_present_duration_us;
        row.reflex_gpu_active_us = source.reflex_gpu_active_us;
        row.reflex_ai_frame_time_us = source.reflex_ai_frame_time_us;
        row.reflex_cpu_latency_us = source.reflex_cpu_latency_us;
        row.gate_margin_us = source.gate_margin_us;
        row.smoothing_offset_us = source.smoothing_offset_us;
        row.p99_render_time_us = source.p99_render_time_us;
        row.total_frame_cost_us = source.total_frame_cost_us;
        return row;
    }

    NativePclRowWire ToWire(const NativePclRow& source) noexcept
    {
        NativePclRowWire row;
        row.pcl_frame_id = source.pcl_frame_id;
        row.status = static_cast<uint32_t>(source.status);
        row.present_start_qpc = source.present_start_qpc;
        row.display_qpc = source.display_qpc;
        row.sample_qpc = source.sample_qpc;
        row.sample_age_us = source.sample_age_us;
        row.input_to_frame_start_us = source.input_to_frame_start_us;
        row.frame_start_to_present_us = source.frame_start_to_present_us;
        row.present_to_display_us = source.present_to_display_us;
        row.total_us = source.total_us;
        row.dropped_accumulation_us = source.dropped_accumulation_us;
        row.dropped_frame_count = source.dropped_frame_count;
        row.lost_present_count = source.lost_present_count;
        row.etw_events_lost = source.etw_events_lost;
        row.etw_buffers_lost = source.etw_buffers_lost;
        row.overflowed_present_count = source.overflowed_present_count;
        row.qpc_frequency = source.qpc_frequency;
        row.pcl_sim_start_qpc = source.pcl_sim_start_qpc;
        row.pcl_sim_end_qpc = source.pcl_sim_end_qpc;
        row.pcl_render_submit_start_qpc = source.pcl_render_submit_start_qpc;
        row.pcl_render_submit_end_qpc = source.pcl_render_submit_end_qpc;
        row.pcl_present_start_qpc = source.pcl_present_start_qpc;
        row.pcl_present_end_qpc = source.pcl_present_end_qpc;
        row.effective_sim_start_qpc = source.effective_sim_start_qpc;
        row.previous_pcl_frame_id = source.previous_pcl_frame_id;
        row.used_simulation_fallback = source.used_simulation_fallback ? 1u : 0u;
        row.pcl_frame_id_delta = source.pcl_frame_id_delta;
        row.previous_pcl_sim_start_qpc = source.previous_pcl_sim_start_qpc;
        row.pcl_sim_start_delta_us = source.pcl_sim_start_delta_us;
        row.provider_present_to_dxgi_present_us =
            source.provider_present_to_dxgi_present_us;
        row.frame_type = source.frame_type;
        return row;
    }

    void DrainProducerQueues() noexcept
    {
        FrameRow scheduler_row{};
        while (g_scheduler_queue.Pop(&scheduler_row)) {
            g_scheduler_rows.Append(ToWire(scheduler_row));
        }

        NativePclRow native_pcl_row;
        while (g_native_pcl_queue.Pop(&native_pcl_row)) {
            g_native_pcl_rows.Append(ToWire(native_pcl_row));
        }
    }

    bool BuildPipeSecurity(SECURITY_ATTRIBUTES* attributes, PSECURITY_DESCRIPTOR* descriptor)
    {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
            return false;
        }

        DWORD token_bytes = 0;
        GetTokenInformation(token, TokenGroups, nullptr, 0, &token_bytes);
        std::vector<std::byte> token_storage(token_bytes);
        const bool queried = GetTokenInformation(
            token, TokenGroups, token_storage.data(), token_bytes, &token_bytes) != FALSE;
        CloseHandle(token);
        if (!queried) {
            return false;
        }

        const TOKEN_GROUPS* token_groups =
            reinterpret_cast<const TOKEN_GROUPS*>(token_storage.data());
        PSID logon_sid = nullptr;
        for (DWORD index = 0; index < token_groups->GroupCount; ++index) {
            if ((token_groups->Groups[index].Attributes & SE_GROUP_LOGON_ID) ==
                SE_GROUP_LOGON_ID) {
                logon_sid = token_groups->Groups[index].Sid;
                break;
            }
        }
        if (!logon_sid) {
            return false;
        }

        LPWSTR sid_text = nullptr;
        if (!ConvertSidToStringSidW(logon_sid, &sid_text)) {
            return false;
        }

        wchar_t sddl[512] = {};
        swprintf_s(
            sddl,
            L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;0x00120183;;;%ls)",
            sid_text);
        LocalFree(sid_text);

        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                sddl, SDDL_REVISION_1, descriptor, nullptr)) {
            return false;
        }

        attributes->nLength = sizeof(*attributes);
        attributes->lpSecurityDescriptor = *descriptor;
        attributes->bInheritHandle = FALSE;
        return true;
    }

    bool TransferChunkWithTimeout(
        HANDLE pipe,
        void* buffer,
        DWORD size,
        bool write_operation,
        DWORD* transferred_bytes) noexcept
    {
        if (size == 0) {
            *transferred_bytes = 0;
            return true;
        }

        OVERLAPPED overlapped{};
        overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!overlapped.hEvent) {
            return false;
        }

        DWORD transferred = 0;
        const BOOL started = write_operation
            ? WriteFile(pipe, buffer, size, &transferred, &overlapped)
            : ReadFile(pipe, buffer, size, &transferred, &overlapped);
        if (!started && GetLastError() != ERROR_IO_PENDING) {
            CloseHandle(overlapped.hEvent);
            return false;
        }

        if (!started) {
            HANDLE waits[] = { g_stop_event, overlapped.hEvent };
            const DWORD wait_result = WaitForMultipleObjects(
                2, waits, FALSE, kClientIoTimeoutMilliseconds);
            if (wait_result != WAIT_OBJECT_0 + 1 ||
                !GetOverlappedResult(pipe, &overlapped, &transferred, FALSE)) {
                CancelIoEx(pipe, &overlapped);
                GetOverlappedResult(pipe, &overlapped, &transferred, TRUE);
                CloseHandle(overlapped.hEvent);
                return false;
            }
        }

        CloseHandle(overlapped.hEvent);
        *transferred_bytes = transferred;
        return transferred != 0;
    }

    bool ReadExact(HANDLE pipe, void* buffer, DWORD size) noexcept
    {
        std::byte* cursor = static_cast<std::byte*>(buffer);
        while (size != 0) {
            DWORD transferred = 0;
            if (!TransferChunkWithTimeout(
                    pipe, cursor, size, false, &transferred)) {
                return false;
            }
            cursor += transferred;
            size -= transferred;
        }
        return true;
    }

    bool WriteExact(HANDLE pipe, const void* buffer, DWORD size) noexcept
    {
        const std::byte* cursor = static_cast<const std::byte*>(buffer);
        while (size != 0) {
            DWORD transferred = 0;
            if (!TransferChunkWithTimeout(
                    pipe, const_cast<std::byte*>(cursor), size, true, &transferred)) {
                return false;
            }
            cursor += transferred;
            size -= transferred;
        }
        return true;
    }

    template <typename Payload>
    bool WriteResponse(
        HANDLE pipe,
        const MessageHeader& request,
        const Payload& payload) noexcept
    {
        MessageHeader response;
        response.type = ResponseType(request.type);
        response.request_id = request.request_id;
        response.payload_size = sizeof(payload);
        return WriteExact(pipe, &response, sizeof(response)) &&
            WriteExact(pipe, &payload, sizeof(payload));
    }

    bool WriteError(
        HANDLE pipe,
        const MessageHeader& request,
        ResultCode code,
        const char* message) noexcept
    {
        ErrorResponse response;
        response.result.code = code;
        strncpy_s(response.message, message, _TRUNCATE);
        return WriteResponse(pipe, request, response);
    }

    StatusWire BuildStatus() noexcept
    {
        StatusWire status;
        status.process_id = GetCurrentProcessId();
        status.architecture_bits = sizeof(void*) * 8;
        status.server_started_qpc = g_server_started_qpc;
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        status.qpc_frequency = static_cast<uint64_t>(frequency.QuadPart);
        status.scheduler_first_sequence = g_scheduler_rows.FirstSequence();
        status.scheduler_next_sequence = g_scheduler_rows.NextSequence();
        status.native_pcl_first_sequence = g_native_pcl_rows.FirstSequence();
        status.native_pcl_next_sequence = g_native_pcl_rows.NextSequence();
        status.scheduler_queue_drops = g_scheduler_queue.DroppedRows();
        status.native_pcl_queue_drops = g_native_pcl_queue.DroppedRows();
        status.scheduler_retained_rows = g_scheduler_rows.Count();
        status.native_pcl_retained_rows = g_native_pcl_rows.Count();
        status.active_api = static_cast<uint32_t>(SwapMgr_GetActiveAPI());
        status.enforcement_path = static_cast<uint32_t>(EnfDisp_GetActivePath());
        status.native_pcl_status =
            static_cast<uint32_t>(NativePcl_GetSnapshot().status);
        status.target_fps = static_cast<uint32_t>(
            std::max(0, g_user_target_fps.load(std::memory_order_relaxed)));
        status.output_fps = g_output_fps.load(std::memory_order_relaxed);
        if (g_scheduler_rows.Count() != 0 && frequency.QuadPart > 0) {
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            const double now_us = static_cast<double>(now.QuadPart) * 1'000'000.0 /
                static_cast<double>(frequency.QuadPart);
            const double last_timestamp_us = g_scheduler_rows.AtSequence(
                g_scheduler_rows.NextSequence() - 1).row.timestamp_us;
            status.scheduler_last_row_age_us = std::max(
                0.0, now_us - last_timestamp_us);
        }
        strncpy_s(status.process_name, Log_GetProcessName(), _TRUNCATE);
        strncpy_s(status.addon_version, RELIMITER_VERSION_STRING, _TRUNCATE);
        return status;
    }

    template <typename Row, size_t Capacity>
    bool WriteRowsResponse(
        HANDLE pipe,
        const MessageHeader& request,
        const RowsRequest& rows_request,
        const RetainedRows<Row, Capacity>& retained_rows)
    {
        const uint32_t limit = std::clamp(
            rows_request.limit, 1u, kMaximumRowsPerRequest);
        const uint64_t first_available = retained_rows.FirstSequence();
        const uint64_t next_sequence = retained_rows.NextSequence();

        uint64_t first_returned = next_sequence;
        if (rows_request.after_sequence == 0) {
            const uint64_t retained_count = next_sequence - first_available;
            first_returned = next_sequence - std::min<uint64_t>(limit, retained_count);
        }
        else if (rows_request.after_sequence < std::numeric_limits<uint64_t>::max()) {
            first_returned = std::max(
                rows_request.after_sequence + 1, first_available);
            first_returned = std::min(first_returned, next_sequence);
        }

        const uint32_t count = static_cast<uint32_t>(std::min<uint64_t>(
            limit, next_sequence - first_returned));
        RowsResponseHeader rows_header;
        rows_header.first_available_sequence = first_available;
        rows_header.next_sequence = next_sequence;
        rows_header.first_returned_sequence = first_returned;
        rows_header.row_count = count;
        rows_header.row_size = sizeof(SequencedRow<Row>);
        rows_header.history_gap = rows_request.after_sequence != 0 &&
            rows_request.after_sequence < first_available - 1;

        const size_t payload_size = sizeof(rows_header) +
            static_cast<size_t>(count) * sizeof(SequencedRow<Row>);
        std::vector<std::byte> payload(payload_size);
        std::memcpy(payload.data(), &rows_header, sizeof(rows_header));
        std::byte* destination = payload.data() + sizeof(rows_header);
        for (uint32_t index = 0; index < count; ++index) {
            const SequencedRow<Row>& source =
                retained_rows.AtSequence(first_returned + index);
            std::memcpy(
                destination + static_cast<size_t>(index) * sizeof(source),
                &source,
                sizeof(source));
        }

        MessageHeader response;
        response.type = ResponseType(request.type);
        response.request_id = request.request_id;
        response.payload_size = static_cast<uint32_t>(payload.size());
        return WriteExact(pipe, &response, sizeof(response)) &&
            WriteExact(pipe, payload.data(), response.payload_size);
    }

    bool HandleClient(HANDLE pipe)
    {
        MessageHeader request;
        if (!ReadExact(pipe, &request, sizeof(request))) {
            return false;
        }
        if (request.magic != kProtocolMagic) {
            return WriteError(
                pipe, request, ResultCode::InvalidMessage, "invalid protocol magic");
        }
        if (request.version != kProtocolVersion) {
            return WriteError(
                pipe, request, ResultCode::UnsupportedVersion,
                "unsupported live-debug protocol version");
        }
        if (request.payload_size > kMaximumRequestPayloadBytes) {
            return WriteError(
                pipe, request, ResultCode::InvalidPayload, "request payload is too large");
        }

        std::array<std::byte, kMaximumRequestPayloadBytes> payload{};
        if (!ReadExact(pipe, payload.data(), request.payload_size)) {
            return false;
        }

        DrainProducerQueues();
        switch (request.type) {
        case MessageType::GetStatus:
            if (request.payload_size != 0) {
                return WriteError(
                    pipe, request, ResultCode::InvalidPayload,
                    "status request has a payload");
            }
            {
                StatusResponse response;
                response.status = BuildStatus();
                return WriteResponse(pipe, request, response);
            }

        case MessageType::GetSchedulerRows:
        case MessageType::GetNativePclRows:
            if (request.payload_size != sizeof(RowsRequest)) {
                return WriteError(
                    pipe, request, ResultCode::InvalidPayload, "invalid rows request");
            }
            {
                RowsRequest rows_request;
                std::memcpy(&rows_request, payload.data(), sizeof(rows_request));
                if (request.type == MessageType::GetSchedulerRows) {
                    return WriteRowsResponse(
                        pipe, request, rows_request, g_scheduler_rows);
                }
                return WriteRowsResponse(
                    pipe, request, rows_request, g_native_pcl_rows);
            }

        default:
            return WriteError(
                pipe, request, ResultCode::UnsupportedRequest,
                "unsupported read-only request");
        }
    }

    void WaitForClientToDrainResponse(HANDLE pipe) noexcept
    {
        // DisconnectNamedPipe discards response bytes the client has not read yet.
        // The bridge closes after consuming one response, so a bounded read waits
        // for that close without the unbounded wait imposed by FlushFileBuffers.
        std::byte unexpected_byte{};
        ReadExact(pipe, &unexpected_byte, sizeof(unexpected_byte));
    }

    DWORD WINAPI WorkerThread(LPVOID)
    {
        SECURITY_ATTRIBUTES security_attributes{};
        PSECURITY_DESCRIPTOR security_descriptor = nullptr;
        if (!BuildPipeSecurity(&security_attributes, &security_descriptor)) {
            LOG_ERROR("LiveDebug: failed to construct same-user pipe security");
            g_enabled.store(false, std::memory_order_release);
            return 1;
        }

        LOG_INFO("LiveDebug: read-only pipe ready at %ls", g_pipe_name);
        while (WaitForSingleObject(g_stop_event, 0) != WAIT_OBJECT_0) {
            HANDLE pipe = CreateNamedPipeW(
                g_pipe_name,
                PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                1,
                kMaximumResponsePayloadBytes + sizeof(MessageHeader),
                sizeof(MessageHeader) + kMaximumRequestPayloadBytes,
                0,
                &security_attributes);
            if (pipe == INVALID_HANDLE_VALUE) {
                LOG_ERROR("LiveDebug: CreateNamedPipe failed (%lu)", GetLastError());
                break;
            }

            OVERLAPPED connection{};
            connection.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!connection.hEvent) {
                CloseHandle(pipe);
                break;
            }

            const BOOL connected_immediately = ConnectNamedPipe(pipe, &connection);
            const DWORD connect_error = connected_immediately ? ERROR_SUCCESS : GetLastError();
            bool connected = connected_immediately != FALSE ||
                connect_error == ERROR_PIPE_CONNECTED;
            const bool connection_pending = !connected && connect_error == ERROR_IO_PENDING;
            if (!connected && !connection_pending) {
                LOG_WARN("LiveDebug: ConnectNamedPipe failed (%lu)", connect_error);
                CloseHandle(connection.hEvent);
                CloseHandle(pipe);
                continue;
            }

            while (connection_pending && !connected) {
                HANDLE waits[] = { g_stop_event, connection.hEvent };
                const DWORD wait_result = WaitForMultipleObjects(2, waits, FALSE, 10);
                if (wait_result == WAIT_OBJECT_0) {
                    CancelIoEx(pipe, &connection);
                    DWORD ignored = 0;
                    GetOverlappedResult(pipe, &connection, &ignored, TRUE);
                    break;
                }
                if (wait_result == WAIT_OBJECT_0 + 1) {
                    DWORD ignored = 0;
                    connected = GetOverlappedResult(pipe, &connection, &ignored, FALSE) != FALSE ||
                        GetLastError() == ERROR_PIPE_CONNECTED;
                    break;
                }
                DrainProducerQueues();
            }

            if (connected) {
                if (HandleClient(pipe)) {
                    WaitForClientToDrainResponse(pipe);
                }
                DisconnectNamedPipe(pipe);
            }

            CloseHandle(connection.hEvent);
            CloseHandle(pipe);
            DrainProducerQueues();
        }

        LocalFree(security_descriptor);
        g_enabled.store(false, std::memory_order_release);
        LOG_INFO("LiveDebug: worker stopped");
        return 0;
    }
}

void LiveDebug_StartIfReady() noexcept
{
    if (g_enabled.load(std::memory_order_acquire) ||
        g_stopping.load(std::memory_order_acquire)) {
        return;
    }

    AcquireSRWLockExclusive(&g_lifecycle_lock);
    if (g_worker_thread &&
        WaitForSingleObject(g_worker_thread, 0) == WAIT_OBJECT_0) {
        CloseHandle(g_worker_thread);
        if (g_stop_event) CloseHandle(g_stop_event);
        g_worker_thread = nullptr;
        g_stop_event = nullptr;
    }
    if (!g_worker_thread && !g_stopping.load(std::memory_order_relaxed)) {
        if (!g_module_pin && !GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                reinterpret_cast<LPCWSTR>(
                    reinterpret_cast<uintptr_t>(&LiveDebug_StartIfReady)),
                &g_module_pin)) {
            LOG_ERROR("LiveDebug: failed to pin addon module (%lu)", GetLastError());
            ReleaseSRWLockExclusive(&g_lifecycle_lock);
            return;
        }
        g_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        LARGE_INTEGER started{};
        QueryPerformanceCounter(&started);
        g_server_started_qpc = static_cast<uint64_t>(started.QuadPart);
        swprintf_s(
            g_pipe_name,
            L"%s%lu.v%u",
            kPipePrefix,
            GetCurrentProcessId(),
            kProtocolVersion);
        if (g_stop_event) {
            g_enabled.store(true, std::memory_order_release);
            g_worker_thread = CreateThread(nullptr, 0, WorkerThread, nullptr, 0, nullptr);
        }
        if (!g_worker_thread) {
            g_enabled.store(false, std::memory_order_release);
            if (g_stop_event) {
                CloseHandle(g_stop_event);
                g_stop_event = nullptr;
            }
            LOG_ERROR("LiveDebug: failed to start worker (%lu)", GetLastError());
        }
    }
    ReleaseSRWLockExclusive(&g_lifecycle_lock);
}

void LiveDebug_Stop() noexcept
{
    g_stopping.store(true, std::memory_order_release);
    g_enabled.store(false, std::memory_order_release);
    AcquireSRWLockExclusive(&g_lifecycle_lock);
    if (g_worker_thread) {
        SetEvent(g_stop_event);
        WaitForSingleObject(g_worker_thread, INFINITE);
        CloseHandle(g_worker_thread);
        CloseHandle(g_stop_event);
        g_worker_thread = nullptr;
        g_stop_event = nullptr;
    }
    if (g_module_pin) {
        HMODULE module_pin = g_module_pin;
        g_module_pin = nullptr;
        FreeLibrary(module_pin);
    }
    ReleaseSRWLockExclusive(&g_lifecycle_lock);
}

void LiveDebug_NotifyProcessDetach() noexcept
{
    g_stopping.store(true, std::memory_order_release);
    g_enabled.store(false, std::memory_order_release);
    HANDLE stop_event = g_stop_event;
    if (stop_event) {
        SetEvent(stop_event);
    }
}

void LiveDebug_PushScheduler(const FrameRow& row) noexcept
{
    if (g_enabled.load(std::memory_order_relaxed)) {
        g_scheduler_queue.Push(row);
    }
}

void LiveDebug_PushNativePcl(const NativePclRow& row) noexcept
{
    if (g_enabled.load(std::memory_order_relaxed)) {
        g_native_pcl_queue.Push(row);
    }
}