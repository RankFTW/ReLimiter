#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace relimiter::live_debug
{
    constexpr uint32_t kProtocolMagic = 0x524C4442; // "RLDB"
    constexpr uint16_t kProtocolVersion = 1;
    constexpr uint32_t kMaximumRequestPayloadBytes = 64;
    constexpr uint32_t kMaximumResponsePayloadBytes = 512 * 1024;
    constexpr uint32_t kMaximumRowsPerRequest = 512;
    constexpr wchar_t kPipePrefix[] = L"\\\\.\\pipe\\ReLimiter.LiveDebug.";

    enum class MessageType : uint16_t
    {
        GetStatus = 1,
        GetSchedulerRows = 2,
        GetNativePclRows = 3,
        ResponseFlag = 0x8000,
    };

    enum class ResultCode : uint32_t
    {
        Success = 0,
        InvalidMessage = 1,
        UnsupportedVersion = 2,
        UnsupportedRequest = 3,
        InvalidPayload = 4,
        InternalError = 5,
    };

    constexpr MessageType ResponseType(MessageType request_type) noexcept
    {
        return static_cast<MessageType>(
            static_cast<uint16_t>(request_type) |
            static_cast<uint16_t>(MessageType::ResponseFlag));
    }

#pragma pack(push, 8)
    struct MessageHeader
    {
        uint32_t magic = kProtocolMagic;
        uint16_t version = kProtocolVersion;
        MessageType type = MessageType::GetStatus;
        uint32_t request_id = 0;
        uint32_t payload_size = 0;
        uint32_t reserved = 0;
    };

    struct ResultPrefix
    {
        ResultCode code = ResultCode::Success;
        uint32_t reserved = 0;
    };

    struct ErrorResponse
    {
        ResultPrefix result;
        char message[192] = {};
    };

    struct RowsRequest
    {
        uint64_t after_sequence = 0;
        uint32_t limit = 120;
        uint32_t reserved = 0;
    };

    struct StatusWire
    {
        uint32_t process_id = 0;
        uint32_t architecture_bits = 0;
        uint64_t server_started_qpc = 0;
        uint64_t qpc_frequency = 0;
        uint64_t scheduler_first_sequence = 0;
        uint64_t scheduler_next_sequence = 0;
        uint64_t native_pcl_first_sequence = 0;
        uint64_t native_pcl_next_sequence = 0;
        uint64_t scheduler_queue_drops = 0;
        uint64_t native_pcl_queue_drops = 0;
        uint32_t scheduler_retained_rows = 0;
        uint32_t native_pcl_retained_rows = 0;
        uint32_t active_api = 0;
        uint32_t enforcement_path = 0;
        uint32_t native_pcl_status = 0;
        uint32_t target_fps = 0;
        double output_fps = 0.0;
        double scheduler_last_row_age_us = 0.0;
        char process_name[128] = {};
        char addon_version[24] = {};
    };

    struct StatusResponse
    {
        ResultPrefix result;
        StatusWire status;
    };

    struct RowsResponseHeader
    {
        ResultPrefix result;
        uint64_t first_available_sequence = 0;
        uint64_t next_sequence = 0;
        uint64_t first_returned_sequence = 0;
        uint32_t row_count = 0;
        uint32_t row_size = 0;
        uint32_t history_gap = 0;
        uint32_t reserved = 0;
    };

    struct SchedulerRowWire
    {
        uint64_t frame_id = 0;
        double timestamp_us = 0.0;
        double predicted_us = 0.0;
        double effective_interval_us = 0.0;
        double actual_frame_time_us = 0.0;
        double sleep_duration_us = 0.0;
        double wake_error_us = 0.0;
        double ceiling_margin_us = 0.0;
        double stress_level = 0.0;
        double cv = 0.0;
        double damping_correction_us = 0.0;
        int32_t tier = 0;
        int32_t overload = 0;
        double fg_divisor = 0.0;
        int32_t mode = 0;
        int32_t queue_depth = 0;
        double scanout_error_us = 0.0;
        int32_t api = 0;
        int32_t predictor_warm = 0;
        double pqi = 0.0;
        double cadence_score = 0.0;
        double stutter_score = 0.0;
        double deadline_score = 0.0;
        double jitter_us = 0.0;
        double own_sleep_us = 0.0;
        double driver_sleep_us = 0.0;
        double gate_sleep_us = 0.0;
        double deadline_drift_us = 0.0;
        double smoothness_us = 0.0;
        int32_t reflex_injected = 0;
        int32_t feedback_rate = 0;
        double present_interval_us = 0.0;
        double present_cadence_smoothness_us = 0.0;
        double present_bias_us = 0.0;
        double feedback_alpha = 0.0;
        double reflex_present_to_gpu_end_us = 0.0;
        double reflex_queue_trend_us = 0.0;
        double reflex_present_duration_us = 0.0;
        double reflex_gpu_active_us = 0.0;
        double reflex_ai_frame_time_us = 0.0;
        double reflex_cpu_latency_us = 0.0;
        double gate_margin_us = 0.0;
        double smoothing_offset_us = 0.0;
        double p99_render_time_us = 0.0;
        double total_frame_cost_us = 0.0;
    };

    struct NativePclRowWire
    {
        uint32_t pcl_frame_id = 0;
        uint32_t status = 0;
        uint64_t present_start_qpc = 0;
        uint64_t display_qpc = 0;
        uint64_t sample_qpc = 0;
        double sample_age_us = 0.0;
        double input_to_frame_start_us = 0.0;
        double frame_start_to_present_us = 0.0;
        double present_to_display_us = 0.0;
        double total_us = 0.0;
        double dropped_accumulation_us = 0.0;
        uint32_t dropped_frame_count = 0;
        uint32_t lost_present_count = 0;
        uint32_t etw_events_lost = 0;
        uint32_t etw_buffers_lost = 0;
        uint32_t overflowed_present_count = 0;
        uint64_t qpc_frequency = 0;
        uint64_t pcl_sim_start_qpc = 0;
        uint64_t pcl_sim_end_qpc = 0;
        uint64_t pcl_render_submit_start_qpc = 0;
        uint64_t pcl_render_submit_end_qpc = 0;
        uint64_t pcl_present_start_qpc = 0;
        uint64_t pcl_present_end_qpc = 0;
        uint64_t effective_sim_start_qpc = 0;
        uint32_t previous_pcl_frame_id = 0;
        uint32_t used_simulation_fallback = 0;
        int64_t pcl_frame_id_delta = 0;
        uint64_t previous_pcl_sim_start_qpc = 0;
        double pcl_sim_start_delta_us = 0.0;
        double provider_present_to_dxgi_present_us = 0.0;
        int32_t frame_type = 0;
        uint32_t reserved = 0;
    };

    template <typename Row>
    struct SequencedRow
    {
        uint64_t sequence = 0;
        Row row;
    };
#pragma pack(pop)

    static_assert(sizeof(MessageHeader) == 20);
    static_assert(sizeof(RowsRequest) == 16);
    static_assert(sizeof(StatusWire) == 264);
    static_assert(sizeof(RowsResponseHeader) == 48);
    static_assert(sizeof(SchedulerRowWire) == 328);
    static_assert(sizeof(NativePclRowWire) == 216);
    static_assert(sizeof(SequencedRow<SchedulerRowWire>) == 336);
    static_assert(sizeof(SequencedRow<NativePclRowWire>) == 224);
    static_assert(std::is_trivially_copyable_v<MessageHeader>);
    static_assert(std::is_trivially_copyable_v<StatusWire>);
    static_assert(std::is_trivially_copyable_v<SchedulerRowWire>);
    static_assert(std::is_trivially_copyable_v<NativePclRowWire>);
}