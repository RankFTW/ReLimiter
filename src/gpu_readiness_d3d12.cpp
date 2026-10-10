#include "gpu_readiness.h"
#include "gpu_present_gate.h"
#include "frame_timeline.h"
#include "native_present_gate.h"
#include "runtime_lifecycle.h"
#include "streamline_hooks.h"
#include "swapchain_manager.h"
#include "logger.h"
#include "gpu_readiness_d3d12_queue.h"
#include "pacing_telemetry.h"
#include "frame_plan.h"
#include <array>
#include <atomic>

namespace {
constexpr size_t kQueueCapacity = 32;
constexpr size_t kSampleCapacity = 256;
enum class QueueState : uint8_t { Empty, Creating, Requested, Ready, Failed, Reclaiming };
enum class SampleState : uint8_t { Empty, Preparing, Pending };
constexpr uint64_t kFinishValid = 1, kFinishInvalid = 2, kFinishClosed = 3;
struct QueueSlot : GpuReadinessQueue {
    std::atomic<QueueState> state{QueueState::Empty};
    std::atomic<bool> retired{false};
    std::atomic<uint64_t> identity{0}; // visible even while AddRef is in flight
    std::atomic<uint32_t> scope_pins{0};
    uint64_t generation=0;
    uint64_t device_identity=0; // validated native device interface for this lifetime
    bool resources_released=false; // sampler-owned while Reclaiming
    std::atomic_flag signal_busy = ATOMIC_FLAG_INIT;
    uint64_t next_value = 1; // signal_busy only; allocated and signaled in order
    std::atomic_flag observation_busy = ATOMIC_FLAG_INIT;
    ReadyPresentObservation latest_prehold{};
    std::atomic<uint64_t> observation_faults{0};
};
struct SampleSlot {
    std::atomic<SampleState> state{SampleState::Empty};
    std::atomic<uint64_t> finish_token{0}; // sample_id<<2 | qualification; never touches payload
    GpuReadinessSample sample{};
    uint64_t native_swapchain = 0;
    QueueSlot* queue = nullptr;
    GpuTimestampSlot* timestamp_slot = nullptr;
    ReadyPresentObservation present_observation{};
    PacingFrameKey source_key{}; // pacing telemetry; valid only when the native gate attributed the frame
};
struct PresentScope {
    uint64_t native = 0;
    uint64_t generation = 0;
    uint64_t settings = 0;
    QueueSlot* queue = nullptr;
    SampleSlot* sample = nullptr;
    uint64_t sample_id = 0; // TLS may outlive an abandoned/reused sample slot
    uint32_t depth = 0;
    uint32_t calls = 0;
    bool valid = false;
    bool completed = false;
};
std::array<QueueSlot, kQueueCapacity> s_queues;
std::array<SampleSlot, kSampleCapacity> s_samples;
SRWLOCK s_registration=SRWLOCK_INIT;
std::atomic<uint64_t> s_queue_generation{1};
std::atomic<bool> s_stopping{true};
std::atomic<uint64_t> s_sample_sequence{1}, s_dropped{0}, s_signals{0}, s_completions{0};
HANDLE s_thread = nullptr;
HANDLE s_stop_event = nullptr;
HANDLE s_work_event = nullptr; // retained through callback rundown, not closed by Stop
int64_t s_frequency = 0; // set before thread/callback publication
thread_local PresentScope s_scope;

int64_t Qpc() noexcept { LARGE_INTEGER qpc{}; QueryPerformanceCounter(&qpc); return qpc.QuadPart; }
void WakeWorker() noexcept { if (s_work_event) SetEvent(s_work_event); }

bool Eligible() noexcept {
    // Explicit CSV observer in native Shadow may measure the exposed queue
    // prefix under static DLSS FG. It cannot create runtime gate authority:
    // only Native status populates the learning/source identity below.
    const bool fg_observation = GpuReadiness_IsEnabled() && FrameTimeline_IsEnabled() &&
        NativeGate_GetMode() == NativeGateMode::Shadow &&
        NativeGate_GetContext(Qpc()).observe_frame_generation;
    return !s_stopping.load(std::memory_order_acquire) && !RuntimeLifecycle_IsStopping() &&
        (GpuReadiness_IsEnabled() || GpuPresent_RequiresReadiness()) && SwapMgr_GetActiveAPI() == ActiveAPI::DX12 &&
        (fg_observation || (NativeGate_HasNoFrameGeneration(FrameTimeline_CurrentControllerGeneration()) &&
        !g_fg_active.load(std::memory_order_relaxed) && !g_fg_presenting.load(std::memory_order_relaxed) &&
        g_fg_mode.load(std::memory_order_relaxed) == 0));
}
QueueSlot* FindOrRequest(ID3D12CommandQueue* queue, uint64_t expected_device=0) noexcept {
    if (!queue) return nullptr;
    if (!TryAcquireSRWLockExclusive(&s_registration)) { ++s_dropped; return nullptr; }
    QueueSlot* available = nullptr;
    for (auto& slot : s_queues) {
        const auto state = slot.state.load(std::memory_order_acquire);
        if (slot.identity.load() == reinterpret_cast<uint64_t>(queue)) {
            const bool ready=state==QueueState::Ready && slot.device_identity==expected_device && !slot.retired.load() && !slot.faulted.load() && !slot.uncertain_submission.load();
            if(ready)slot.scope_pins.fetch_add(1,std::memory_order_acq_rel);
            ReleaseSRWLockExclusive(&s_registration);
            return ready?&slot:nullptr;
        }
        if (state == QueueState::Empty && !available) available = &slot;
    }
    if (available) {
        available->generation=s_queue_generation.fetch_add(1,std::memory_order_relaxed);
        available->device_identity=expected_device;
        available->identity.store(reinterpret_cast<uint64_t>(queue));
        available->state.store(QueueState::Creating, std::memory_order_release);
    }
    ReleaseSRWLockExclusive(&s_registration);
    if (!available) { ++s_dropped; return nullptr; }
    if(expected_device && !GpuReadinessQueue_MatchesDevice(queue,reinterpret_cast<ID3D12Device*>(expected_device))) {
        available->state.store(QueueState::Failed,std::memory_order_release);++s_dropped;return nullptr;
    }
    if (!GpuReadinessQueue_Retain(queue)) { available->state.store(QueueState::Failed); ++s_dropped; return nullptr; }
    available->queue = queue;
    available->state.store(QueueState::Requested, std::memory_order_release);
    WakeWorker();
    return nullptr; // worker creates fence; never wait for it here
}
// Pacing telemetry (phase0_measurement_design.md): one record per published
// sample. ready_qpc is the mapped GPU completion query, 0 when not valid.
void PushPacingReady(const SampleSlot& slot) noexcept {
    const auto& sample = slot.sample;
    const bool valid = sample.status == GpuReadinessStatus::Completed &&
        sample.timestamp.status == GpuTimestampStatus::Valid;
    PacingReadyRecord record;
    record.key = slot.source_key;
    record.swapchain_generation = sample.swapchain_generation;
    record.present_sequence = sample.present_sequence;
    record.submit_qpc = sample.timestamp.submit_before_qpc;
    record.ready_qpc = valid ? sample.timestamp.mapped_qpc : 0;
    record.valid = static_cast<uint8_t>(valid);
    record.before_gate = static_cast<uint8_t>(sample.before_gate ? 1 : 0);
    PacingTelemetry_PushReady(record);
}
// GpuReady r of an H3 Present (design 2.2): the valid pre-gate readiness query
// of a runtime Present whose exact plan is still registered on GpuQueueWait.
// Find is non-consuming, so the output claim stays with the executor.
void PushGpuReady(const SampleSlot& slot) noexcept {
    const auto& sample = slot.sample;
    const auto& observation = slot.present_observation;
    if (!sample.before_gate || !observation.identity.runtime || !observation.source_frame ||
        sample.status != GpuReadinessStatus::Completed || sample.timestamp.status != GpuTimestampStatus::Valid ||
        sample.timestamp.mapped_qpc <= 0) return;
    const PacingPlanKey key{PacingIdentityDomain::NvApiFrame, observation.identity.producer,
        observation.identity.controller, observation.source_frame};
    PacingFramePlan plan;
    if (!g_frame_plans.Find(key, &plan) || plan.rung != PacingRung::GpuQueueWait || !plan.grid_index) return;
    PacingStageEvent event;
    event.key = key; event.grid_index = plan.grid_index; event.qpc = sample.timestamp.mapped_qpc;
    event.kind = PacingStageEventKind::GpuReady; event.rung = PacingRung::GpuQueueWait;
    FramePlan_PushStageEvent(event);
}
void PublishAndFree(SampleSlot* slot) noexcept {
    const auto& sample = slot->sample;
    const int64_t publication_qpc = Qpc(); // publication, not completion's upper bound
    PushPacingReady(*slot);
    PushGpuReady(*slot);
    if(sample.before_gate && slot->present_observation.source_frame) {
        auto observation=slot->present_observation;
        observation.query_qpc=sample.timestamp.mapped_qpc;
        observation.publication_qpc=publication_qpc;
        observation.valid=sample.status==GpuReadinessStatus::Completed && sample.timestamp.status==GpuTimestampStatus::Valid;
        observation.calibration_bracket_rejected=sample.status==GpuReadinessStatus::Completed && sample.timestamp.calibration_bracket_rejected;
        // Invalid completions must survive snapshot replacement and try-lock
        // contention. The consumer observes this watermark even without payload.
        if(!observation.valid)slot->queue->observation_faults.fetch_add(1,std::memory_order_release);
        if(!slot->queue->observation_busy.test_and_set(std::memory_order_acquire)) {
            // Reused sample slots are polled by array index, not source order.
            // An older completion must never replace a newer feedback snapshot.
            if(observation.sample>slot->queue->latest_prehold.sample)
                slot->queue->latest_prehold=observation;
            slot->queue->observation_busy.clear(std::memory_order_release);
        }
    }
    slot->timestamp_slot->ownership.store(GpuTimestampOwnership::Retired, std::memory_order_release);
    slot->state.store(SampleState::Empty, std::memory_order_release);
}
void FinishScope(bool valid) noexcept {
    if (s_scope.depth != 0) { s_scope.valid = false; return; }
    if (s_scope.sample) {
        auto* slot = s_scope.sample;
        uint64_t waiting = s_scope.sample_id << 2;
        slot->finish_token.compare_exchange_strong(waiting,
            waiting | (valid ? kFinishValid : kFinishInvalid), std::memory_order_acq_rel);
        WakeWorker(); // immutable token: stale finish cannot qualify reused storage
    }
    const uint32_t depth = s_scope.depth;
    if(s_scope.queue)s_scope.queue->scope_pins.fetch_sub(1,std::memory_order_release);
    s_scope = {};
    s_scope.depth = depth;
}
void ReclaimRetiredQueues() noexcept {
    for(auto& queue:s_queues) {
        if(!queue.retired.load(std::memory_order_acquire) || queue.faulted.load() || queue.uncertain_submission.load())continue;
        if(!TryAcquireSRWLockExclusive(&s_registration))continue;
        const auto state=queue.state.load(std::memory_order_acquire);
        bool idle=state!=QueueState::Empty && state!=QueueState::Creating &&
            queue.scope_pins.load(std::memory_order_acquire)==0;
        for(const auto& sample:s_samples) {
            const auto sample_state=sample.state.load(std::memory_order_acquire);
            if(sample_state==SampleState::Preparing ||
                (sample_state==SampleState::Pending && sample.queue==&queue))idle=false;
        }
        if(idle)queue.state.store(QueueState::Reclaiming,std::memory_order_release);
        ReleaseSRWLockExclusive(&s_registration);
        if(!idle)continue;
        // Sole worker; Reclaiming rejects scopes. COM release is outside the
        // registry lock, and failures retain a non-reusable tombstone.
        if(!queue.resources_released)queue.resources_released=GpuReadinessQueue_Release(&queue);
        if(!queue.resources_released)continue;
        if(!TryAcquireSRWLockExclusive(&s_registration))continue;
        queue.next_value=1;queue.generation=0;queue.device_identity=0;queue.latest_prehold={};queue.resources_released=false;
        queue.observation_faults.store(0,std::memory_order_relaxed);
        queue.last_submitted=0;queue.calibration={};queue.wake_value=0;queue.wake_qpc=0;
        queue.notification_failed=false;
        for(auto& timestamp:queue.timestamps) {
            timestamp.retire_value=0;timestamp.ownership.store(GpuTimestampOwnership::Free,std::memory_order_release);
        }
        queue.identity.store(0,std::memory_order_release);
        queue.retired.store(false,std::memory_order_release);
        queue.state.store(QueueState::Empty,std::memory_order_release);
        ReleaseSRWLockExclusive(&s_registration);
    }
}
void Poll(bool stopping) noexcept {
    for (auto& queue : s_queues) {
        if (!stopping && !queue.retired.load() && queue.state.load(std::memory_order_acquire) == QueueState::Requested) {
            const HRESULT result = GpuReadinessQueue_Initialize(&queue);
            queue.state.store(SUCCEEDED(result) && queue.fence ? QueueState::Ready : QueueState::Failed, std::memory_order_release);
            if (FAILED(result)) LOG_WARN("GPU readiness: timestamp queue initialization failed (hr=0x%08X)", result);
        }
    }
    for (auto& slot : s_samples) {
        if (slot.state.load(std::memory_order_acquire) != SampleState::Pending) continue;
        auto& sample = slot.sample;
        const uint64_t base = sample.sample_id << 2;
        uint64_t finish = slot.finish_token.load(std::memory_order_acquire);
        const bool expired = Qpc() - sample.signal_before_qpc > s_frequency / 4;
        const bool cancelled = stopping || slot.queue->retired.load(std::memory_order_acquire) ||
            sample.settings_generation != GpuReadiness_Settings() ||
            sample.controller_generation != FrameTimeline_CurrentControllerGeneration() ||
            FrameTimeline_GetPresentSwapchainGeneration(slot.native_swapchain) != sample.swapchain_generation || !Eligible();
        if (sample.status == GpuReadinessStatus::Pending || sample.status == GpuReadinessStatus::Completed) {
            if (slot.queue->faulted.load()) sample.status = GpuReadinessStatus::NativeFault;
            else if (cancelled) sample.status = GpuReadinessStatus::Cancelled;
            else if (finish == (base | kFinishInvalid)) sample.status = GpuReadinessStatus::InvalidScope;
            else if (sample.status == GpuReadinessStatus::Pending) {
                const int64_t before = Qpc();
                const uint64_t value = GpuReadinessQueue_Completed(slot.queue);
                const int64_t after = Qpc();
                GpuReadiness_Observe(&sample, value, before, after);
                if (sample.status == GpuReadinessStatus::Completed) {
                    sample.timestamp.completion_observe_before_qpc = before;
                    if (slot.queue->wake_value == sample.fence_value)
                        sample.timestamp.notification_wake_qpc = slot.queue->wake_qpc;
                    GpuReadinessQueue_ReadTimestamp(slot.queue, slot.timestamp_slot, &sample, s_frequency);
                }
                if (slot.queue->faulted.load()) sample.status = GpuReadinessStatus::NativeFault;
            }
        }
        if (sample.status == GpuReadinessStatus::Pending || sample.status == GpuReadinessStatus::Completed) {
            if (finish == base) {
                if (!expired) continue; // GPU result may be ready before native finish
                if (!slot.finish_token.compare_exchange_strong(finish, base | kFinishClosed, std::memory_order_acq_rel))
                    continue; // finish won the expiry race; its work event wakes us
                sample.status = GpuReadinessStatus::InvalidScope;
            } else if (sample.status == GpuReadinessStatus::Pending) {
                if (!expired) continue;
                sample.status = GpuReadinessStatus::Timeout;
            }
        }
        slot.finish_token.store(base | kFinishClosed, std::memory_order_release);
        if (sample.status == GpuReadinessStatus::Completed) ++s_completions;
        PublishAndFree(&slot);
    }
    for (auto& queue : s_queues) {
        if (queue.state.load(std::memory_order_acquire) != QueueState::Ready) continue;
        if (queue.faulted.load() || queue.uncertain_submission.load()) continue;
        const uint64_t completed = GpuReadinessQueue_Completed(&queue);
        if (completed == UINT64_MAX) queue.retired.store(true, std::memory_order_release);
        GpuReadinessQueue_Reclaim(&queue, completed);
        if (stopping || completed == UINT64_MAX || queue.faulted.load() || queue.uncertain_submission.load() ||
            queue.armed_value || queue.notification_failed) continue;
        uint64_t next = UINT64_MAX;
        SampleSlot* armed_sample = nullptr;
        for (auto& slot : s_samples) {
            if (slot.state.load(std::memory_order_acquire) != SampleState::Pending || slot.queue != &queue) continue;
            const auto& sample = slot.sample;
            if (sample.signal_result >= 0 && sample.fence_value > completed && sample.fence_value < next) {
                next = sample.fence_value;
                armed_sample = &slot;
            }
        }
        // Cancelled/timed-out rows may already be published while their GPU
        // resources are still retiring. Keep those values observable as well.
        for (auto& timestamp : queue.timestamps) {
            if (timestamp.ownership.load(std::memory_order_acquire) == GpuTimestampOwnership::Retired &&
                timestamp.retire_value > completed && timestamp.retire_value < next) {
                next = timestamp.retire_value;
                armed_sample = nullptr;
            }
        }
        if (next == UINT64_MAX) continue;
        const int64_t arm_qpc = Qpc();
        const HRESULT result = GpuReadinessQueue_Arm(&queue, next);
        if (armed_sample) {
            armed_sample->sample.timestamp.notification_arm_qpc = arm_qpc;
            armed_sample->sample.timestamp.notification_result = result;
        }
        if (SUCCEEDED(result)) queue.armed_value = next;
        else {
            queue.notification_failed = true; // fallback is work-event/50ms watchdog, never busy-spin
            LOG_WARN("GPU readiness: completion notification unavailable (hr=0x%08X)", result);
        }
    }
    if(!stopping)ReclaimRetiredQueues();
}
DWORD WINAPI Worker(void*) noexcept {
    while (!s_stopping.load(std::memory_order_acquire)) {
        Poll(false);
        HANDLE handles[2 + kQueueCapacity]{s_stop_event, s_work_event};
        QueueSlot* owners[2 + kQueueCapacity]{};
        DWORD count = 2;
        for (auto& queue : s_queues) {
            if (!queue.armed_value) continue;
            handles[count] = queue.completion_event;
            owners[count++] = &queue;
        }
        const DWORD wait = WaitForMultipleObjects(count, handles, FALSE, 50);
        if (wait == WAIT_OBJECT_0) break;
        if (wait >= WAIT_OBJECT_0 + 2 && wait < WAIT_OBJECT_0 + count) {
            auto* queue = owners[wait-WAIT_OBJECT_0];
            queue->wake_value = queue->armed_value;
            queue->wake_qpc = Qpc();
            queue->armed_value = 0;
        } else if (wait == WAIT_FAILED) {
            LOG_WARN("GPU readiness: sampler wait failed; closing observation admission");
            s_stopping.store(true, std::memory_order_release);
        }
    }
    Poll(true);
    return 0;
}
}

uint64_t GpuReadiness_Dropped() noexcept { return s_dropped.load(); }
uint64_t GpuReadiness_Signals() noexcept { return s_signals.load(); }
uint64_t GpuReadiness_Completions() noexcept { return s_completions.load(); }
bool GpuReadiness_Start() noexcept {
    if (s_thread) return true;
    LARGE_INTEGER frequency{}; QueryPerformanceFrequency(&frequency); s_frequency = frequency.QuadPart;
    if (s_frequency <= 0) return false;
    s_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!s_stop_event) return false;
    s_work_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!s_work_event) { CloseHandle(s_stop_event); s_stop_event = nullptr; return false; }
    s_stopping.store(false, std::memory_order_release);
    s_thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    if (!s_thread) {
        s_stopping = true; CloseHandle(s_stop_event); s_stop_event = nullptr;
        CloseHandle(s_work_event); s_work_event = nullptr; return false;
    }
    return true;
}
bool GpuReadiness_Stop() noexcept {
    s_stopping.store(true, std::memory_order_release);
    if (!s_thread) return true;
    SetEvent(s_stop_event);
    if (WaitForSingleObject(s_thread, 5000) != WAIT_OBJECT_0) return false;
    CloseHandle(s_thread); s_thread = nullptr;
    CloseHandle(s_stop_event); s_stop_event = nullptr;
    return true; // refs survive until callback rundown, including paused native calls
}
bool GpuReadiness_ReleaseAfterRundown() noexcept {
    if (s_thread || !s_stopping.load()) return false;
    for(const auto& queue:s_queues)if(queue.scope_pins.load(std::memory_order_acquire))return false;
    for (auto& slot : s_samples) {
        if (slot.state.load() == SampleState::Preparing) {
            LOG_WARN("GPU readiness: producer still owns a preparing slot after rundown; retaining resources");
            return false;
        }
    }
    Poll(true);
    bool released = true;
    for (auto& queue : s_queues) {
        if (!queue.queue) continue;
        if (!GpuReadinessQueue_Release(&queue)) {
            released = false; // don't release a fence/queue beneath outstanding GPU work
            continue;
        }
        queue.retired.store(true); // no slot reuse in a presented addon lifetime
        queue.state.store(QueueState::Failed);
    }
    if (s_work_event) { CloseHandle(s_work_event); s_work_event = nullptr; }
    if (!released) LOG_WARN("GPU readiness: outstanding queue signals at shutdown; retaining resources, no GPU wait");
    return released;
}
void GpuReadiness_OnQueueDestroy(reshade::api::command_queue* queue) noexcept {
    if (!queue || queue->get_device()->get_api() != reshade::api::device_api::d3d12) return;
    const auto native = reinterpret_cast<ID3D12CommandQueue*>(queue->get_native());
    GpuPresent_OnQueueDestroy(reinterpret_cast<uint64_t>(native));
    AcquireSRWLockShared(&s_registration);
    for (auto& slot : s_queues) {
        if (slot.identity.load() == reinterpret_cast<uint64_t>(native)) slot.retired.store(true);
    }
    ReleaseSRWLockShared(&s_registration);
    WakeWorker();
}
void GpuReadiness_OnPresent(reshade::api::command_queue* queue, reshade::api::swapchain* swapchain) noexcept {
    if (s_scope.depth != 0) { s_scope.valid = false; return; }
    if (s_scope.queue || s_scope.sample || s_scope.valid || s_scope.calls != 0) FinishScope(false); // missing previous finish callback
    if (!queue || !swapchain || queue->get_device()->get_api() != reshade::api::device_api::d3d12 ||
        swapchain->get_device()->get_api() != reshade::api::device_api::d3d12) return;
    const uint64_t native = swapchain->get_native();
    if (native != SwapMgr_GetNativeHandle()) return;
    // GpuQueue capability discovery: runs before any H3 plan exists, so the
    // resolver can select the rung that later makes readiness required.
    if (!RuntimeLifecycle_IsStopping()) GpuPresent_ObservePresentQueue(queue->get_native(), native);
    if (!Eligible() || (!FrameTimeline_IsEnabled() && !GpuPresent_RequiresReadiness()) || s_scope.depth != 0) return;
    if(!swapchain->get_device()->get_native())return;
    s_scope.native = native;
    s_scope.generation = FrameTimeline_GetPresentSwapchainGeneration(native);
    s_scope.settings = GpuReadiness_Settings();
    s_scope.queue = FindOrRequest(reinterpret_cast<ID3D12CommandQueue*>(queue->get_native()),swapchain->get_device()->get_native());
    s_scope.valid = s_scope.queue && s_scope.generation;
}
void GpuReadiness_OnFinishPresent(reshade::api::swapchain* swapchain) noexcept {
    const bool valid = swapchain && s_scope.valid && s_scope.completed && s_scope.calls == 1 && s_scope.depth == 0 &&
        swapchain->get_native() == s_scope.native && s_scope.settings == GpuReadiness_Settings() && Eligible() &&
        FrameTimeline_GetPresentSwapchainGeneration(s_scope.native) == s_scope.generation;
    FinishScope(valid);
}
GpuReadinessCall::GpuReadinessCall(const PresentTimelineSpan* span, bool before_gate) noexcept {
    if (!GpuReadiness_IsEnabled() && !GpuPresent_RequiresReadiness() && !s_scope.valid) return;
    entered_ = true;
    if (s_scope.depth++ != 0 || ++s_scope.calls != 1) { s_scope.valid = false; return; }
    if (!span || !s_scope.valid || !Eligible() || span->attempt_index != 0 || (span->present_flags & (1u|8u)) ||
        span->native_swapchain != s_scope.native || span->key.swapchain_generation != s_scope.generation ||
        s_scope.settings != GpuReadiness_Settings() || s_scope.queue->retired.load() || s_scope.queue->faulted.load() ||
        s_scope.queue->uncertain_submission.load()) { s_scope.valid = false; return; }
    QueueSlot* queue = s_scope.queue;
    if (queue->signal_busy.test_and_set(std::memory_order_acquire)) { ++s_dropped; return; }
    SampleSlot* slot = nullptr;
    for (auto& candidate : s_samples) {
        auto empty = SampleState::Empty;
        if (candidate.state.compare_exchange_strong(empty, SampleState::Preparing, std::memory_order_acq_rel)) {
            slot = &candidate;
            break;
        }
    }
    if (!slot) { ++s_dropped; queue->signal_busy.clear(std::memory_order_release); return; }
    slot->timestamp_slot = GpuReadinessQueue_Acquire(queue);
    if (!slot->timestamp_slot) {
        ++s_dropped; slot->state.store(SampleState::Empty, std::memory_order_release);
        queue->signal_busy.clear(std::memory_order_release); return;
    }
    // Without runtime demand, sample only an attempt of the current capture.
    const bool runtime=GpuPresent_RequiresReadiness();
    uint32_t capture_epoch=0;
    if(span->capture_epoch) {
        const auto capture=FrameTimeline_CurrentCaptureState();
        if(capture.enabled && capture.epoch==span->capture_epoch)capture_epoch=span->capture_epoch;
    }
    if (!runtime && !capture_epoch) {
        slot->timestamp_slot->ownership.store(GpuTimestampOwnership::Free, std::memory_order_release);
        slot->state.store(SampleState::Empty, std::memory_order_release);
        queue->signal_busy.clear(std::memory_order_release); return;
    }
    slot->queue = queue;
    slot->native_swapchain = span->native_swapchain;
    auto& sample = slot->sample;
    sample = {};
    sample.sample_id = s_sample_sequence.fetch_add(1);
    if (sample.sample_id >= (UINT64_MAX >> 2) || queue->next_value == UINT64_MAX) {
        ++s_dropped; queue->retired.store(true);
        slot->timestamp_slot->ownership.store(GpuTimestampOwnership::Free, std::memory_order_release);
        slot->state.store(SampleState::Empty, std::memory_order_release);
        queue->signal_busy.clear(std::memory_order_release); return;
    }
    sample.queue_generation = queue->generation;
    sample.native_queue = reinterpret_cast<uint64_t>(queue->queue);
    sample.swapchain_generation = span->key.swapchain_generation;
    sample.present_sequence = span->key.present_sequence;
    sample.thread_id = GetCurrentThreadId();
    sample.controller_generation = FrameTimeline_CurrentControllerGeneration();
    sample.settings_generation = s_scope.settings;
    sample.fence_value = queue->next_value++;
    sample.before_gate=before_gate;
    slot->present_observation={};
    slot->source_key={};
    const auto& trace = span->native_gate;
    if (trace.mode == NativeGateMode::Native &&
        trace.status == NativeGateStatus::Native &&
        trace.source_frame_id && trace.producer_epoch &&
        trace.controller_generation == sample.controller_generation &&
        trace.swapchain_generation == sample.swapchain_generation &&
        trace.prediction_epoch == NativeGate_PredictionEpoch()) {
        slot->source_key = {PacingIdentityDomain::NvApiFrame, trace.producer_epoch, trace.source_frame_id};
        if(before_gate) {
            auto& observation=slot->present_observation;
            observation.identity={ReadyPresent_GetConfig().word,sample.settings_generation,trace.prediction_epoch,
                sample.swapchain_generation,sample.queue_generation,trace.producer_epoch,trace.controller_generation,
                runtime ? 0 : capture_epoch,sample.thread_id,runtime};
            observation.source_frame=trace.source_frame_id;observation.sample=sample.sample_id;
            observation.present_sequence=sample.present_sequence;observation.frequency=s_frequency;
        }
    }
    GpuReadinessQueue_CopyCalibration(queue, &sample.timestamp.start);
    sample.signal_result = GpuReadinessQueue_Submit(queue, slot->timestamp_slot, &sample);
    slot->present_observation.submit_qpc=sample.timestamp.submit_before_qpc;
    sample.completion_lower_qpc = sample.signal_before_qpc;
    sample.status = SUCCEEDED(sample.signal_result) ? GpuReadinessStatus::Pending : GpuReadinessStatus::SignalFailed;
    if (queue->faulted.load()) sample.status = GpuReadinessStatus::NativeFault;
    if (SUCCEEDED(sample.signal_result)) ++s_signals;
    if(before_gate && SUCCEEDED(sample.signal_result) && !queue->faulted.load() && slot->present_observation.source_frame) {
        queue_=queue;fence_value_=sample.fence_value;identity_=slot->present_observation.identity;native_swapchain_=span->native_swapchain;
    }
    queue->signal_busy.clear(std::memory_order_release);
    s_scope.sample = slot;
    s_scope.sample_id = sample.sample_id;
    slot->finish_token.store(sample.sample_id << 2, std::memory_order_release);
    slot->state.store(SampleState::Pending, std::memory_order_release);
    WakeWorker();
}
GpuReadinessCall::~GpuReadinessCall() {
    if (entered_ && s_scope.depth) --s_scope.depth;
}
void GpuReadinessCall::Complete(bool success, bool retried) noexcept {
    if (!entered_) return;
    s_scope.completed = success && !retried;
    if (!s_scope.completed) s_scope.valid = false;
}
bool GpuReadinessCall::GetReadyPresentContext(ReadyPresentContext* context,ReadyPresentObservation* observation) const noexcept {
    if(!queue_ || !context || !observation)return false;
    context->identity=identity_;
    context->latest_snapshot=true;
    auto* queue=static_cast<QueueSlot*>(queue_);
    if(!queue->observation_busy.test_and_set(std::memory_order_acquire)) {
        *observation=queue->latest_prehold;
        queue->observation_busy.clear(std::memory_order_release);
    }
    context->snapshot_faults=queue->observation_faults.load(std::memory_order_acquire);
    bool ready=false;return ProbeReady(&ready);
}
bool GpuReadinessCall::ProbeReady(bool* ready) const noexcept {
    if(!ready)return false;
    *ready=false;
    if(!queue_ || !Eligible() || !s_scope.valid || s_scope.depth!=1 || s_scope.calls!=1 ||
        identity_.config!=ReadyPresent_GetConfig().word || identity_.observer!=GpuReadiness_Settings() ||
        identity_.prediction!=NativeGate_PredictionEpoch() || identity_.controller!=FrameTimeline_CurrentControllerGeneration() ||
        identity_.producer!=FrameTimeline_CurrentProducerEpoch(FrameIdentityDomain::NvApiFrame) ||
        identity_.swapchain!=FrameTimeline_GetPresentSwapchainGeneration(native_swapchain_))return false;
    const auto capture=FrameTimeline_CurrentCaptureState();
    if(identity_.runtime) {
        if(!GpuPresent_RequiresReadiness() || identity_.capture!=0)return false;
    } else if(!capture.enabled || capture.epoch!=identity_.capture)return false;
    auto* queue=static_cast<QueueSlot*>(queue_);
    if(queue->retired.load() || queue->faulted.load() || queue->uncertain_submission.load())return false;
    const auto completed=GpuReadinessQueue_Completed(queue);
    if(completed==UINT64_MAX || queue->faulted.load())return false;
    *ready=completed>=fence_value_;
    return true;
}
uint64_t GpuReadinessCall::NativeQueue() const noexcept {
    return queue_?reinterpret_cast<uint64_t>(static_cast<QueueSlot*>(queue_)->queue):0;
}