#include "csv_writer.h"
#include "adaptive_deadline_reserve.h"
#include "frame_timeline.h"
#include "logger.h"
#include "present_association.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>

// ── Scheduler-frame SPSC ring buffer ──
static constexpr int RING_SIZE = 4096; // power of 2
static constexpr int RING_MASK = RING_SIZE - 1;

template <typename Row>
struct CapturedRow {
    uint32_t capture_epoch = 0;
    Row row{};
};

static CapturedRow<FrameRow> s_ring[RING_SIZE];
static std::atomic<uint64_t> s_write_pos{0};
static std::atomic<uint64_t> s_read_pos{0};
static std::atomic<uint64_t> s_dropped_frame_rows{0};

// ── Native PCL displayed-frame SPSC ring buffer ──
static CapturedRow<NativePclRow> s_native_pcl_ring[RING_SIZE];
static std::atomic<uint64_t> s_native_pcl_write_pos{0};
static std::atomic<uint64_t> s_native_pcl_read_pos{0};
static std::atomic<uint64_t> s_native_pcl_dropped_rows{0};

// ── PresentMon Present-observation SPSC ring buffer ──
static CapturedRow<PresentTimelineObservation>
    s_present_observation_ring[RING_SIZE];
static std::atomic<uint64_t> s_present_observation_write_pos{0};
static std::atomic<uint64_t> s_present_observation_read_pos{0};
static std::atomic<uint64_t> s_present_observation_dropped_rows{0};
static std::atomic<uint64_t> s_next_present_observation_sequence{1};

// ── Keyed NvAPI GPU-observation SPSC ring buffer ──
static CapturedRow<GpuObservationRow> s_gpu_observation_ring[RING_SIZE];
static std::atomic<uint64_t> s_gpu_observation_write_pos{0};
static std::atomic<uint64_t> s_gpu_observation_read_pos{0};
static std::atomic<uint64_t> s_gpu_observation_dropped_rows{0};
static std::atomic<uint64_t> s_next_gpu_observation_sequence{1};

static std::atomic<bool> s_stop_requested{false};
static HANDLE s_writer_thread = nullptr;
static FILE* s_frame_file = nullptr;
static FILE* s_native_pcl_file = nullptr;
static FILE* s_timeline_file = nullptr;
static FILE* s_present_timeline_file = nullptr;
static FILE* s_present_association_file = nullptr;
static FILE* s_gpu_observation_file = nullptr;
static char s_output_dir[MAX_PATH] = {};
static uint64_t s_qpc_frequency = 0;
static uint64_t s_capture_session = 0;
static uint32_t s_open_capture_epoch = 0;
static std::array<uint32_t, FRAME_TIMELINE_CAPTURE_CAPACITY> s_opened_captures{};
static PresentAssociationIndex s_present_association_index;
static PendingPresentObservations s_pending_observations;

static const char* FRAME_CSV_HEADER =
    "frame_id,timestamp_us,predicted_us,effective_interval_us,actual_frame_time_us,"
    "sleep_duration_us,wake_error_us,ceiling_margin_us,stress_level,cv,"
    "damping_correction_us,tier,overload,fg_divisor,mode,scanout_error_us,"
    "queue_depth,api,pqi,cadence_score,stutter_score,deadline_score,jitter_us,"
    "own_sleep_us,driver_sleep_us,gate_sleep_us,deadline_drift_us,predictor_warm,"
    "smoothness_us,reflex_injected,"
    "present_interval_us,present_cadence_smoothness_us,present_bias_us,"
    "feedback_rate,feedback_alpha,"
    "reflex_present_to_gpu_end_us,reflex_queue_trend_us,"
    "reflex_present_duration_us,reflex_gpu_active_us,"
    "reflex_ai_frame_time_us,reflex_cpu_latency_us,gate_margin_us,"
    "smoothing_offset_us,p99_render_time_us,total_frame_cost_us,"
    "adaptation_sample_sequence,adaptation_frame_time_us,"
    "adaptation_producer_epoch,adaptation_nvapi_frame_id,"
    "adaptation_cpu_active_us,adaptation_gpu_service_us,"
    "adaptation_owned_cpu_stage_us,adaptation_owned_gate_hold_us\n";

static const char* NATIVE_PCL_CSV_HEADER =
    "pcl_frame_id,present_start_qpc,display_qpc,sample_qpc,sample_age_us,"
    "i2fs_us,fs2p_us,p2d_us,total_us,dropped_accumulation_us,"
    "dropped_frame_count,status,lost_present_count,etw_events_lost,"
    "etw_buffers_lost,overflowed_present_count,qpc_frequency,"
    "pcl_sim_start_qpc,pcl_sim_end_qpc,pcl_render_submit_start_qpc,"
    "pcl_render_submit_end_qpc,pcl_present_start_qpc,pcl_present_end_qpc,"
    "effective_sim_start_qpc,previous_pcl_frame_id,pcl_frame_id_delta,"
    "previous_pcl_sim_start_qpc,pcl_sim_start_delta_us,"
    "provider_present_to_dxgi_present_us,frame_type,"
    "used_simulation_fallback\n";

static const char* TIMELINE_CSV_HEADER =
    "sequence,capture_epoch,event_type,path,identity_domain,producer_epoch,"
    "controller_generation,frame_id,clock_domain,qpc_frequency,event_qpc,"
    "capture_qpc,target_qpc,forwarding_order,flags,legacy_target_qpc,"
    "estimator_target_qpc,estimator_predicted_a_us,estimator_reserve_us,"
    "estimator_sample_count,estimator_stable_frame_count,estimator_regime,"
    "estimator_marker,estimator_fallback_reason,estimator_mode,"
    "estimator_active,adaptation_workload_offset_us,"
    "adaptation_configured_reserve_us,adaptive_reserve_proposed_us,"
    "adaptive_reserve_slack_us,adaptive_reserve_required_us,"
    "adaptive_reserve_miss_ema,adaptive_reserve_applied_us,"
    "adaptive_reserve_mode,adaptive_reserve_authority,"
    "adaptive_reserve_fallback_reason,adaptive_reserve_sample_count,"
    "adaptive_reserve_active_capped\n";

static const char* PRESENT_TIMELINE_CSV_HEADER =
    "span_sequence,event_type,capture_epoch,swapchain_generation,present_sequence,"
    "api_call_sequence,attempt_index,api,entry_point,native_swapchain,thread_id,"
    "sync_interval,present_flags,event_qpc,p_qpc,e_qpc,duration_qpc,"
    "result_domain,result_code,qpc_frequency,dropped_span_count,"
    "stale_span_count,invalid_span_count,unregistered_call_count,"
    "identity_failure_count,registry_overflow_count\n";

static const char* PRESENT_ASSOCIATION_CSV_HEADER =
    "observation_sequence,event_type,capture_epoch,observer_epoch,runtime,"
    "native_swapchain,thread_id,present_start_qpc,present_end_qpc,report_qpc,"
    "qpc_frequency,final_state,displayed_instance_count,reported_frame_type,"
    "frame_classification,classification_evidence,pcl_frame_id,"
    "pcl_sim_start_qpc,used_simulation_fallback,match_status,candidate_count,"
    "matched_capture_epoch,matched_span_sequence,matched_swapchain_generation,"
    "matched_present_sequence,matched_api_call_sequence,matched_attempt_index,"
    "matched_api,matched_entry_point,matched_p_qpc,matched_e_qpc,report_age_us,"
    "dropped_observation_count,retention_age_evictions,"
    "retention_capacity_evictions,unique_association_count,"
    "not_found_association_count,ambiguous_association_count,"
    "incompatible_observation_count,"
    "gpu_start_qpc,gpu_ready_qpc,gpu_duration_qpc\n";

static const char* GPU_OBSERVATION_CSV_HEADER =
    "observation_sequence,capture_epoch,producer_epoch,nvapi_frame_id,"
    "report_qpc,qpc_frequency,sim_start_qpc,sim_end_qpc,"
    "render_submit_start_qpc,render_submit_end_qpc,"
    "present_start_qpc,present_end_qpc,driver_start_qpc,driver_end_qpc,"
    "os_render_queue_start_qpc,os_render_queue_end_qpc,"
    "gpu_render_start_qpc,gpu_render_end_qpc,"
    "gpu_active_render_time_us,gpu_frame_time_us,ai_frame_time_us,"
    "dropped_observation_count\n";

static void CloseFiles()
{
    s_open_capture_epoch = 0;
    if (s_frame_file) {
        fclose(s_frame_file);
        s_frame_file = nullptr;
    }
    if (s_native_pcl_file) {
        fclose(s_native_pcl_file);
        s_native_pcl_file = nullptr;
    }
    if (s_timeline_file) {
        fclose(s_timeline_file);
        s_timeline_file = nullptr;
    }
    if (s_present_timeline_file) {
        fclose(s_present_timeline_file);
        s_present_timeline_file = nullptr;
    }
    if (s_present_association_file) {
        fclose(s_present_association_file);
        s_present_association_file = nullptr;
    }
    if (s_gpu_observation_file) {
        fclose(s_gpu_observation_file);
        s_gpu_observation_file = nullptr;
    }
}

static FILE* OpenCaptureFile(const char* path, const char* header)
{
    // Re-selecting an older epoch must append, never truncate. Inspect the file
    // itself so an earlier open failure cannot later create a headerless CSV.
    FILE* file = fopen(path, "a+");
    if (file) {
        if (fseek(file, 0, SEEK_END) != 0) {
            fclose(file);
            return nullptr;
        }
        if (ftell(file) == 0) fputs(header, file);
    }
    return file;
}

static void OpenNewFiles(uint32_t capture_epoch)
{
    CloseFiles();

    // Session + epoch suffix preserves every accepted capture, including an
    // enable/disable pair completed before the worker first wakes up.
    char process_name[MAX_PATH]{};
    const int name_length = snprintf(process_name, sizeof(process_name), "%s_%lu_%llu_capture_%u",
        Log_GetProcessName(), GetCurrentProcessId(),
        static_cast<unsigned long long>(s_capture_session), capture_epoch);
    if (name_length < 0 || static_cast<size_t>(name_length) >= sizeof(process_name) ||
        strlen(s_output_dir) + static_cast<size_t>(name_length) +
            sizeof("\\relimiter_present_associations_.csv") > MAX_PATH) {
        LOG_ERROR("CSV: capture path exceeds MAX_PATH; refusing truncated filenames");
        return;
    }
    const bool first_open =
        s_opened_captures[capture_epoch % s_opened_captures.size()] != capture_epoch;
    char frame_path[MAX_PATH]{};
    char native_pcl_path[MAX_PATH]{};
    char timeline_path[MAX_PATH]{};
    char present_timeline_path[MAX_PATH]{};
    char present_association_path[MAX_PATH]{};
    snprintf(
        frame_path,
        sizeof(frame_path),
        "%s\\relimiter_frames_%s.csv",
        s_output_dir,
        process_name);
    snprintf(
        native_pcl_path,
        sizeof(native_pcl_path),
        "%s\\relimiter_pcl_%s.csv",
        s_output_dir,
        process_name);
    snprintf(
        timeline_path,
        sizeof(timeline_path),
        "%s\\relimiter_timeline_%s.csv",
        s_output_dir,
        process_name);
    snprintf(
        present_timeline_path,
        sizeof(present_timeline_path),
        "%s\\relimiter_presents_%s.csv",
        s_output_dir,
        process_name);
    snprintf(
        present_association_path,
        sizeof(present_association_path),
        "%s\\relimiter_present_associations_%s.csv",
        s_output_dir,
        process_name);
    char gpu_observation_path[MAX_PATH]{};
    snprintf(
        gpu_observation_path,
        sizeof(gpu_observation_path),
        "%s\\relimiter_gpu_observations_%s.csv",
        s_output_dir,
        process_name);

    s_frame_file = OpenCaptureFile(frame_path, FRAME_CSV_HEADER);
    if (s_frame_file) {
        if (first_open) LOG_INFO("CSV: opened scheduler telemetry %s", frame_path);
    }
    else {
        LOG_WARN("CSV: failed to open scheduler telemetry %s", frame_path);
    }

    s_native_pcl_file = OpenCaptureFile(native_pcl_path, NATIVE_PCL_CSV_HEADER);
    if (s_native_pcl_file) {
        if (first_open) LOG_INFO("CSV: opened native PCL telemetry %s", native_pcl_path);
    }
    else {
        LOG_WARN("CSV: failed to open native PCL telemetry %s", native_pcl_path);
    }

    s_timeline_file = OpenCaptureFile(timeline_path, TIMELINE_CSV_HEADER);
    if (s_timeline_file) {
        if (first_open) LOG_INFO("CSV: opened frame timeline telemetry %s", timeline_path);
    }
    else {
        LOG_WARN("CSV: failed to open frame timeline telemetry %s", timeline_path);
    }

    s_present_timeline_file = OpenCaptureFile(
        present_timeline_path, PRESENT_TIMELINE_CSV_HEADER);
    if (s_present_timeline_file) {
        if (first_open) LOG_INFO("CSV: opened API Present timeline telemetry %s", present_timeline_path);
    }
    else {
        LOG_WARN("CSV: failed to open API Present timeline telemetry %s", present_timeline_path);
    }

    s_present_association_file = OpenCaptureFile(
        present_association_path, PRESENT_ASSOCIATION_CSV_HEADER);
    if (s_present_association_file) {
        if (first_open) LOG_INFO(
            "CSV: opened delayed Present association telemetry %s",
            present_association_path);
    }
    else {
        LOG_WARN(
            "CSV: failed to open delayed Present association telemetry %s",
            present_association_path);
    }

    s_gpu_observation_file = OpenCaptureFile(
        gpu_observation_path, GPU_OBSERVATION_CSV_HEADER);
    if (s_gpu_observation_file) {
        if (first_open) LOG_INFO(
            "CSV: opened keyed GPU observation telemetry %s",
            gpu_observation_path);
    }
    else {
        LOG_WARN(
            "CSV: failed to open keyed GPU observation telemetry %s",
            gpu_observation_path);
    }
    s_open_capture_epoch = capture_epoch;
}

static void WriteFrameRow(const FrameRow& row)
{
    if (!s_frame_file) return;
    fprintf(s_frame_file,
        "%llu,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.3f,%.4f,"
        "%.1f,%d,%d,%.2f,%d,%.1f,%d,%d,%.1f,%.3f,%.3f,%.3f,%.1f,"
        "%.1f,%.1f,%.1f,%.1f,%d,%.1f,%d,"
        "%.1f,%.1f,%.1f,%d,%.4f,"
        "%.1f,%.1f,"
        "%.1f,%.1f,%.1f,%.1f,%.1f,"
        "%.1f,%.1f,%.1f,%llu,%.1f,%u,%llu,%.1f,%.1f,%.1f,%.1f\n",
        row.frame_id, row.timestamp_us, row.predicted_us, row.effective_interval_us,
        row.actual_frame_time_us, row.sleep_duration_us, row.wake_error_us,
        row.ceiling_margin_us, row.stress_level, row.cv,
        row.damping_correction_us, row.tier, row.overload, row.fg_divisor,
        row.mode, row.scanout_error_us, row.queue_depth, row.api,
        row.pqi, row.cadence_score, row.stutter_score, row.deadline_score, row.jitter_us,
        row.own_sleep_us, row.driver_sleep_us, row.gate_sleep_us,
        row.deadline_drift_us, row.predictor_warm, row.smoothness_us,
        row.reflex_injected,
        row.present_interval_us, row.present_cadence_smoothness_us,
        row.present_bias_us, row.feedback_rate, row.feedback_alpha,
        row.reflex_present_to_gpu_end_us, row.reflex_queue_trend_us,
        row.reflex_present_duration_us, row.reflex_gpu_active_us,
        row.reflex_ai_frame_time_us, row.reflex_cpu_latency_us,
        row.gate_margin_us,
        row.smoothing_offset_us, row.p99_render_time_us,
        row.total_frame_cost_us,
        static_cast<unsigned long long>(row.adaptation_sample_sequence),
        row.adaptation_frame_time_us,
        row.adaptation_producer_epoch,
        static_cast<unsigned long long>(row.adaptation_nvapi_frame_id),
        row.adaptation_cpu_active_us,
        row.adaptation_gpu_service_us,
        row.adaptation_owned_cpu_stage_us,
        row.adaptation_owned_gate_hold_us);
}

static void WriteNativePclRow(const NativePclRow& row)
{
    if (!s_native_pcl_file) return;
    fprintf(
        s_native_pcl_file,
        "%u,%llu,%llu,%llu,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%u,%s,%u,%u,%u,%u,"
        "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%u,%lld,%llu,%.1f,%.1f,%d,%d\n",
        row.pcl_frame_id,
        static_cast<unsigned long long>(row.present_start_qpc),
        static_cast<unsigned long long>(row.display_qpc),
        static_cast<unsigned long long>(row.sample_qpc),
        row.sample_age_us,
        row.input_to_frame_start_us,
        row.frame_start_to_present_us,
        row.present_to_display_us,
        row.total_us,
        row.dropped_accumulation_us,
        row.dropped_frame_count,
        NativePcl_StatusText(row.status),
        row.lost_present_count,
        row.etw_events_lost,
        row.etw_buffers_lost,
        row.overflowed_present_count,
        static_cast<unsigned long long>(row.qpc_frequency),
        static_cast<unsigned long long>(row.pcl_sim_start_qpc),
        static_cast<unsigned long long>(row.pcl_sim_end_qpc),
        static_cast<unsigned long long>(row.pcl_render_submit_start_qpc),
        static_cast<unsigned long long>(row.pcl_render_submit_end_qpc),
        static_cast<unsigned long long>(row.pcl_present_start_qpc),
        static_cast<unsigned long long>(row.pcl_present_end_qpc),
        static_cast<unsigned long long>(row.effective_sim_start_qpc),
        row.previous_pcl_frame_id,
        static_cast<long long>(row.pcl_frame_id_delta),
        static_cast<unsigned long long>(row.previous_pcl_sim_start_qpc),
        row.pcl_sim_start_delta_us,
        row.provider_present_to_dxgi_present_us,
        row.frame_type,
        row.used_simulation_fallback ? 1 : 0);
}

static void WriteTimelineEvent(const FrameTimelineEvent& event)
{
    if (!s_timeline_file) return;
    fprintf(
        s_timeline_file,
        "%llu,%u,%s,%s,%s,%u,%u,%llu,%s,%llu,%lld,%lld,%lld,%s,%u,"
        "%lld,%lld,%.1f,%.1f,%u,%u,%s,%s,%s,%s,%d,"
        "%.1f,%.1f,%.1f,%.1f,%.1f,%.6f,%.1f,%s,%s,%s,%u,%d\n",
        static_cast<unsigned long long>(event.sequence),
        event.capture_epoch,
        FrameTimeline_EventTypeText(event.type),
        FrameTimeline_PathText(event.path),
        FrameTimeline_IdentityDomainText(event.key.identity_domain),
        event.key.producer_epoch,
        event.controller_generation,
        static_cast<unsigned long long>(event.key.normalized_frame_id),
        FrameTimeline_ClockDomainText(event.clock_domain),
        static_cast<unsigned long long>(s_qpc_frequency),
        static_cast<long long>(event.event_qpc),
        static_cast<long long>(event.capture_qpc),
        static_cast<long long>(event.target_qpc),
        FrameTimeline_ForwardingOrderText(event.forwarding_order),
        event.flags,
        static_cast<long long>(event.legacy_target_qpc),
        static_cast<long long>(event.estimator_target_qpc),
        event.estimator_predicted_arrival_lead_us,
        event.estimator_reserve_us,
        event.estimator_sample_count,
        event.estimator_stable_frame_count,
        ArrivalLead_RegimeText(event.estimator_regime),
        ArrivalLead_MarkerText(event.estimator_marker),
        ArrivalLead_FallbackReasonText(event.estimator_fallback_reason),
        ArrivalLead_ModeText(event.estimator_mode),
        event.estimator_active ? 1 : 0,
        event.adaptation_workload_offset_us,
        event.adaptation_configured_reserve_us,
        event.adaptive_reserve_proposed_us,
        event.adaptive_reserve_slack_us,
        event.adaptive_reserve_required_us,
        event.adaptive_reserve_miss_ema,
        event.adaptive_reserve_applied_us,
        AdaptiveDeadlineReserve_ModeText(event.adaptive_reserve_mode),
        AdaptiveDeadlineReserve_AuthorityText(
            event.adaptive_reserve_authority),
        AdaptiveDeadlineReserve_FallbackReasonText(
            event.adaptive_reserve_fallback_reason),
        event.adaptive_reserve_sample_count,
        event.adaptive_reserve_active_capped ? 1 : 0);
}

static void WritePresentTimelineSpan(const PresentTimelineSpan& span)
{
    if (!s_present_timeline_file) return;

    const long long duration_qpc = static_cast<long long>(
        span.e_qpc - span.p_qpc);
    const char* api = FrameTimeline_PresentApiText(span.api);
    const char* entry_point =
        FrameTimeline_PresentEntryPointText(span.entry_point);
    const auto write_event_prefix = [&](const char* event_type, int64_t event_qpc) {
        fprintf(
            s_present_timeline_file,
            "%llu,%s,%u,%llu,%llu,%llu,%u,%s,%s,%llu,%u,%u,%u,"
            "%lld,%lld,%lld,%lld,",
            static_cast<unsigned long long>(span.sequence),
            event_type,
            span.capture_epoch,
            static_cast<unsigned long long>(span.key.swapchain_generation),
            static_cast<unsigned long long>(span.key.present_sequence),
            static_cast<unsigned long long>(span.api_call_sequence),
            span.attempt_index,
            api,
            entry_point,
            static_cast<unsigned long long>(span.native_swapchain),
            span.thread_id,
            span.sync_interval,
            span.present_flags,
            static_cast<long long>(event_qpc),
            static_cast<long long>(span.p_qpc),
            static_cast<long long>(span.e_qpc),
            duration_qpc);
    };

    write_event_prefix("api_present_start", span.p_qpc);
    fprintf(
        s_present_timeline_file,
        ",,%llu,,,,,,\n",
        static_cast<unsigned long long>(s_qpc_frequency));
    write_event_prefix("api_present_end", span.e_qpc);
    fprintf(
        s_present_timeline_file,
        "hresult,%u,%llu,,,,,,\n",
        span.result_code,
        static_cast<unsigned long long>(s_qpc_frequency));
}

static void WritePresentAssociationObservation(
    const PresentTimelineObservation& observation,
    const PresentAssociationResult& association)
{
    if (!s_present_association_file) return;

    fprintf(
        s_present_association_file,
        "%llu,present_observation,%u,%u,%s,%llu,%u,%llu,%llu,%llu,%llu,%s,"
        "%u,%d,%s,%u,%u,%llu,%d,%s,%u",
        static_cast<unsigned long long>(observation.sequence),
        observation.capture_epoch,
        observation.observer_epoch,
        PresentAssociation_RuntimeText(observation.runtime),
        static_cast<unsigned long long>(observation.native_swapchain),
        observation.thread_id,
        static_cast<unsigned long long>(observation.present_start_qpc),
        static_cast<unsigned long long>(observation.present_end_qpc),
        static_cast<unsigned long long>(observation.report_qpc),
        static_cast<unsigned long long>(observation.qpc_frequency),
        PresentAssociation_FinalStateText(observation.final_state),
        observation.displayed_instance_count,
        observation.reported_frame_type,
        PresentAssociation_FrameClassificationText(
            observation.frame_classification),
        observation.classification_evidence,
        observation.pcl_frame_id,
        static_cast<unsigned long long>(observation.pcl_sim_start_qpc),
        observation.used_simulation_fallback ? 1 : 0,
        PresentAssociation_StatusText(association.status),
        association.candidate_count);

    if (association.status == PresentAssociationStatus::Unique) {
        const PresentTimelineSpan& source = association.source;
        fprintf(
            s_present_association_file,
            ",%u,%llu,%llu,%llu,%llu,%u,%s,%s,%lld,%lld,%.1f",
            source.capture_epoch,
            static_cast<unsigned long long>(source.sequence),
            static_cast<unsigned long long>(source.key.swapchain_generation),
            static_cast<unsigned long long>(source.key.present_sequence),
            static_cast<unsigned long long>(source.api_call_sequence),
            source.attempt_index,
            FrameTimeline_PresentApiText(source.api),
            FrameTimeline_PresentEntryPointText(source.entry_point),
            static_cast<long long>(source.p_qpc),
            static_cast<long long>(source.e_qpc),
            association.report_age_us);
        for (int field = 33; field <= 39; ++field) {
            fputc(',', s_present_association_file);
        }
    }
    else {
        for (int field = 22; field <= 39; ++field) {
            fputc(',', s_present_association_file);
        }
    }
    fprintf(
        s_present_association_file,
        ",%llu,%llu,%llu\n",
        static_cast<unsigned long long>(observation.gpu_start_qpc),
        static_cast<unsigned long long>(observation.gpu_ready_qpc),
        static_cast<unsigned long long>(observation.gpu_duration_qpc));
}

static void WritePresentAssociationDiagnostics(
    const char* event_type,
    uint32_t capture_epoch)
{
    if (!s_present_association_file) return;

    fprintf(s_present_association_file, ",%s,%u", event_type, capture_epoch);
    for (int field = 4; field < 33; ++field) {
        fputc(',', s_present_association_file);
    }
    const PresentAssociationDiagnostics diagnostics =
        s_present_association_index.Diagnostics();
    fprintf(
        s_present_association_file,
        ",%llu,%llu,%llu,%llu,%llu,%llu,%llu,,,\n",
        static_cast<unsigned long long>(
            s_present_observation_dropped_rows.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(diagnostics.retention_age_evictions),
        static_cast<unsigned long long>(diagnostics.retention_capacity_evictions),
        static_cast<unsigned long long>(diagnostics.unique_associations),
        static_cast<unsigned long long>(diagnostics.not_found_associations),
        static_cast<unsigned long long>(diagnostics.ambiguous_associations),
        static_cast<unsigned long long>(diagnostics.incompatible_observations));
}

static void WriteGpuObservationRow(const GpuObservationRow& row)
{
    if (!s_gpu_observation_file) return;
    fprintf(
        s_gpu_observation_file,
        "%llu,%u,%u,%llu,%llu,%llu,"
        "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,"
        "%u,%u,%u,%llu\n",
        static_cast<unsigned long long>(row.sequence),
        row.capture_epoch,
        row.producer_epoch,
        static_cast<unsigned long long>(row.nvapi_frame_id),
        static_cast<unsigned long long>(row.report_qpc),
        static_cast<unsigned long long>(s_qpc_frequency),
        static_cast<unsigned long long>(row.sim_start_qpc),
        static_cast<unsigned long long>(row.sim_end_qpc),
        static_cast<unsigned long long>(row.render_submit_start_qpc),
        static_cast<unsigned long long>(row.render_submit_end_qpc),
        static_cast<unsigned long long>(row.present_start_qpc),
        static_cast<unsigned long long>(row.present_end_qpc),
        static_cast<unsigned long long>(row.driver_start_qpc),
        static_cast<unsigned long long>(row.driver_end_qpc),
        static_cast<unsigned long long>(row.os_render_queue_start_qpc),
        static_cast<unsigned long long>(row.os_render_queue_end_qpc),
        static_cast<unsigned long long>(row.gpu_render_start_qpc),
        static_cast<unsigned long long>(row.gpu_render_end_qpc),
        row.gpu_active_render_time_us,
        row.gpu_frame_time_us,
        row.ai_frame_time_us,
        static_cast<unsigned long long>(
            s_gpu_observation_dropped_rows.load(std::memory_order_relaxed)));
}

static void WritePresentCaptureDiagnostics(
    const char* event_type,
    uint32_t capture_epoch)
{
    if (!s_present_timeline_file) return;

    fprintf(s_present_timeline_file, ",%s,%u", event_type, capture_epoch);
    for (int field = 4; field < 20; ++field) {
        fputc(',', s_present_timeline_file);
    }
    fprintf(
        s_present_timeline_file,
        ",%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
        static_cast<unsigned long long>(s_qpc_frequency),
        static_cast<unsigned long long>(FrameTimeline_DroppedPresentSpans()),
        static_cast<unsigned long long>(FrameTimeline_StalePresentSpans()),
        static_cast<unsigned long long>(FrameTimeline_InvalidPresentSpans()),
        static_cast<unsigned long long>(FrameTimeline_UnregisteredPresentCalls()),
        static_cast<unsigned long long>(
            FrameTimeline_PresentAttemptIdentityFailures()),
        static_cast<unsigned long long>(
            FrameTimeline_PresentRegistryOverflows()));
}

static void SelectCapture(uint32_t epoch)
{
    if (s_open_capture_epoch == epoch) return;
    OpenNewFiles(epoch);
    uint32_t& opened = s_opened_captures[epoch % s_opened_captures.size()];
    if (opened != epoch) {
        opened = epoch;
        WritePresentCaptureDiagnostics("capture_start", epoch);
        WritePresentAssociationDiagnostics("capture_start", epoch);
    }
}

static bool DrainFrameRows()
{
    uint64_t read_position = s_read_pos.load(std::memory_order_relaxed);
    const uint64_t write_position = s_write_pos.load(std::memory_order_acquire);
    bool consumed_rows = false;

    while (read_position < write_position) {
        const CapturedRow<FrameRow>& captured =
            s_ring[read_position & RING_MASK];
        SelectCapture(captured.capture_epoch);
        WriteFrameRow(captured.row);
        ++read_position;
        consumed_rows = true;
    }
    s_read_pos.store(read_position, std::memory_order_release);
    return consumed_rows;
}

static bool DrainNativePclRows()
{
    uint64_t read_position = s_native_pcl_read_pos.load(std::memory_order_relaxed);
    const uint64_t write_position = s_native_pcl_write_pos.load(std::memory_order_acquire);
    bool consumed_rows = false;

    while (read_position < write_position) {
        const CapturedRow<NativePclRow>& captured =
            s_native_pcl_ring[read_position & RING_MASK];
        SelectCapture(captured.capture_epoch);
        WriteNativePclRow(captured.row);
        ++read_position;
        consumed_rows = true;
    }
    s_native_pcl_read_pos.store(read_position, std::memory_order_release);
    return consumed_rows;
}

static bool DrainPresentObservations(
    uint64_t sweep,
    uint64_t now_qpc)
{
    uint64_t read_position =
        s_present_observation_read_pos.load(std::memory_order_relaxed);
    const uint64_t write_position =
        s_present_observation_write_pos.load(std::memory_order_acquire);
    bool consumed_observations = false;

    while (read_position < write_position) {
        const CapturedRow<PresentTimelineObservation>& captured =
            s_present_observation_ring[read_position & RING_MASK];
        if (!s_pending_observations.Push(captured.row, sweep, now_qpc)) {
            s_present_observation_dropped_rows.fetch_add(
                1, std::memory_order_relaxed);
        }
        ++read_position;
        consumed_observations = true;
    }
    s_present_observation_read_pos.store(
        read_position, std::memory_order_release);
    return consumed_observations;
}

static bool DrainGpuObservations()
{
    uint64_t read_position =
        s_gpu_observation_read_pos.load(std::memory_order_relaxed);
    const uint64_t write_position =
        s_gpu_observation_write_pos.load(std::memory_order_acquire);
    bool consumed_observations = false;

    while (read_position < write_position) {
        const CapturedRow<GpuObservationRow>& captured =
            s_gpu_observation_ring[read_position & RING_MASK];
        SelectCapture(captured.capture_epoch);
        WriteGpuObservationRow(captured.row);
        ++read_position;
        consumed_observations = true;
    }
    s_gpu_observation_read_pos.store(
        read_position, std::memory_order_release);
    return consumed_observations;
}

static bool DrainTimelineEvents()
{
    bool consumed_events = false;
    for (size_t count = 0; count < FRAME_TIMELINE_QUEUE_CAPACITY; ++count) {
        FrameTimelineEvent event{};
        if (!FrameTimeline_TryPop(&event)) break;
        SelectCapture(event.capture_epoch);
        WriteTimelineEvent(event);
        consumed_events = true;
    }
    return consumed_events;
}

static bool DrainPresentTimelineSpans()
{
    bool consumed_spans = false;
    for (size_t count = 0; count < PRESENT_TIMELINE_QUEUE_CAPACITY; ++count) {
        PresentTimelineSpan span{};
        if (!FrameTimeline_TryPopPresentSpan(&span)) break;
        SelectCapture(span.capture_epoch);
        WritePresentTimelineSpan(span);
        s_present_association_index.Retain(span, s_qpc_frequency);
        consumed_spans = true;
    }
    return consumed_spans;
}

static void FlushFiles()
{
    if (s_frame_file) fflush(s_frame_file);
    if (s_native_pcl_file) fflush(s_native_pcl_file);
    if (s_timeline_file) fflush(s_timeline_file);
    if (s_present_timeline_file) fflush(s_present_timeline_file);
    if (s_present_association_file) fflush(s_present_association_file);
    if (s_gpu_observation_file) fflush(s_gpu_observation_file);
}

static void WriterThreadImpl()
{
    uint32_t retired_epoch = FrameTimeline_RetiredCaptureEpoch();
    uint32_t observed_epoch = retired_epoch;
    uint32_t closed_through_epoch = retired_epoch;
    uint64_t sweep = 0;
    uint64_t reported_rejections = FrameTimeline_RejectedCaptureTransitions();

    for (;;) {
        ++sweep;
        const uint64_t rejected = FrameTimeline_RejectedCaptureTransitions();
        if (rejected != reported_rejections) {
            LOG_WARN("CSV: rejected %llu capture transitions (journal full, stopped, or epoch exhausted)",
                static_cast<unsigned long long>(rejected - reported_rejections));
            reported_rejections = rejected;
        }
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        FrameTimelineCaptureState checkpoint{};
        const bool quiescent = FrameTimeline_TryCaptureCheckpoint(&checkpoint);

        // Read the journal, not just the latest state. A control thread may be
        // paused after its state CAS but before publishing its journal entry.
        FrameTimelineCaptureState transition{};
        while (observed_epoch < checkpoint.epoch &&
               FrameTimeline_ReadCaptureTransition(observed_epoch + 1, &transition)) {
            observed_epoch = transition.epoch;
            if (transition.enabled) SelectCapture(transition.epoch);
        }

        // Every accepted row of a closed checkpoint epoch is already published.
        // Drain AFTER taking that checkpoint, including the entire MPSC prefix.
        // Epochs may interleave: never stop at (or discard) a newer epoch's row.
        bool consumed = DrainFrameRows();
        consumed = DrainNativePclRows() || consumed;
        consumed = DrainGpuObservations() || consumed;
        consumed = DrainTimelineEvents() || consumed;
        consumed = DrainPresentTimelineSpans() || consumed;
        consumed = DrainPresentObservations(
            sweep, static_cast<uint64_t>(now.QuadPart)) || consumed;
        if (quiescent) {
            closed_through_epoch = checkpoint.enabled
                ? checkpoint.epoch - 1 : checkpoint.epoch;
        }

        PresentTimelineObservation observation{};
        PresentAssociationResult association{};
        // A checkpoint from a previous sweep does not cover a publisher that
        // entered since then. Pass no drain evidence while one is in flight.
        while (s_pending_observations.TryFinalize(
                        { quiescent ? sweep : 0, static_cast<uint64_t>(now.QuadPart), s_qpc_frequency,
                            closed_through_epoch }, &s_present_association_index,
            &observation, &association)) {
            SelectCapture(observation.capture_epoch);
            WritePresentAssociationObservation(observation, association);
            consumed = true;
        }

        while (retired_epoch < observed_epoch &&
               retired_epoch < closed_through_epoch &&
               !s_pending_observations.HasCapture(retired_epoch + 1)) {
            FrameTimeline_ReadCaptureTransition(retired_epoch + 1, &transition);
            if (transition.enabled) {
                SelectCapture(transition.epoch);
                WritePresentCaptureDiagnostics("capture_end", transition.epoch);
                WritePresentAssociationDiagnostics("capture_end", transition.epoch);
                CloseFiles();
                s_present_association_index.ClearCaptureSpans(transition.epoch);
            }
            FrameTimeline_AcknowledgeCapture(++retired_epoch);
        }

        FlushFiles();
        if (s_stop_requested.load(std::memory_order_acquire)) {
            const auto stopped = FrameTimeline_CurrentCaptureState();
            if (!stopped.enabled && retired_epoch == stopped.epoch) {
                CloseFiles();
                break;
            }
        }
        if (!consumed) Sleep(50); // bounded retry; no producer waits on the worker
    }
}

static DWORD WINAPI WriterThread(LPVOID) noexcept
{
    try {
        WriterThreadImpl();
    }
    catch (...) {
        OutputDebugStringA("ReLimiter: CSV writer rejected an exception.\n");
        CloseFiles();
    }
    return 0;
}

void CSV_Init(const char* output_dir)
{
    // Never reset storage still owned by a writer whose join timed out.
    if (s_writer_thread != nullptr) return;
    strncpy(s_output_dir, output_dir, MAX_PATH - 1);
    s_output_dir[MAX_PATH - 1] = '\0';
    s_write_pos.store(0, std::memory_order_relaxed);
    s_read_pos.store(0, std::memory_order_relaxed);
    s_dropped_frame_rows.store(0, std::memory_order_relaxed);
    s_native_pcl_write_pos.store(0, std::memory_order_relaxed);
    s_native_pcl_read_pos.store(0, std::memory_order_relaxed);
    s_native_pcl_dropped_rows.store(0, std::memory_order_relaxed);
    s_present_observation_write_pos.store(0, std::memory_order_relaxed);
    s_present_observation_read_pos.store(0, std::memory_order_relaxed);
    s_present_observation_dropped_rows.store(0, std::memory_order_relaxed);
    s_next_present_observation_sequence.store(1, std::memory_order_relaxed);
    s_gpu_observation_write_pos.store(0, std::memory_order_relaxed);
    s_gpu_observation_read_pos.store(0, std::memory_order_relaxed);
    s_gpu_observation_dropped_rows.store(0, std::memory_order_relaxed);
    s_next_gpu_observation_sequence.store(1, std::memory_order_relaxed);
    s_opened_captures.fill(0);
    s_pending_observations.Reset();
    s_present_association_index.Reset();
    FILETIME session_time{};
    GetSystemTimeAsFileTime(&session_time);
    s_capture_session = (static_cast<uint64_t>(session_time.dwHighDateTime) << 32) |
        session_time.dwLowDateTime;
    LARGE_INTEGER qpc_frequency;
    QueryPerformanceFrequency(&qpc_frequency);
    s_qpc_frequency = static_cast<uint64_t>(qpc_frequency.QuadPart);
    FrameTimeline_SetEnabled(false);
    FrameTimeline_ResumeCapture();
    s_stop_requested.store(false, std::memory_order_relaxed);
    s_writer_thread = CreateThread(
        nullptr, 0, WriterThread, nullptr, 0, nullptr);
    if (!s_writer_thread) {
        FrameTimeline_StopCapture();
        s_stop_requested.store(true, std::memory_order_release);
        LOG_ERROR("CSV: failed to start writer thread (%lu)", GetLastError());
        return;
    }
    LOG_INFO("CSV writer initialized: dir=%s", s_output_dir);
}

bool CSV_Shutdown()
{
    // Stop is sticky: a racing UI enable/rotation cannot reopen admission.
    FrameTimeline_StopCapture();
    s_stop_requested.store(true, std::memory_order_release);
    if (s_writer_thread) {
        DWORD wait_status = WaitForSingleObject(s_writer_thread, 2500);
        if (wait_status != WAIT_OBJECT_0) {
            CancelSynchronousIo(s_writer_thread);
            wait_status = WaitForSingleObject(s_writer_thread, 500);
        }
        if (wait_status != WAIT_OBJECT_0) {
            OutputDebugStringA(
                "ReLimiter: CSV writer did not stop; file state retained.\n");
            return false;
        }
        CloseHandle(s_writer_thread);
        s_writer_thread = nullptr;
    }

    const uint64_t rejected_transitions = FrameTimeline_RejectedCaptureTransitions();
    if (rejected_transitions != 0) {
        LOG_WARN("CSV: rejected %llu capture transitions (journal full or stopped)",
            static_cast<unsigned long long>(rejected_transitions));
    }
    const uint64_t dropped_frame_rows = s_dropped_frame_rows.load(std::memory_order_relaxed);
    if (dropped_frame_rows != 0) {
        LOG_WARN("CSV: dropped %llu scheduler rows because the telemetry ring was full",
            static_cast<unsigned long long>(dropped_frame_rows));
    }
    const uint64_t dropped_native_pcl_rows =
        s_native_pcl_dropped_rows.load(std::memory_order_relaxed);
    if (dropped_native_pcl_rows != 0) {
        LOG_WARN(
            "CSV: dropped %llu native PCL rows because the telemetry ring was full",
            static_cast<unsigned long long>(dropped_native_pcl_rows));
    }
    const uint64_t dropped_present_observations =
        s_present_observation_dropped_rows.load(std::memory_order_relaxed);
    if (dropped_present_observations != 0) {
        LOG_WARN(
            "CSV: dropped %llu delayed Present observations because the telemetry ring or pending FIFO was full",
            static_cast<unsigned long long>(dropped_present_observations));
    }
    const uint64_t dropped_gpu_observations =
        s_gpu_observation_dropped_rows.load(std::memory_order_relaxed);
    if (dropped_gpu_observations != 0) {
        LOG_WARN(
            "CSV: dropped %llu GPU observations because the telemetry ring was full",
            static_cast<unsigned long long>(dropped_gpu_observations));
    }
    const uint64_t dropped_timeline_events = FrameTimeline_DroppedEvents();
    if (dropped_timeline_events != 0) {
        LOG_WARN(
            "CSV: dropped %llu frame timeline events because the telemetry queue was full",
            static_cast<unsigned long long>(dropped_timeline_events));
    }
    const uint64_t stale_timeline_events = FrameTimeline_StaleEvents();
    if (stale_timeline_events != 0) {
        LOG_WARN(
            "CSV: rejected %llu frame timeline events from stale capture epochs",
            static_cast<unsigned long long>(stale_timeline_events));
    }
    const uint64_t dropped_present_spans = FrameTimeline_DroppedPresentSpans();
    if (dropped_present_spans != 0) {
        LOG_WARN(
            "CSV: dropped %llu API Present spans because the telemetry queue was full",
            static_cast<unsigned long long>(dropped_present_spans));
    }
    const uint64_t invalid_present_spans = FrameTimeline_InvalidPresentSpans();
    if (invalid_present_spans != 0) {
        LOG_WARN(
            "CSV: rejected %llu invalid API Present spans",
            static_cast<unsigned long long>(invalid_present_spans));
    }
    const uint64_t stale_present_spans = FrameTimeline_StalePresentSpans();
    if (stale_present_spans != 0) {
        LOG_WARN(
            "CSV: rejected %llu API Present spans from stale capture epochs",
            static_cast<unsigned long long>(stale_present_spans));
    }
    const uint64_t present_registry_overflows =
        FrameTimeline_PresentRegistryOverflows();
    if (present_registry_overflows != 0) {
        LOG_WARN(
            "CSV: failed to register %llu API Present swapchain lifetimes",
            static_cast<unsigned long long>(present_registry_overflows));
    }
    const uint64_t present_registration_mismatches =
        FrameTimeline_PresentRegistrationMismatches();
    if (present_registration_mismatches != 0) {
        LOG_WARN(
            "CSV: replaced %llu active API Present identities after pointer/API mismatches",
            static_cast<unsigned long long>(present_registration_mismatches));
    }
    const uint64_t unregistered_present_calls =
        FrameTimeline_UnregisteredPresentCalls();
    if (unregistered_present_calls != 0) {
        LOG_WARN(
            "CSV: skipped %llu API Present calls without an active swapchain identity",
            static_cast<unsigned long long>(unregistered_present_calls));
    }
    const uint64_t present_attempt_identity_failures =
        FrameTimeline_PresentAttemptIdentityFailures();
    if (present_attempt_identity_failures != 0) {
        LOG_WARN(
            "CSV: skipped %llu API Present attempts after identity validation failed",
            static_cast<unsigned long long>(present_attempt_identity_failures));
    }
    LOG_INFO("CSV writer shutdown");
    return true;
}

void CSV_Push(const FrameRow& row)
{
    const FrameTimelineCaptureLease lease;
    const FrameTimelineCaptureState capture = lease.capture;
    if (!capture.enabled) return;

    const uint64_t write_position = s_write_pos.load(std::memory_order_relaxed);
    const uint64_t read_position = s_read_pos.load(std::memory_order_acquire);

    // Drop if the ring is full; the producer never blocks.
    if (write_position - read_position >= RING_SIZE) {
        s_dropped_frame_rows.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    s_ring[write_position & RING_MASK] = { capture.epoch, row };
    s_write_pos.store(write_position + 1, std::memory_order_release);
}

void CSV_PushNativePcl(const NativePclRow& row)
{
    const FrameTimelineCaptureLease lease;
    const FrameTimelineCaptureState capture = lease.capture;
    if (!capture.enabled) return;

    const uint64_t write_position =
        s_native_pcl_write_pos.load(std::memory_order_relaxed);
    const uint64_t read_position =
        s_native_pcl_read_pos.load(std::memory_order_acquire);

    if (write_position - read_position >= RING_SIZE) {
        s_native_pcl_dropped_rows.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    s_native_pcl_ring[write_position & RING_MASK] = { capture.epoch, row };
    s_native_pcl_write_pos.store(write_position + 1, std::memory_order_release);
}

void CSV_PushPresentObservation(
    const PresentTimelineObservation& observation)
{
    const FrameTimelineCaptureLease lease;
    const FrameTimelineCaptureState capture = lease.capture;
    if (!capture.enabled) return;

    const uint64_t sequence =
        s_next_present_observation_sequence.fetch_add(
            1, std::memory_order_relaxed);
    const uint64_t write_position =
        s_present_observation_write_pos.load(std::memory_order_relaxed);
    const uint64_t read_position =
        s_present_observation_read_pos.load(std::memory_order_acquire);

    if (write_position - read_position >= RING_SIZE) {
        s_present_observation_dropped_rows.fetch_add(
            1, std::memory_order_relaxed);
        return;
    }

    PresentTimelineObservation captured_observation = observation;
    captured_observation.sequence = sequence;
    captured_observation.capture_epoch = capture.epoch;
    s_present_observation_ring[write_position & RING_MASK] = {
        capture.epoch,
        captured_observation
    };
    s_present_observation_write_pos.store(
        write_position + 1, std::memory_order_release);
}

void CSV_PushGpuObservation(const GpuObservationRow& row)
{
    const FrameTimelineCaptureLease lease;
    const FrameTimelineCaptureState capture = lease.capture;
    if (!capture.enabled) return;

    const uint64_t sequence =
        s_next_gpu_observation_sequence.fetch_add(
            1, std::memory_order_relaxed);
    const uint64_t write_position =
        s_gpu_observation_write_pos.load(std::memory_order_relaxed);
    const uint64_t read_position =
        s_gpu_observation_read_pos.load(std::memory_order_acquire);

    if (write_position - read_position >= RING_SIZE) {
        s_gpu_observation_dropped_rows.fetch_add(
            1, std::memory_order_relaxed);
        return;
    }

    GpuObservationRow captured_observation = row;
    captured_observation.sequence = sequence;
    captured_observation.capture_epoch = capture.epoch;
    s_gpu_observation_ring[write_position & RING_MASK] = {
        capture.epoch,
        captured_observation
    };
    s_gpu_observation_write_pos.store(
        write_position + 1, std::memory_order_release);
}

void CSV_SetEnabled(bool enabled)
{
    FrameTimeline_SetEnabled(enabled);
}

bool CSV_IsEnabled()
{
    return FrameTimeline_IsEnabled();
}

void CSV_Rotate()
{
    if (FrameTimeline_IsEnabled()) {
        FrameTimeline_BeginCaptureEpoch();
    }
}
