#include "fg_present_order.h"
#if defined(_WIN64) && defined(RELIMITER_FG_PRESENT_ORDER)
#include "fg_present_order_rule.h"
#include "fg_break_window_rule.h"
#include "fg_flip_gate_rule.h"
#include "streamline_gpu_handoff.h"
#include "gpu_hold_order.h"
#include "gpu_queue_probe.h"
#include "runtime_lifecycle.h"
#include "logger.h"
#include "hooks.h"
#include <d3d12.h>
#include "streamline_queue_hooks.h"
#include <algorithm>

// See fg_present_order.h. Threads: the game's Present thread enqueues (AfterWaitActive), the thread that submits the
// render queue's work enqueues the breaks (BeforeExecuteActive), DLSS-G's presenting thread and this unit's watchdog
// release (ReleaseDue), the installer's worker withdraws the binding when frame generation stops (Service), the thread
// that destroys the queue and the shutdown let go. With the trailing switch DLSS-G's presenting thread also enqueues
// the unit's Signal behind its first submission after a Present (AfterExecuteActive), and the unit's thread makes the
// release that Signal's completion causes (ReleaseTrailed).
// `lock` shared: the fences and the events are read; exclusive: they are created or released. `release_lock`
// (always taken inside `lock`): one releaser at a time, the ordering rule's state, and the trailing rule's with what
// waits for the presenting thread's anchor. `window_lock` (alone or inside `lock`, never around it): the breaks'
// frames and placement. The Present's return and the detours only try the locks, but for the anchor's publication and
// rollback, which wait for `release_lock` (held for a few loads and stores and, by a releaser, one CPU Signal of the
// order fence); the unit's own thread waits for them, and so does the watchdog at its deadline with the trailing key
// on. No path holds `release_lock` or `window_lock` across a
// blocking call or a log line; the anchor's native Signal and its event registration run under the shared `lock`, as
// the Wait's do.
namespace fg_present_order {
using Rule = FgPresentOrderRule;
constexpr auto relaxed = std::memory_order_relaxed;
SRWLOCK lock = SRWLOCK_INIT;
SRWLOCK release_lock = SRWLOCK_INIT;
ID3D12CommandQueue* bound = nullptr;          // identity of the render queue the fences belong to (never dereferenced)
ID3D12Fence* marker = nullptr;                // completes when the render queue has passed DLSS-G's Wait
ID3D12Fence* order = nullptr;                 // CPU-signalled: lets the render queue go on
Rule rule;                                    // under release_lock
ID3D12Fence* trailed = nullptr;               // signalled only by queue Signals of one presenting queue, rising
const void* bound_device = nullptr;           // identity of the render queue's device (never dereferenced)
// ── Trailing release (developer key `fg_present_order_trailing`, fg_present_order.h) ──
// Under release_lock unless atomic. A Present's return leaves a record that the presenting thread's first outermost
// submission after it (the anchor) answers: while the rule observes, whether the anchor came before the marker's
// deadline, on a queue that suits; while it defers, the remembered release, to which the anchor adds a Signal of
// `trailed` on its queue.
using TrailRule = FgPresentOrderTrailingRule;
TrailRule trailing_rule;
struct Trail {
    DWORD thread = 0;       // the thread whose Present returned: its first submission is the anchor (0: none is awaited)
    int64_t since = 0;      // QPC at that return
    uint64_t value = 0;     // the marker the Present released (observing) or whose release is deferred
    bool deferred = false;  // the release was not made at the return: it waits for the anchor's Signal or the watchdog
    bool enqueued = false;  // the unit's Signal of `trailed` with `value` has gone on the anchor's queue (native call done)
};
Trail trail;
uint64_t trailed_value = 0;                   // the newest value a Signal of `trailed` was taken for (they rise)
// What the watchdog applies to the newest marker it woke for: the marker and its deadline (QPC). An anchor is judged
// against the deadline of its Present's marker.
uint64_t deadline_marker = 0;
int64_t deadline_qpc = 0;
std::atomic<DWORD> trail_thread{0};           // trail.thread, for the anchor hook's first test without a lock
std::atomic<bool> trail_signalled{false};     // trail.enqueued while a release waits for it: the unit's thread looks here
std::atomic<void*> trail_queue{nullptr};      // the queue of the block's anchors: the only one `trailed` is signalled on
std::atomic<bool> trail_enqueuing{false};     // one enqueuer at a time: a value is taken and enqueued as one step
std::atomic<bool> trailing_engaged{false};    // the key was seen on since the evidence last started
std::atomic<bool> trailing_tried{false};      // `trailed` and its event were made (or could not be) for this binding
std::atomic<uint8_t> trailing_shown{0};       // the trailing rule's state (TrailRule::State), for the report
std::atomic<uint64_t> by_trailing{0}, left_missing{0}, left_late{0};
std::atomic<uint64_t> defer_count{0}, defer_sum_us{0}, defer_max_us{0};   // Present's return to release, deferred ones
std::atomic<unsigned> trailing_said{0};       // per binding: the first few changes at info level
constexpr unsigned kTrailingLines = 4;
std::atomic<bool> issuing{false};
std::atomic<uint64_t> by_present{0}, by_watchdog{0}, enqueue_failures{0}, release_failures{0};
HANDLE thread = nullptr, stop_event = nullptr, marker_event = nullptr, released_event = nullptr;
std::atomic<HANDLE> trailed_event{nullptr};   // completion of `trailed`; made with it, closed at shutdown
// The watchdog bounds every order Wait: none is enqueued unless it runs. Set when its thread is created, cleared by
// the thread on its way out.
std::atomic<bool> watchdog_running{false};
int64_t frequency = 0;
// ── Present ahead (developer experiment, live key `fg_present_ahead`; the section further down) ──
using AheadRegisterFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Fence*, UINT64, HANDLE);
std::atomic<AheadRegisterFn> ahead_native{nullptr};   // the runtime's ID3D12Fence::SetEventOnCompletion behind the detour
std::atomic<void*> ahead_target{nullptr};             // its address, read from the unit's own order fence at a bind
std::atomic<bool> ahead_installed{false}, ahead_refused{false};
std::atomic<HANDLE> ahead_event{nullptr};             // the presenting thread's hold at its Present's return
std::atomic<DWORD> ahead_thread{0};                   // the thread whose Present last released a marker
std::atomic<uint64_t> ahead_matched{0}, ahead_other{0}, ahead_woke{0}, ahead_again{0};
// A generated frame's Present that returned before frame generation was done leaves its marker here: the unit's thread
// releases the render queue when that marker completes (0: none).
std::atomic<uint64_t> ahead_presented{0};
// The unit's thread stamps each marker's completion with its wake for it: the real frame's Present is timed from it.
std::atomic<uint32_t> ahead_stamp_seq{0};
std::atomic<uint64_t> ahead_stamp_value{0};
std::atomic<int64_t> ahead_stamp_qpc{0};
std::atomic<uint64_t> ahead_early{0}, ahead_behind{0}, ahead_at_marker{0};
std::atomic<uint64_t> ahead_held{0}, ahead_held_us{0}, ahead_held_max_us{0}, ahead_unheld{0}, ahead_gave_up{0};
std::atomic<uint64_t> ahead_spacing_us{0}, ahead_lead_us{0};
std::atomic<unsigned> ahead_lines{0};
void AheadAtMarker(int64_t woke) noexcept;
// What the installer's worker last said (FgPresentOrder_Service): a queue is bound only while frame generation
// presents, and the binding stands for one multiplier.
std::atomic<bool> presenting{false};
std::atomic<int> multiplier{0};
std::atomic<bool> bound_live{false};          // mirrors `bound != nullptr` for the service pass
std::atomic<int> bound_multiplier{0};         // the multiplier the binding was made at (written under `lock`)
// A render queue whose fences could not be created: not tried again until it is destroyed or frame generation
// starts again.
std::atomic<void*> failed_queue{nullptr};

// The markers' interval: the frame time the watchdog's bound and the breaks' budget are shares of.
std::atomic<int64_t> frame_qpc{0};            // when the newest marker was enqueued
std::atomic<unsigned> frame_us{0};            // the markers' interval, smoothed over eight

// ── Breaks in the render queue's committed work (developer live key `fg_render_break_every`, default 1 = on) ──
// The GPU is handed the render queue's work up to twelve submissions ahead, and a Present of DLSS-G's own thread that
// arrives meanwhile gets the GPU 1 to 2 ms later at the median, 2 to 5 ms at the 90th percentile, although its queue
// has the higher priority; where the render queue has nothing committed it runs at once. In front of a
// submission the render queue therefore signals `breaker` and waits for that value: the GPU is handed nothing past
// that point until it has reached it, releases the Wait itself there, and takes a waiting Present first. No thread
// and no watchdog: each Wait follows its own Signal in the same queue. One submitter at a time enqueues a pair, so
// the values rise in queue order (a pair out of order would leave a Wait above the fence's value).
ID3D12Fence* breaker = nullptr;
std::atomic<uint64_t> break_value{0}, breaks{0};
std::atomic<bool> breaking{false};
// The breaks go only where frame generation's later Presents arrive (fg_break_window_rule.h). A frame is the work
// submitted behind one marker; its first Present (the generated frame's) releases the marker, every later one (2x:
// the real frame's; 3x and 4x: more generated frames first) arrives amid that work and has a window of submissions
// of its own. When such a Present has returned, the newest break the GPU has reached says at which submission it
// was. Developer live key `fg_render_break_window`: -1 (default) the rule's own widths, 0 every n-th submission of
// the whole frame, above 0 that half-width. A frame's keys and windows are fixed when its marker is enqueued, and
// everything about a frame is read and written under `window_lock`, which is held for a few loads and stores only.
using BreakRule = FgBreakWindowRule;
constexpr unsigned kBreakPoints = 160;         // a frame's breaks that are kept; later ones still break, unrecorded
struct BreakFrame {
    uint64_t frame = 0;                        // the marker value the frame's work follows
    unsigned count = 0;                        // its submissions so far
    unsigned every = 1, fixed = 0;             // the keys at its start
    bool whole = false;                        // every n-th submission of the whole frame, no windows
    unsigned windows = 0;
    BreakRule::Window window[FgBreakOutputs::kMost];
    unsigned later = 0;                        // Presents after its first that have returned
    unsigned point_count = 0;
    BreakRule::Point points[kBreakPoints];     // its breaks in the order they were enqueued
};
SRWLOCK window_lock = SRWLOCK_INIT;
BreakRule window_rules[FgBreakOutputs::kMost]; // under window_lock, one per later output
FgBreakOutputs outputs;                        // under window_lock
BreakFrame window_frames[4];                   // under window_lock, by marker value
uint64_t window_current = 0;                   // under window_lock: the frame being submitted, 0 while none
uint64_t window_counted = 0;                   // under window_lock: the frame whose later Presents are counted
unsigned window_count = 0;                     // under window_lock: the submissions of the newest finished frame
std::atomic<bool> window_live{false};          // a frame is open: the release side keeps its books
std::atomic<unsigned> break_centre{0}, break_half{0}, break_windows{0};  // for the report (the first window)
std::atomic<uint64_t> window_before{0}, window_inside{0}, window_after{0};

int64_t Qpc() noexcept { LARGE_INTEGER value{}; QueryPerformanceCounter(&value); return value.QuadPart; }
HRESULT QueueCall(FgPresentOrder_FenceFn call, ID3D12CommandQueue* queue, ID3D12Fence* fence, uint64_t value) noexcept {
    __try { return call(queue, fence, value); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return E_FAIL; }
}
HRESULT CpuSignal(ID3D12Fence* fence, uint64_t value) noexcept {
    __try { return fence->Signal(value); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return E_FAIL; }
}
// UINT64_MAX: the device is removed (the runtime has then released every queue wait) or the read faulted.
uint64_t Completed(ID3D12Fence* fence) noexcept {
    __try { return fence->GetCompletedValue(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return UINT64_MAX; }
}
HRESULT RegisterEvent(ID3D12Fence* fence, uint64_t value, HANDLE event) noexcept {
    __try {
        // The unit's own registrations go past the present-ahead detour (they are no evidence about DLSS-G).
        if (const AheadRegisterFn native = ahead_native.load(std::memory_order_acquire)) return native(fence, value, event);
        return fence->SetEventOnCompletion(value, event);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return E_FAIL; }
}
// Lets the order Waits up to `value` go. True when nothing waits below it any more: the Signal went through, or the
// device is gone and the runtime released the queue's waits itself.
bool ReleaseOrder(uint64_t value) noexcept {
    if (SUCCEEDED(CpuSignal(order, value)) || Completed(order) == UINT64_MAX) return true;
    release_failures.fetch_add(1, relaxed);
    return false;
}

// `lock` held (shared). A marker was enqueued: the work submitted from here on is the frame `value`. Its keys and
// windows are fixed here, so its submitter and its Presents read one consistent frame.
void BreakFrameStart(uint64_t value) noexcept {
    const int every = g_fg_render_break_every.load(relaxed);
    if (every <= 0) {
        if (window_live.exchange(false, relaxed)) {
            AcquireSRWLockExclusive(&window_lock);
            window_current = window_counted = 0;
            window_count = 0;
            ReleaseSRWLockExclusive(&window_lock);
        }
        return;
    }
    const int setting = g_fg_render_break_window.load(relaxed);
    const unsigned interval = frame_us.load(relaxed);
    AcquireSRWLockExclusive(&window_lock);
    if (window_current) {
        const BreakFrame& ended = window_frames[window_current & 3];
        if (ended.frame == window_current && ended.count) window_count = ended.count;
    }
    BreakFrame& frame = window_frames[value & 3];
    frame.frame = value;
    frame.count = 0;
    frame.every = static_cast<unsigned>(every);
    frame.fixed = setting > 0 ? static_cast<unsigned>(setting) : 0;
    frame.whole = setting == 0;
    frame.windows = 0;
    frame.later = 0;
    frame.point_count = 0;
    if (!frame.whole) {
        frame.windows = outputs.windows;
        for (unsigned k = 0; k < frame.windows; ++k)
            frame.window[k] = window_rules[k].Start(window_count, interval, frame.windows, frame.fixed,
                FgBreakOutputs::Place(k, frame.windows));
    }
    window_current = value;
    ReleaseSRWLockExclusive(&window_lock);
    window_live.store(true, relaxed);
}
// A marker was released. By a Present (`released` its value): that was the frame's first Present, every later one
// until the next release is counted for it, and the frame before it is over. By the watchdog (0): the frame's first
// Present is still to come, so its Presents cannot be told apart and are not counted.
void BreakFrameReleased(uint64_t released) noexcept {
    AcquireSRWLockExclusive(&window_lock);
    if (window_counted) {
        const BreakFrame& before = window_frames[window_counted & 3];
        if (before.frame == window_counted) outputs.OnFrame(before.later);
    }
    window_counted = released;
    ReleaseSRWLockExclusive(&window_lock);
}
// `lock` held (shared). A Present after the first of the frame whose work follows marker `frame` has returned: it
// is that frame's next output, and where the GPU was in its window moves that window.
void BreakLaterPresentShared(uint64_t frame) noexcept {
    if (!breaker) return;
    const uint64_t completed = Completed(breaker);
    const unsigned interval = frame_us.load(relaxed);
    AcquireSRWLockExclusive(&window_lock);
    BreakFrame& record = window_frames[frame & 3];
    if (record.frame == frame) {
        const unsigned output = record.later++;
        BreakRule::Found found = BreakRule::Found::Before;
        unsigned at = 0;
        if (completed != UINT64_MAX && output < record.windows &&
            BreakRule::Locate(record.window[output], record.every, record.points, record.point_count, completed,
                &found, &at)) {
            window_rules[output].Observe(record.window[output], found, at, window_count, interval, record.windows,
                record.fixed);
            (found == BreakRule::Found::Before ? window_before
                : found == BreakRule::Found::After ? window_after : window_inside).fetch_add(1, relaxed);
            if (output == 0) {
                break_centre.store(window_rules[0].centre, relaxed);
                break_half.store(window_rules[0].Half(interval, record.windows, record.fixed), relaxed);
                break_windows.store(record.windows, relaxed);
            }
        }
    }
    ReleaseSRWLockExclusive(&window_lock);
}
// A failed bind: 1 events or threads, 2 GetDevice, 3 to 5 the marker, order and break fences, 6 a structured
// exception. Stage 0 with a false result: the unit may not bind now (off, stopping, or no frame generation). The
// trailing fence is made apart (MakeTrailingLocked) and its failure fails nothing. `device_id`: the device the fences are on
// (an identity to compare; no reference is kept, the queue holds one).
struct BindFailure { int stage = 0; long result = 0; };
bool CreateFences(ID3D12CommandQueue* queue, ID3D12Fence** fences, int count, BindFailure* failure,
    const void** device_id) noexcept {
    ID3D12Device* device = nullptr;
    bool created = false;
    __try {
        HRESULT result = queue->GetDevice(IID_PPV_ARGS(&device));
        if (FAILED(result) || !device) *failure = {2, result};
        else {
            created = true;
            *device_id = device;
            for (int i = 0; i < count && created; ++i) {
                result = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fences[i]));
                if (FAILED(result) || !fences[i]) { *failure = {3 + i, result}; created = false; }
            }
            device->Release();
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { created = false; *failure = {6, 0}; }
    return created;
}
// The identity of the device `queue` belongs to (a value to compare, no reference kept: the queue is alive, it is the
// object a detour is running on); null when it cannot be read.
const void* DeviceOf(ID3D12CommandQueue* queue) noexcept {
    ID3D12Device* device = nullptr;
    __try {
        if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device))) || !device) return nullptr;
        device->Release();
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    return device;
}
void ReleaseFence(ID3D12Fence** fence) noexcept {
    __try { if (*fence) (*fence)->Release(); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    *fence = nullptr;
}
// release_lock held. Nothing is awaited from the presenting thread any more.
void ClearTrail() noexcept {
    trail = Trail{};
    trail_thread.store(0, relaxed);
    trail_signalled.store(false, relaxed);
}
// release_lock held. The trailing evidence starts over: a new binding, the presenting queue gone, the key turned off.
void ResetTrailing() noexcept {
    trailing_rule.Reset();
    ClearTrail();
    trail_queue.store(nullptr, relaxed);
    trailing_shown.store(0, relaxed);
}
// release_lock held. The ordering rule changed state, a new stretch of ordering: what Observe and Defer have counted
// starts over with it, but a Stopped verdict stands (it is about the presenting queue, and keeps the queue it names).
void RestartTrailing() noexcept {
    ClearTrail();
    trailing_rule.OnOrderingChange();
    if (!trailing_rule.IsStopped()) trail_queue.store(nullptr, relaxed);
    trailing_shown.store(static_cast<uint8_t>(trailing_rule.state), relaxed);
}
// ── Flip gate (fg_present_ahead 3 and 4; see "Present ahead" further down) ──
// A Wait of this unit's own fence goes on DLSS-G's presenting queue directly behind DXGI's Signal inside the native
// Present, in front of the Signal each flip waits for (GateArm, further down); the clock thread signals it from the
// CPU at the flip's time (fg_flip_gate_rule.h). A Signal of a second fence goes directly in front of that Wait: it
// completes when the queue has done the frame's own work, which no flip can come before and which, for a flip whose
// gate was open by then, is the time of the flip. It is not how long the frame has waited: that work stands behind
// the gate before it, and the hold's limit counts from frame generation's completion (the marker stamps below). The
// flip follows the Wait's release by 0.14 ms (packet traces of 2026-11-16 and 17, N.160, N.163). The fences are made
// on the render queue's device; a Wait goes only on a queue of that device; every value enqueued is signalled by the
// clock thread no later than kGateMostMs after its enqueue, and all of them at a drop.
ID3D12Fence* gate = nullptr;                   // under `lock`: made exclusive, used shared
ID3D12Fence* gate_ready = nullptr;             // under `lock`, as `gate`: the readiness stamps, the same values
std::atomic<bool> gate_tried{false};           // the fences were made (or could not be) for this binding
std::atomic<uint64_t> gate_issued{0};          // the newest value enqueued (the presenting thread, `lock` shared)
std::atomic<uint32_t> gate_epoch{0};           // counts the drops: a value belongs to the fences of one epoch
struct GateEntry {
    uint64_t value = 0;                        // the fence value its Wait is for
    uint64_t marker = 0;                       // the cycle's marker: the pacer's interval for the frame goes by it
    int64_t spacing = 0;                       // the real frame's: DLSS-G's own call spacing (QPC), else 0
    int64_t enqueued = 0;                      // QPC
    bool real = false;                         // the cycle's second Present (the real frame's)
    bool stamped = false;                      // its readiness stamp went on the queue in front of the Wait
    uint8_t note = 0;                          // the real frame's: how its Present's hold ended (for the pair lines)
    uint32_t released = 0, issued = 0;         // the ordering's markers when it was armed (for the pair lines)
};
constexpr unsigned kGateRing = 16;
constexpr int64_t kGateMostMs = 60;
GateEntry gate_ring[kGateRing];                // one producer (the presenting thread), one consumer (the clock)
std::atomic<uint32_t> gate_head{0}, gate_tail{0};
// By marker value: the pacer's interval (ticks) for each marker's frame (positive where it held the frame's FG input
// to its grid, negative where it had a plan but did not hold, 0 no plan), and when frame generation completed for it
// (the unit's thread's wake for the marker).
constexpr unsigned kCycleRing = 32;
std::atomic<uint64_t> cycle_marker[kCycleRing];
std::atomic<int64_t> cycle_ticks[kCycleRing];
std::atomic<uint64_t> done_marker[kCycleRing];
std::atomic<int64_t> done_qpc[kCycleRing];
HANDLE gate_thread = nullptr;                  // under `gate_thread_lock`
SRWLOCK gate_thread_lock = SRWLOCK_INIT;       // the installer's worker makes and reaps the clock, the shutdown joins it
std::atomic<HANDLE> gate_event{nullptr};
std::atomic<bool> gate_running{false}, gate_stop{false};
std::atomic<uint64_t> gate_enqueued{0}, gate_on_target{0}, gate_immediate{0}, gate_bounded{0};
std::atomic<uint64_t> gate_planned{0}, gate_late{0}, gate_unplanned{0}, gate_wake_err_us{0};
std::atomic<int64_t> gate_lead_us{0}, gate_half_us{0};
std::atomic<uint64_t> gate_refused[4] = {};    // why a gate was not enqueued
std::atomic<uint64_t> gate_missed{0}, gate_slack_sum_us{0}, gate_slack_count{0};
std::atomic<uint64_t> gate_behind{0}, gate_front{0};   // placed behind a Signal inside the Present; in front of the call
std::atomic<uint64_t> gate_real_late{0}, gate_real_late_us{0};   // real flips whose own work came after their time
std::atomic<uint64_t> gate_pairs{0};           // flips under 4 ms behind the flip before
std::atomic<uint64_t> gate_ambiguous{0};       // generated Presents whose marker was not the oldest outstanding
std::atomic<uint64_t> gate_undated{0};         // generated frames held with no stamp of their generation's completion
std::atomic<uint64_t> gate_lag_sum_us{0}, gate_lag_count{0};   // generated flips behind generation's completion
std::atomic<uint64_t> gate_moved{0}, gate_moved_us{0};         // real flips moved forward for the next frame
std::atomic<uint64_t> gate_unstamped{0};       // gates whose readiness stamp could not be enqueued
// fg_present_ahead 4: the render queue's release at a marker is left to the clock, which makes it when the generated
// frame's own work is done. The marker whose release the clock owes (0 none), and the newest marker whose frame's
// work the clock has seen done; the two stores and loads are sequentially consistent, so one side always releases.
std::atomic<uint64_t> gate_release_marker{0}, gate_work_marker{0};
std::atomic<uint64_t> gate_handed{0}, gate_hand_us{0}, gate_kept{0};
std::atomic<unsigned> gate_pair_lines{0};
// Exclusive lock held: every gate Wait is let go, then the fences are released.
void GateDropLocked() noexcept {
    if (gate) {
        // As the ordering's fence at a drop; a Signal that failed on a device that is not removed is counted.
        if (FAILED(CpuSignal(gate, gate_issued.load(relaxed))) && Completed(gate) != UINT64_MAX)
            release_failures.fetch_add(1, relaxed);
        ReleaseFence(&gate);
    }
    ReleaseFence(&gate_ready);
    gate_epoch.fetch_add(1, relaxed);
    for (unsigned slot = 0; slot < kCycleRing; ++slot) {
        cycle_marker[slot].store(0, relaxed);
        done_marker[slot].store(0, relaxed);
    }
    gate_issued.store(0, relaxed);
    gate_release_marker.store(0);
    gate_work_marker.store(0);
    gate_head.store(gate_tail.load(relaxed), std::memory_order_release);
    gate_tried.store(false, relaxed);
}
// The game's thread, inside the proxy Present scope, as the marker `value` is enqueued: the pacer's interval for this
// frame's FG input as it noted it in the same scope (0 where it did not) goes with the marker.
void GateNoteMarker(uint64_t value) noexcept {
    int64_t cycle = 0;
    if (auto* scope = StreamlineInputScope::Current()) {
        cycle = scope->order_cycle;
        scope->order_cycle = 0;
    }
    const unsigned slot = static_cast<unsigned>(value % kCycleRing);
    cycle_marker[slot].store(0, relaxed);
    cycle_ticks[slot].store(cycle, relaxed);
    cycle_marker[slot].store(value, std::memory_order_release);
}
int64_t GateCycleOf(uint64_t value) noexcept {
    const unsigned slot = static_cast<unsigned>(value % kCycleRing);
    if (cycle_marker[slot].load(std::memory_order_acquire) != value) return 0;
    const int64_t cycle = cycle_ticks[slot].load(relaxed);
    return cycle_marker[slot].load(std::memory_order_acquire) == value ? cycle : 0;
}
// The unit's thread, at its wake for a marker: frame generation has completed for the frame of marker `value`.
void GateNoteDone(uint64_t value, int64_t qpc) noexcept {
    const unsigned slot = static_cast<unsigned>(value % kCycleRing);
    done_marker[slot].store(0, relaxed);
    done_qpc[slot].store(qpc, relaxed);
    done_marker[slot].store(value, std::memory_order_release);
}
int64_t GateDoneOf(uint64_t value) noexcept {
    const unsigned slot = static_cast<unsigned>(value % kCycleRing);
    if (done_marker[slot].load(std::memory_order_acquire) != value) return 0;
    const int64_t qpc = done_qpc[slot].load(relaxed);
    return done_marker[slot].load(std::memory_order_acquire) == value ? qpc : 0;
}
// Exclusive lock held: every wait enqueued so far is released, then the fences are let go and the rules start again.
// The runtime keeps a fence for the Signals and Waits still queued on it, and a break's Wait follows its own Signal,
// so nothing queued is stranded. A release deferred to the presenting thread's Signal is among the Waits released (its
// value is below `issued`); a Signal of `trailed` still queued completes into nothing, and nothing is left awaited.
void DropLocked() noexcept {
    AcquireSRWLockExclusive(&release_lock);
    if (order) {
        const uint64_t issued = g_fg_present_order_issued.load(std::memory_order_acquire);
        if (issued > g_fg_present_order_released.load(relaxed)) ReleaseOrder(issued);
    }
    rule.Reset();
    ResetTrailing();
    trailed_value = 0;
    GateDropLocked();
    deadline_marker = 0;
    deadline_qpc = 0;
    trail_enqueuing.store(false, relaxed);
    trailing_engaged.store(false, relaxed);
    trailing_tried.store(false, relaxed);
    trailing_said.store(0, relaxed);
    g_fg_present_order_queue.store(nullptr, std::memory_order_release);
    g_fg_present_order_real_awaited.store(0, std::memory_order_release);
    ReleaseSRWLockExclusive(&release_lock);
    ReleaseFence(&marker);
    ReleaseFence(&order);
    ReleaseFence(&breaker);
    ReleaseFence(&trailed);
    break_value.store(0, relaxed);
    break_centre.store(0, relaxed);
    break_half.store(0, relaxed);
    break_windows.store(0, relaxed);
    window_live.store(false, relaxed);
    AcquireSRWLockExclusive(&window_lock);
    for (BreakRule& placed : window_rules) placed.Reset();
    outputs.Reset();
    for (BreakFrame& record : window_frames) { record.frame = 0; record.count = 0; record.windows = 0; record.point_count = 0; }
    window_current = window_counted = 0;
    window_count = 0;
    ReleaseSRWLockExclusive(&window_lock);
    bound = nullptr;
    bound_device = nullptr;
    bound_live.store(false, relaxed);
    g_fg_present_order_released.store(0, relaxed);
    g_fg_present_order_issued.store(0, std::memory_order_release);
    frame_qpc.store(0, relaxed);
    frame_us.store(0, relaxed);
}
// Exclusive lock held. The trailing fence and its event, made once for a binding and only for a key that is on (a bind
// with the key off makes nothing more than it did): at the bind, or at the next followed Wait when the key is turned
// on after it. `device`: the render queue's. Where either cannot be made nothing is deferred and every release is made
// as without the key.
void MakeTrailingLocked(ID3D12CommandQueue* queue, const void* device) noexcept {
    trailing_tried.store(true, relaxed);
    if (trailed) return;
    if (!trailed_event.load(relaxed)) trailed_event.store(CreateEventW(nullptr, FALSE, FALSE, nullptr), relaxed);
    BindFailure ignored;
    const void* same = nullptr;
    if (!trailed_event.load(relaxed) || !CreateFences(queue, &trailed, 1, &ignored, &same) || same != device)
        ReleaseFence(&trailed);
}
DWORD WINAPI Watchdog(void*) noexcept;
// Exclusive lock held. The watchdog and its events are created once and live until shutdown; a watchdog that ended
// is replaced. Nothing is bound while the key is off, the runtime stops, or frame generation does not present: a
// bind that raced the shutdown or a key turned off finds that here.
bool BindLocked(ID3D12CommandQueue* queue, BindFailure* failure) noexcept {
    DropLocked();
    // A queue the present-first unit for held frames holds (gpu_hold_order.h) is not ordered here as well.
    if (RuntimeLifecycle_IsStopping() || !g_fg_present_order.load(relaxed) || !presenting.load(relaxed) ||
        GpuHoldOrder_Holds(queue)) return false;
    if (thread && (WaitForSingleObject(thread, 0) == WAIT_OBJECT_0 || !watchdog_running.load(relaxed))) {
        // It ended (after a shutdown that did not wait for it, or on its own) and is replaced. One that is still on
        // its way out is not waited for under the lock: the next followed Wait binds.
        if (WaitForSingleObject(thread, 0) != WAIT_OBJECT_0) return false;
        CloseHandle(thread);
        thread = nullptr;
        if (stop_event) ResetEvent(stop_event);
    }
    if (!thread) {
        LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value);
        frequency = value.QuadPart;
        if (!stop_event) stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!marker_event) marker_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!released_event) released_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (stop_event && marker_event && released_event) {
            watchdog_running.store(true, relaxed);
            thread = CreateThread(nullptr, 0, &Watchdog, nullptr, 0, nullptr);
            if (!thread) watchdog_running.store(false, relaxed);
        }
        if (!thread) { *failure = {1, static_cast<long>(GetLastError())}; return false; }
    }
    ID3D12Fence* fences[3] = {};
    const void* device = nullptr;
    if (!CreateFences(queue, fences, 3, failure, &device)) {
        for (ID3D12Fence*& fence : fences) ReleaseFence(&fence);
        return false;
    }
    marker = fences[0]; order = fences[1]; breaker = fences[2];
    // fg_present_ahead: the runtime's event registration, slot 9 of every fence of this runtime (an address only).
    if (!ahead_target.load(relaxed)) {
        __try { ahead_target.store((*reinterpret_cast<void* const* const*>(order))[9], relaxed); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    bound_device = device;
    bound = queue;
    // The trailing fence only for a key that is on: with the key off a bind makes nothing more than it did.
    if (g_fg_present_order_trailing.load(relaxed)) MakeTrailingLocked(queue, device);
    bound_multiplier.store(multiplier.load(relaxed), relaxed);
    bound_live.store(true, relaxed);
    return true;
}
// Either lock held. The queue's fences exist and a watchdog bounds its waits.
bool ReadyLocked(ID3D12CommandQueue* queue) noexcept {
    return bound == queue && marker && order && marker_event && watchdog_running.load(relaxed);
}
// Shared lock held. What the render queue has reached and not been released for: 0 when nothing is to release.
uint64_t ReleasableShared() noexcept {
    if (!marker || !order) return 0;
    uint64_t reached = Completed(marker);
    const uint64_t issued = g_fg_present_order_issued.load(std::memory_order_acquire);
    if (reached > issued) reached = issued;
    return reached > g_fg_present_order_released.load(relaxed) ? reached : 0;
}
bool Pending() noexcept {
    AcquireSRWLockShared(&lock);
    const bool pending = ReleasableShared() != 0;
    ReleaseSRWLockShared(&lock);
    return pending;
}
// The render queue and the fence of DLSS-G's Wait on it, learned inside a proxy Present scope (identity only, never
// dereferenced). Outside a scope a Wait with this identity is the same Wait. Cleared when that queue is destroyed.
// The thread that signals that fence is DLSS-G's worker: with hardware flip metering it also presents, before its
// return Signal, and its Presents are no evidence and release nothing.
std::atomic<void*> learned_queue{nullptr}, learned_fence{nullptr};
std::atomic<DWORD> worker_thread{0};
// ── Releases ──
// What made a release: a Present's return (the release of today), the watchdog's deadline, the completion of the unit's
// own Signal behind the presenting thread's anchor (Trailed), or the watchdog's deadline for a release deferred to that
// anchor: Missing (no anchor came before it) or Late (the anchor came and the Signal behind it had not completed). A
// Present did come for the last three.
enum class Origin { Present, Watchdog, Trailed, Missing, Late };
struct Finish {
    Rule::Change change = Rule::Change::None;
    TrailRule::Change trail_change = TrailRule::Change::None;
    bool was_ordering = false;
};
// The time from the Present's return to a deferred release: what the deferral costs the render queue.
void NoteDeferWait(int64_t ticks) noexcept {
    if (ticks < 0 || frequency <= 0) return;
    const uint64_t us = static_cast<uint64_t>(ticks) * 1'000'000 / static_cast<uint64_t>(frequency);
    defer_count.fetch_add(1, relaxed);
    defer_sum_us.fetch_add(us, relaxed);
    uint64_t most = defer_max_us.load(relaxed);
    while (us > most && !defer_max_us.compare_exchange_weak(most, us, relaxed)) {}
}
// release_lock held. The trailing rule changed state: what it took for its block goes with it. A rule that stops keeps
// the queue: it is the one that was stopped, and its destruction is what lets the rule observe again.
void ApplyTrailChange(TrailRule::Change change) noexcept {
    if (change == TrailRule::Change::None) return;
    if (change == TrailRule::Change::Restart || change == TrailRule::Change::Observe)
        trail_queue.store(nullptr, relaxed);
    trailing_shown.store(static_cast<uint8_t>(trailing_rule.state), relaxed);
}
// release_lock held, the order Waits through `reached` have just been released: the books, the rules, the published
// state. A Present came inside the bound for every origin but the watchdog's own, so a deferred release that the
// anchor's Signal did not make in time tells the ordering rule what a Present's release tells it: what failed is the
// trailing path, which its own rule judges, and it must not take the ordering itself down.
void FinishLocked(Origin origin, uint64_t reached, int64_t now, Finish* finish) noexcept {
    const int64_t since = trail.since;
    g_fg_present_order_released.store(reached, std::memory_order_release);
    (origin == Origin::Present ? by_present : origin == Origin::Watchdog ? by_watchdog
        : origin == Origin::Trailed ? by_trailing : origin == Origin::Missing ? left_missing : left_late)
        .fetch_add(1, relaxed);
    finish->was_ordering = g_fg_present_order_queue.load(relaxed) != nullptr;
    finish->change = origin == Origin::Watchdog ? rule.OnWatchdog() : rule.OnPresent();
    if (finish->change != Rule::Change::None) {
        g_fg_present_order_queue.store(finish->change == Rule::Change::Ordering ? bound : nullptr,
            std::memory_order_release);
        RestartTrailing();   // a new stretch of the ordering: Observe and Defer start over with it, a Stop stands
    }
    if (origin == Origin::Trailed || origin == Origin::Missing || origin == Origin::Late) {
        NoteDeferWait(now - since);
        finish->trail_change = trailing_rule.OnDeferred(origin == Origin::Trailed ? TrailRule::Outcome::Trailed
            : origin == Origin::Missing ? TrailRule::Outcome::Missing : TrailRule::Outcome::Late);
        ApplyTrailChange(finish->trail_change);
        ClearTrail();
    }
    // This Present was the frame's first: the later ones of other threads than the worker, until the next release,
    // are its other outputs, which the breaks' windows are placed by (a deferred release is the first Present's too:
    // it returned before the release was made). No window in use, or the watchdog released with no Present: nothing
    // is awaited and the next Present does no work here.
    g_fg_present_order_real_awaited.store(
        origin != Origin::Watchdog && window_live.load(relaxed) && g_fg_render_break_window.load(relaxed) != 0
            ? reached : 0,
        std::memory_order_release);
}
// release_lock held. A Present's release was made at its return and the rule observes: the thread's first outermost
// submission after it, before the marker's deadline, is what the evidence is about.
void StartObservation(int64_t now, uint64_t value) noexcept {
    trail = Trail{GetCurrentThreadId(), now, value, false, false};
    trail_thread.store(trail.thread, relaxed);
}
// Shared lock held. Whether a Signal of the unit's fence may go on `queue`: not on the render queue (the unit's own
// Wait stands in front of anything signalled there, so that Signal could not complete before the release it is to
// cause) and only on a queue of the render queue's device (a queue cannot signal a fence of another device). `device`
// is the queue's, read by the caller at this anchor, outside every lock: a native queue's destruction does not always
// reach the unit, so nothing about a queue is remembered from one anchor to the next.
bool QueueUsableLocked(ID3D12CommandQueue* queue, const void* device) noexcept {
    return queue && queue != bound && bound_device && device == bound_device;
}
// Both locks held. A release can be deferred: the fence and its event exist, the queue the block took is known, the
// unit's thread runs (it releases what the anchor's Signal does not) and the runtime does not stop.
bool DeferrableLocked() noexcept {
    return trailed && trailed_event.load(relaxed) && order && trail_queue.load(relaxed) != nullptr &&
        watchdog_running.load(relaxed) && !RuntimeLifecycle_IsStopping();
}
// What a release is to do now.
enum class Plan { Release, Defer, Hold };
// Both locks held, the key on, `reached` releasable. `origin` is Present or Watchdog on entry. Release: made now (the
// watchdog's becomes Missing or Late when it finds a deferred release due; after a Present's the caller starts the
// observation). Defer: remembered for the thread's anchor. Hold: not made; a deferred release waits for its anchor's
// Signal or the watchdog, and no Present passes it while the watchdog runs to release it (the real frame's Present is
// not its frame's first, a thread that presents more than once before it submits its work, which 3x and 4x may do,
// must not release it early, and two presenting threads must not take each other's records); `later` is then the
// frame this Present is a later output of, which the breaks are told. `change`: what the trailing rule did with the
// Present before this one, which no anchor followed.
Plan PlanTrailingLocked(Origin* origin, int64_t now, uint64_t reached, TrailRule::Change* change,
    uint64_t* later) noexcept {
    trailing_engaged.store(true, relaxed);
    const bool pending = trail.deferred && trail.value > g_fg_present_order_released.load(relaxed);
    if (trail.deferred && !pending) ClearTrail();   // released while the key was off: nothing waits for an anchor
    if (*origin == Origin::Watchdog) {
        if (pending) *origin = trail.enqueued ? Origin::Late : Origin::Missing;
        return Plan::Release;
    }
    if (g_fg_present_order_queue.load(relaxed) == nullptr) return Plan::Release;   // inert unless the unit orders
    if (pending) {
        if (watchdog_running.load(relaxed)) {
            *later = trail.value;
            return Plan::Hold;
        }
        *origin = trail.enqueued ? Origin::Late : Origin::Missing;   // nothing else can release it: this Present does
        return Plan::Release;
    }
    if (!trail.deferred && trail.thread != 0) {
        // The anchor of the Present before was never seen (none came, or a busy lock left its record).
        *change = trailing_rule.OnObserved(false);
        ApplyTrailChange(*change);
        ClearTrail();
    }
    if (trailing_rule.Defers() && DeferrableLocked()) {
        trail = Trail{GetCurrentThreadId(), now, reached, true, false};
        trail_thread.store(trail.thread, relaxed);
        return Plan::Defer;
    }
    return Plan::Release;
}
// Shared lock held, release_lock not: the breaks are told the release and the unit's thread woken.
void TellReleased(Origin origin, uint64_t reached) noexcept {
    if (window_live.load(relaxed)) BreakFrameReleased(origin == Origin::Watchdog ? 0 : reached);
    if (released_event) SetEvent(released_event);
}
// After every lock: what the rules decided is said.
void ReportMarkerChange(Rule::Change change, bool was_ordering) noexcept {
    if (change == Rule::Change::Ordering)
        LOG_INFO("FG present order: of %u markers at most %u were left to the watchdog; the render queue now waits "
                 "for the generated frame's Present", Rule::kRun, Rule::kEnter);
    else if (change == Rule::Change::Rest) {
        // Where no Present ever follows (hardware flip metering) this happens every few seconds: said once.
        static std::atomic<bool> said{false};
        if (was_ordering || !said.exchange(true, relaxed))
            LOG_INFO("FG present order: of %u markers more than %u were not followed by a Present in time; nothing "
                     "is enqueued for the next %u frames%s", Rule::kRun, Rule::kLeave, Rule::kRest,
                was_ordering ? " and the render queue no longer waits" : " (said once)");
    }
}
// One info line when the trailing evidence is met, one when it is lost and one when it stops for good, which says which
// of the two reasons it was (at most once until the evidence starts over); a binding whose evidence comes and goes
// says the first few enter and leave lines at info level and the rest at debug level.
void ReportTrailChange(TrailRule::Change change) noexcept {
    if (change == TrailRule::Change::Defer || change == TrailRule::Change::Observe) {
        const LogLevel level = trailing_said.fetch_add(1, relaxed) < kTrailingLines ? LogLevel::Info : LogLevel::Debug;
        if (change == TrailRule::Change::Defer)
            Log_Write(level, "FG present order: of %u Presents at least %u were followed by a submission of the "
                             "presenting thread before the watchdog's deadline; the render queue is now released behind "
                             "that submission",
                TrailRule::kRun, TrailRule::kEnter);
        else
            Log_Write(level, "FG present order: %u deferred releases of a block of at most %u were left to the "
                             "watchdog (no submission of the presenting thread in time, or the Signal behind it not "
                             "completed); the render queue is released when the generated frame's Present returns again",
                TrailRule::kLeave + 1, TrailRule::kRun);
    } else if (change == TrailRule::Change::StopLate) {
        LOG_INFO("FG present order: the deferral is stopped for good: %u deferred releases of a block of at most %u were "
                 "left to the watchdog with the Signal behind the presenting thread's submission not completed (its "
                 "queue seems to wait for the render queue); the render queue is released when the generated frame's "
                 "Present returns until the binding, the presenting queue or the key changes",
            TrailRule::kLeave + 1, TrailRule::kRun);
    } else if (change == TrailRule::Change::StopLeaves) {
        LOG_INFO("FG present order: the deferral is stopped for good: it was left for observing %u times in a row, with "
                 "no block of %u deferred releases that went to the watchdog at most %u times in between (the "
                 "presenting thread's submission seems to come in time only while the render queue runs); the render "
                 "queue is released when the generated frame's Present returns until the binding, the presenting queue "
                 "or the key changes",
            TrailRule::kLeaves + 1, TrailRule::kRun, TrailRule::kLeave);
    }
}
// The unit's thread, at a marker's wake: the deadline it applies to that marker (the newest the render queue has
// passed), for the evidence to judge an anchor against (TrailRule::Followed).
void PublishDeadline(int64_t deadline) noexcept {
    AcquireSRWLockShared(&lock);
    uint64_t passed = marker ? Completed(marker) : 0;
    const uint64_t issued = g_fg_present_order_issued.load(std::memory_order_acquire);
    if (passed > issued) passed = issued;
    AcquireSRWLockExclusive(&release_lock);
    deadline_marker = passed;
    deadline_qpc = deadline;
    ReleaseSRWLockExclusive(&release_lock);
    ReleaseSRWLockShared(&lock);
}
// The unit's thread: the Signal behind the presenting thread's anchor has completed (its event fired, or the deadline
// is here). The order Waits through the deferred value are released from the CPU, as the watchdog does, so the order
// fence keeps its one writer; the books and the rules are told as for any release.
void ReleaseTrailed() noexcept {
    Finish finish;
    bool released = false;
    uint64_t value = 0;
    AcquireSRWLockShared(&lock);
    AcquireSRWLockExclusive(&release_lock);
    if (trail.deferred && trail.enqueued && trail.value > g_fg_present_order_released.load(relaxed) && trailed &&
        order && Completed(trailed) >= trail.value) {
        value = trail.value;
        // A Signal that failed releases nothing: the books stay as they are and the watchdog tries again.
        if (ReleaseOrder(value)) {
            released = true;
            FinishLocked(Origin::Trailed, value, Qpc(), &finish);
        }
    }
    ReleaseSRWLockExclusive(&release_lock);
    if (released) TellReleased(Origin::Trailed, value);
    ReleaseSRWLockShared(&lock);
    ReportMarkerChange(finish.change, finish.was_ordering);
    ReportTrailChange(finish.trail_change);
}
// The developer key was turned off: the evidence of the trailing release starts over when it is turned on again. A
// release that waits for its anchor stays with the watchdog.
void EndTrailing() noexcept {
    AcquireSRWLockShared(&lock);
    AcquireSRWLockExclusive(&release_lock);
    ResetTrailing();
    trailing_engaged.store(false, relaxed);
    ReleaseSRWLockExclusive(&release_lock);
    ReleaseSRWLockShared(&lock);
}
// A release: at a native Present's return (`watchdog` false: the thread of that Present) or at the watchdog's deadline.
// `wait`: the locks are waited for, not tried. That is the watchdog's deadline with the trailing key on, where it may
// be the only releaser: a try that lost cost 4 ms and a fresh bound.
// `at_marker` (fg_present_ahead): the unit's thread makes the release of a generated frame's Present that returned
// before frame generation was done; it is that Present's release, made when the marker completes, never deferred.
void ReleaseDue(bool watchdog, bool wait, bool at_marker = false, uint64_t only = 0, uint32_t only_epoch = 0) noexcept {
    if (!watchdog && !at_marker && GetCurrentThreadId() == worker_thread.load(relaxed)) return;
    if (wait) AcquireSRWLockShared(&lock);
    else if (!TryAcquireSRWLockShared(&lock)) return;
    // `only` (the flip gate's clock, for a release handed to it): that marker and none after it, in the binding it was
    // handed in. The clock may come here late: the watchdog has then released the marker, and a later marker's
    // generated Present may not have returned.
    if (only != 0 && gate_epoch.load(relaxed) != only_epoch) {
        ReleaseSRWLockShared(&lock);
        return;
    }
    Finish finish;
    Origin origin = watchdog ? Origin::Watchdog : Origin::Present;
    bool released = false;
    uint64_t later = 0;   // a Present that returned while a release waits for its anchor: the frame it is an output of
    uint64_t reached = ReleasableShared();
    // fg_present_ahead: the thread whose Present releases a marker is DLSS-G's presenting thread.
    if (!watchdog && !at_marker && reached != 0 && g_fg_present_ahead.load(relaxed) != 0 &&
        ahead_thread.load(relaxed) != GetCurrentThreadId()) ahead_thread.store(GetCurrentThreadId(), relaxed);
    if (reached != 0) {
        bool locked = true;
        if (wait) AcquireSRWLockExclusive(&release_lock);
        else locked = TryAcquireSRWLockExclusive(&release_lock) != FALSE;
        if (locked) {
            reached = ReleasableShared();
            if (only != 0 && reached > only) reached = g_fg_present_order_released.load(relaxed) < only ? only : 0;
            Plan plan = Plan::Release;
            int64_t now = 0;
            // The developer switch (off: this load and branch are all it adds): a Present's release may be deferred to
            // the presenting thread's anchor, and the watchdog finds a release deferred and not made in time.
            if (reached != 0 && !at_marker && g_fg_present_order_trailing.load(relaxed)) {
                now = Qpc();
                plan = PlanTrailingLocked(&origin, now, reached, &finish.trail_change, &later);
            }
            // A Signal that failed releases nothing: the books stay as they are and the watchdog tries again.
            if (plan == Plan::Release && reached != 0 && ReleaseOrder(reached)) {
                released = true;
                FinishLocked(origin, reached, now, &finish);
                // A release made at the Present's return while the trailing rule observes (and a release could be
                // deferred at all): the thread's first submission after it is what the rule waits to see.
                if (now != 0 && origin == Origin::Present && trailing_rule.Observes() && trailed &&
                    trailed_event.load(relaxed) && g_fg_present_order_queue.load(relaxed) != nullptr)
                    StartObservation(now, reached);
            }
            ReleaseSRWLockExclusive(&release_lock);
            if (released) TellReleased(origin, reached);
            // A Present that returns while a release waits for its anchor is still a later output of that release's
            // frame: the breaks place their window by it, as for any later Present.
            else if (later != 0 && window_live.load(relaxed) && g_fg_render_break_window.load(relaxed) != 0)
                BreakLaterPresentShared(later);
        }
    } else if (!watchdog && !at_marker) {
        if (const uint64_t awaited = g_fg_present_order_real_awaited.load(std::memory_order_acquire))
            BreakLaterPresentShared(awaited);
    }
    ReleaseSRWLockShared(&lock);
    ReportMarkerChange(finish.change, finish.was_ordering);
    ReportTrailChange(finish.trail_change);
}
// Waits for `released` (another path released) or `signalled` (the unit's Signal behind the presenting thread's anchor
// completed) until the deadline. A high-resolution timer: the default timer resolution would make a bound of a
// millisecond sixteen.
void WaitUntil(HANDLE timer, HANDLE released, HANDLE signalled, int64_t now, int64_t deadline) noexcept {
    HANDLE events[3] = {released};
    DWORD count = 1, timeout = 0;
    if (signalled) events[count++] = signalled;
    if (timer) {
        LARGE_INTEGER due{};
        due.QuadPart = -((deadline - now) * 10'000'000 / frequency + 1);
        SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
        events[count++] = timer;
        timeout = 50;
    } else {
        timeout = static_cast<DWORD>((deadline - now) * 1000 / frequency) + 1;
    }
    WaitForMultipleObjects(count, events, FALSE, timeout);
}
// The watchdog bounds the render queue's waits, so it runs at the priority of the other timing workers.
DWORD WINAPI Watchdog(void*) noexcept {
    SetThreadDescription(GetCurrentThread(), L"ReLimiter FG present order");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    HANDLE handles[2] = {stop_event, marker_event};
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    int64_t last_report = Qpc();
    uint64_t reported_present = 0, reported_watchdog = 0, reported_breaks = 0, reported_window[3] = {};
    uint64_t reported_trailing = 0, reported_missing = 0, reported_late = 0, reported_defers = 0, reported_defer_sum = 0;
    double wait_sum = 0.0, wait_max = 0.0;
    uint64_t wait_count = 0;
    uint64_t reported_ahead[10] = {};
    uint64_t reported_gate[7] = {};
    for (;;) {
        // Outstanding markers are re-checked every few milliseconds even without the marker's event (a registration
        // that failed, or a release whose Signal failed, must not leave the queue waiting).
        const bool outstanding = g_fg_present_order_issued.load(relaxed) != g_fg_present_order_released.load(relaxed);
        const DWORD woken = WaitForMultipleObjects(2, handles, FALSE, outstanding ? 4 : INFINITE);
        if (woken != WAIT_OBJECT_0 + 1 && woken != WAIT_TIMEOUT) break;
        if (woken == WAIT_TIMEOUT && !Pending()) continue;
        // The render queue has passed DLSS-G's Wait (frame generation is done): the Present has until the deadline,
        // a share of the frame time.
        const int64_t woke = Qpc();
        // fg_present_ahead: the completion is stamped for the real frame's Present, and a generated frame's Present
        // that has returned already releases the render queue now.
        if (g_fg_present_ahead.load(relaxed) >= 2 || ahead_presented.load(relaxed) != 0) AheadAtMarker(woke);
        const int64_t deadline = woke + frequency * Rule::BoundUs(frame_us.load(relaxed)) / 1'000'000;
        // With the trailing key on, the evidence judges an anchor against this deadline (TrailRule::Followed).
        if (g_fg_present_order_trailing.load(relaxed)) PublishDeadline(deadline);
        const bool waited = g_fg_present_order_queue.load(relaxed) != nullptr;
        for (;;) {
            if (!Pending()) break;
            const int64_t now = Qpc();
            if (now >= deadline) {
                // A deferred release whose Signal has completed by now is the Signal's, not the watchdog's. With the
                // trailing key on, or with present ahead (a generated frame's Present that returned early releases
                // nothing, and the flip gate's clock may owe the release), this thread may be the only releaser of
                // the marker, so its locks are waited for, not given up (a try that lost cost 4 ms and a fresh bound).
                if (trail_signalled.load(relaxed)) ReleaseTrailed();
                ReleaseDue(true, g_fg_present_order_trailing.load(relaxed) || g_fg_present_ahead.load(relaxed) != 0 ||
                    gate_release_marker.load(relaxed) != 0);
                break;
            }
            WaitUntil(timer, released_event, trailed_event.load(relaxed), now, deadline);
            if (trail_signalled.load(relaxed)) ReleaseTrailed();
        }
        const int64_t done = Qpc();
        if (waited) {
            const double wait = static_cast<double>(done - woke) * 1e6 / static_cast<double>(frequency);
            wait_sum += wait; ++wait_count;
            if (wait > wait_max) wait_max = wait;
        }
        if (wait_count && done - last_report >= frequency * 10) {
            const uint64_t present = by_present.load(relaxed);
            const uint64_t watchdog = by_watchdog.load(relaxed);
            if (g_fg_present_order_trailing.load(relaxed)) {
                const uint64_t trailing = by_trailing.load(relaxed);
                const uint64_t missing = left_missing.load(relaxed), late = left_late.load(relaxed);
                const uint64_t defers = defer_count.load(relaxed), defer_sum = defer_sum_us.load(relaxed);
                const uint64_t deferred = defers - reported_defers;
                const uint8_t shown = trailing_shown.load(relaxed);
                LOG_INFO("FG present order: in 10 s %llu markers released by a Present, %llu by the watchdog (bound %u us); "
                         "the render queue waited %.0f us on average, %.0f us at most, after frame generation was done; "
                         "trailing release %s: %llu released behind the presenting thread's first submission, %llu at "
                         "the Present's return, %llu deferred ones left to the watchdog for want of a submission in "
                         "time, %llu for want of the Signal behind it completing in time; from the Present's return "
                         "to a deferred release %.0f us on average, %.0f us at most",
                    static_cast<unsigned long long>(present - reported_present),
                    static_cast<unsigned long long>(watchdog - reported_watchdog),
                    Rule::BoundUs(frame_us.load(relaxed)), wait_sum / static_cast<double>(wait_count), wait_max,
                    shown == static_cast<uint8_t>(TrailRule::State::Defer) ? "deferring"
                        : shown == static_cast<uint8_t>(TrailRule::State::Stopped) ? "stopped" : "observing",
                    static_cast<unsigned long long>(trailing - reported_trailing),
                    static_cast<unsigned long long>(present - reported_present),
                    static_cast<unsigned long long>(missing - reported_missing),
                    static_cast<unsigned long long>(late - reported_late),
                    deferred ? static_cast<double>(defer_sum - reported_defer_sum) / static_cast<double>(deferred) : 0.0,
                    static_cast<double>(defer_max_us.exchange(0, relaxed)));
                reported_trailing = trailing; reported_missing = missing; reported_late = late;
                reported_defers = defers; reported_defer_sum = defer_sum;
            } else {
                LOG_INFO("FG present order: in 10 s %llu markers released by a Present, %llu by the watchdog (bound %u us); "
                         "the render queue waited %.0f us on average, %.0f us at most, after frame generation was done",
                    static_cast<unsigned long long>(present - reported_present),
                    static_cast<unsigned long long>(watchdog - reported_watchdog),
                    Rule::BoundUs(frame_us.load(relaxed)), wait_sum / static_cast<double>(wait_count), wait_max);
            }
            reported_present = present; reported_watchdog = watchdog;
            if (const uint64_t broke = breaks.load(relaxed); broke != reported_breaks) {
                const uint64_t before = window_before.load(relaxed);
                const uint64_t inside = window_inside.load(relaxed);
                const uint64_t after = window_after.load(relaxed);
                const unsigned windows = break_windows.load(relaxed);
                LOG_INFO("FG render breaks: %llu in 10 s; %u window(s) a frame, the first at centre %u, half-width %u "
                         "(the frame time pays for %u each); the later Presents found the GPU before / inside / "
                         "after their window %llu / %llu / %llu times",
                    static_cast<unsigned long long>(broke - reported_breaks), windows,
                    break_centre.load(relaxed), break_half.load(relaxed),
                    BreakRule::BudgetHalf(frame_us.load(relaxed), windows),
                    static_cast<unsigned long long>(before - reported_window[0]),
                    static_cast<unsigned long long>(inside - reported_window[1]),
                    static_cast<unsigned long long>(after - reported_window[2]));
                reported_breaks = broke;
                reported_window[0] = before; reported_window[1] = inside; reported_window[2] = after;
            }
            if (const int ahead_mode = g_fg_present_ahead.load(relaxed); ahead_mode != 0 || ahead_woke.load(relaxed) != reported_ahead[2]) {
                const uint64_t now_ahead[10] = {ahead_matched.load(relaxed), ahead_other.load(relaxed),
                    ahead_woke.load(relaxed), ahead_again.load(relaxed), ahead_early.load(relaxed),
                    ahead_behind.load(relaxed), ahead_at_marker.load(relaxed), ahead_held.load(relaxed),
                    ahead_unheld.load(relaxed), ahead_gave_up.load(relaxed)};
                const uint64_t held = now_ahead[7] - reported_ahead[7];
                const double per_hold = held ? 1.0 / static_cast<double>(held) : 0.0;
                LOG_INFO("FG present ahead (mode %d, detour %s): in 10 s the presenting thread registered %llu events "
                         "for a value its own queue waits for and %llu others; woken ahead %llu (registered again "
                         "%llu); its generated Present returned before frame generation was done %llu times (the "
                         "unit's thread released the render queue at the completion %llu times) and after it %llu "
                         "times; the real frame's Present was held %llu times, %.0f us on average, %.0f us at most, to "
                         "stand %.0f us behind the completion, which the wake preceded by %.0f us; not held %llu, "
                         "gave up %llu",
                    ahead_mode, ahead_installed.load(relaxed) ? "installed" : ahead_refused.load(relaxed) ? "refused" : "not installed",
                    static_cast<unsigned long long>(now_ahead[0] - reported_ahead[0]),
                    static_cast<unsigned long long>(now_ahead[1] - reported_ahead[1]),
                    static_cast<unsigned long long>(now_ahead[2] - reported_ahead[2]),
                    static_cast<unsigned long long>(now_ahead[3] - reported_ahead[3]),
                    static_cast<unsigned long long>(now_ahead[4] - reported_ahead[4]),
                    static_cast<unsigned long long>(now_ahead[6] - reported_ahead[6]),
                    static_cast<unsigned long long>(now_ahead[5] - reported_ahead[5]),
                    static_cast<unsigned long long>(held),
                    static_cast<double>(ahead_held_us.exchange(0, relaxed)) * per_hold,
                    static_cast<double>(ahead_held_max_us.exchange(0, relaxed)),
                    static_cast<double>(ahead_spacing_us.exchange(0, relaxed)) * per_hold,
                    static_cast<double>(ahead_lead_us.exchange(0, relaxed)) * per_hold,
                    static_cast<unsigned long long>(now_ahead[8] - reported_ahead[8]),
                    static_cast<unsigned long long>(now_ahead[9] - reported_ahead[9]));
                for (int i = 0; i < 10; ++i) reported_ahead[i] = now_ahead[i];
            }
            if (g_fg_present_ahead.load(relaxed) >= 3 || gate_enqueued.load(relaxed) != reported_gate[0]) {
                const uint64_t now_gate[7] = {gate_enqueued.load(relaxed), gate_planned.load(relaxed),
                    gate_unplanned.load(relaxed), gate_on_target.load(relaxed), gate_immediate.load(relaxed),
                    gate_bounded.load(relaxed), gate_wake_err_us.load(relaxed)};
                const uint64_t timed = now_gate[3] - reported_gate[3];
                const uint64_t held = gate_slack_count.exchange(0, relaxed);
                const uint64_t held_us = gate_slack_sum_us.exchange(0, relaxed);
                const uint64_t late = gate_real_late.exchange(0, relaxed);
                const uint64_t late_us = gate_real_late_us.exchange(0, relaxed);
                const uint64_t lagged = gate_lag_count.exchange(0, relaxed);
                const uint64_t lag_us = gate_lag_sum_us.exchange(0, relaxed);
                const uint64_t moved = gate_moved.exchange(0, relaxed);
                const uint64_t moved_us = gate_moved_us.exchange(0, relaxed);
                const uint64_t handed = gate_handed.exchange(0, relaxed);
                const uint64_t handed_us = gate_hand_us.exchange(0, relaxed);
                // Two lines: the logger formats into 1024 characters.
                LOG_INFO("FG flip gate: in 10 s %llu flips gated (generated frames the pacer held %llu, planned but "
                         "did not hold %llu, had no plan for %llu); released at their time %llu (wake %.0f us late "
                         "on average), at once %llu, at the bound %llu; generated flips held behind their own work "
                         "%llu, by %llu us on average; generated flips behind generation's completion by %llu us on "
                         "average (%llu held with no stamp of it); the real one half a cycle (%lld us) after a held "
                         "one; real flips whose own work came after their time %llu, by %llu us on average; real "
                         "flips moved forward for the next frame %llu, by %llu us on average; flips under 4 ms behind "
                         "the one before %llu",
                    static_cast<unsigned long long>(now_gate[0] - reported_gate[0]),
                    static_cast<unsigned long long>(now_gate[1] - reported_gate[1]),
                    static_cast<unsigned long long>(gate_late.exchange(0, relaxed)),
                    static_cast<unsigned long long>(now_gate[2] - reported_gate[2]),
                    static_cast<unsigned long long>(timed),
                    timed ? static_cast<double>(now_gate[6] - reported_gate[6]) / static_cast<double>(timed) : 0.0,
                    static_cast<unsigned long long>(now_gate[4] - reported_gate[4]),
                    static_cast<unsigned long long>(now_gate[5] - reported_gate[5]),
                    static_cast<unsigned long long>(held),
                    static_cast<unsigned long long>(held ? held_us / held : 0),
                    static_cast<unsigned long long>(lagged ? lag_us / lagged : 0),
                    static_cast<unsigned long long>(gate_undated.exchange(0, relaxed)),
                    static_cast<long long>(gate_half_us.load(relaxed)),
                    static_cast<unsigned long long>(late),
                    static_cast<unsigned long long>(late ? late_us / late : 0),
                    static_cast<unsigned long long>(moved),
                    static_cast<unsigned long long>(moved ? moved_us / moved : 0),
                    static_cast<unsigned long long>(gate_pairs.exchange(0, relaxed)));
                LOG_INFO("FG flip gate (continued): generated frames whose marker was not the oldest outstanding %llu; "
                         "no readiness stamp %llu; the render queue let go at the generated frame's work %llu (%llu us "
                         "after generation on average), at the marker with a gate closed in front %llu; placed inside "
                         "the Present %llu, in front of the call %llu, not placed %llu; refusals %llu/%llu/%llu/%llu "
                         "(key or clock, multiplier, no queue, queue or device or full)",
                    static_cast<unsigned long long>(gate_ambiguous.exchange(0, relaxed)),
                    static_cast<unsigned long long>(gate_unstamped.exchange(0, relaxed)),
                    static_cast<unsigned long long>(handed),
                    static_cast<unsigned long long>(handed ? handed_us / handed : 0),
                    static_cast<unsigned long long>(gate_kept.exchange(0, relaxed)),
                    static_cast<unsigned long long>(gate_behind.exchange(0, relaxed)),
                    static_cast<unsigned long long>(gate_front.exchange(0, relaxed)),
                    static_cast<unsigned long long>(gate_missed.exchange(0, relaxed)),
                    static_cast<unsigned long long>(gate_refused[0].exchange(0, relaxed)),
                    static_cast<unsigned long long>(gate_refused[1].exchange(0, relaxed)),
                    static_cast<unsigned long long>(gate_refused[2].exchange(0, relaxed)),
                    static_cast<unsigned long long>(gate_refused[3].exchange(0, relaxed)));
                for (int i = 0; i < 7; ++i) reported_gate[i] = now_gate[i];
            }
            wait_sum = 0.0; wait_max = 0.0; wait_count = 0; last_report = done;
        }
    }
    if (timer) CloseHandle(timer);
    // From here no order Wait is enqueued (ReadyLocked); the next bind replaces this thread.
    watchdog_running.store(false, relaxed);
    return 0;
}

// ── Present ahead (developer experiment, live key `fg_present_ahead`: 0 off, 1 observe, 2 act) ──
// What it answers. Without hardware flip metering DLSS-G's presenting thread, once the worker has submitted a
// generation, puts a Wait for that generation's completion on its own queue, registers an event for the same fence
// value and sleeps on it (7 ms in Phantom Blade Zero; packet and wait traces of 2026-11-16). Only when the GPU has
// finished generating does it record its copies and call Present, other tools' work inside that call included, and
// the ordered render queue sits that CPU time out: 0.5 to 0.9 ms a frame. With flip metering the same Presents are
// called seven to ten milliseconds before the frame's render is done and nothing waits for the thread's CPU time.
// Acting. A registration by the presenting thread for exactly the fence value its own newest queue Wait is for (so
// the GPU keeps the order by itself: whatever the thread submits stands behind that Wait) is answered by setting the
// event at once and is not handed on. The thread records and queues its work and calls the generated frame's Present
// while the GPU still renders; the work runs when generation is done. That Present returns before the unit's marker
// has completed, so it releases nothing: it leaves the marker's value, and the unit's thread, which wakes for every
// marker, makes the release at the completion. DLSS-G counts its wait for the real frame's Present from its own
// (early) wake, so that Present would come early by as much: it is held at its entry until it stands as far behind
// frame generation's completion as DLSS-G put it behind the generated frame's Present (DLSS-G's own spacing, from the
// moment the generated frame is shown at). A second registration for the same value before the Present (DLSS-G
// looking again) is handed on unchanged, so the thread then sleeps as it did. Only while the unit orders.
// Observing. Nothing is changed: the presenting thread's registrations are counted and the first few logged.
// Measured 2026-11-16, first form (the thread held at the generated Present's return instead): DLSS-G never looked
// again and nothing failed; the render queue's wait fell from 0.92 ms to 0.03 ms and the frame rate rose 6.7 fps; but
// the real frame's Present followed at once (DLSS-G's wait for it had run out during the hold), so both frames were
// shown 0.5 ms apart.
struct AheadWait { ID3D12Fence* fence = nullptr; uint64_t value = 0; int64_t qpc = 0; ID3D12CommandQueue* queue = nullptr; };
thread_local AheadWait ahead_wait;       // the calling thread's newest outermost queue Wait
// One cycle of a thread woken ahead: its next native Present is the generated frame's, the one after it the real one's.
struct AheadCycle {
    AheadWait answered;        // what the thread was woken for
    int64_t wake = 0;          // QPC at the wake
    int64_t first = 0;         // QPC at the generated frame's Present (0: not made yet)
    uint64_t value = 0;        // the marker whose completion says frame generation is done for this cycle (0: unknown)
    bool woken = false;        // the generated frame's Present has not returned yet
    bool real = false;         // it has: the next native Present is the real frame's
    bool counted = false;      // g_fg_present_ahead_woken counts this thread
};
thread_local AheadCycle ahead;
// fg_present_ahead 3: the gate a Present of a cycle woken ahead is to get. The flip waits, on the swapchain's own
// queue, for a Signal the D3D12 runtime makes on the presenting thread's work queue inside the native Present, behind
// a command list and a Signal of DXGI's own (CD3D12Device::CloseAndSubmitCommandList; packet trace, hook log and
// symbols of 2026-11-16, N.159). Neither the runtime's Signal nor the swapchain queue's Wait passes the queue hooks.
// The gate's Wait goes on the work queue directly behind DXGI's Signal, the first outermost Signal the thread makes
// inside the call: only the runtime's Signal and the flip then stand behind it. Where the thread's last Present made
// no Signal inside the call the Wait goes in front of the call, on the queue ReShade's present event names.
struct GateArm {
    bool armed = false, real = false, placed = false;
    uint64_t marker = 0;
    int64_t spacing = 0;
    unsigned signals = 0;   // outermost Signals the thread made inside this native Present
    uint8_t note = 0;       // the real frame's: how its Present's hold ended (AheadBeforePresent)
    uint32_t released = 0, issued = 0;   // the ordering's markers when it was armed
    bool open = false;      // its native Present is running; closed, the gate waits for the thread's next one (a retry)
};
thread_local GateArm gate_arm;
thread_local uint64_t gate_marker = 0;    // the marker of the thread's newest generated Present
thread_local bool gate_in_call = false;   // the thread's last Present made a Signal inside the native call
// The command queue of the swapchain this thread presents next, from ReShade's present event (in front of the native
// call): with DLSS-G its work queue.
thread_local ID3D12CommandQueue* present_queue = nullptr;
thread_local HANDLE ahead_timer = nullptr;
constexpr unsigned kAheadLines = 12;
constexpr DWORD kAheadHoldMs = 50;

void AheadEnd() noexcept {
    const bool counted = ahead.counted;
    ahead = AheadCycle{};
    if (counted) g_fg_present_ahead_woken.fetch_sub(1, relaxed);
}
// fg_present_ahead 4. The unit's thread, at a marker whose generated Present has returned: true where the flip gate's
// clock lets the render queue go instead, when the generated frame's own work in front of its gate is done. The
// render queue's burst, let go at the marker, took the GPU from that work in 13 frames of 64 s of packet traces (the
// work reached the hardware 0.01 ms after generation, the first render packet 0.04 to 0.07 ms after it, and the work
// ended 3 to 15 ms later: N.162). The clock can say only where the gate next to be let go is that frame's (its work
// then stands behind no closed gate) and carries a readiness stamp; else the release stays here. The unit's watchdog
// bounds a release the clock owes as it bounds any.
bool GateHandRelease(uint64_t value) noexcept {
    if (g_fg_present_ahead.load(relaxed) < 4 || !gate_running.load(relaxed)) return false;
    // Handed under the shared lock: a drop, which releases every marker and clears the hand-over, is before or after.
    AcquireSRWLockShared(&lock);
    const uint32_t head = gate_head.load(std::memory_order_acquire);
    const bool any = head != gate_tail.load(std::memory_order_acquire);
    const GateEntry entry = any ? gate_ring[head % kGateRing] : GateEntry{};
    const bool hand = any && !entry.real && entry.marker == value && entry.stamped;
    if (hand) gate_release_marker.store(value);
    ReleaseSRWLockShared(&lock);
    if (!hand) {
        gate_kept.fetch_add(1, relaxed);
        return false;
    }
    // The clock may have seen the work done in between: then the release is taken back, unless the clock took it.
    if (gate_work_marker.load() == value) {
        uint64_t expected = value;
        if (gate_release_marker.compare_exchange_strong(expected, 0)) return false;
    }
    return true;
}
// The clock thread: the generated frame of marker `value` (an entry of the binding `epoch`) has its own work done, or
// its gate is finished with. Lets the render queue go where the unit's thread left that to the clock: that marker
// only, and only in the binding the entry belongs to (after a drop nothing is owed).
void GateWorkSeen(uint64_t value, uint32_t epoch) noexcept {
    if (value == 0) return;
    AcquireSRWLockShared(&lock);
    bool taken = false;
    if (gate_epoch.load(relaxed) == epoch) {
        gate_work_marker.store(value);
        uint64_t expected = value;
        taken = gate_release_marker.compare_exchange_strong(expected, 0);
    }
    ReleaseSRWLockShared(&lock);
    if (!taken) return;
    if (const int64_t done = GateDoneOf(value)) {
        const int64_t now = Qpc();
        if (now > done) gate_hand_us.fetch_add(static_cast<uint64_t>((now - done) * 1'000'000 / frequency), relaxed);
    }
    gate_handed.fetch_add(1, relaxed);
    ReleaseDue(false, true, true, value, epoch);
}
// The unit's thread, at its wake for a marker.
void AheadAtMarker(int64_t woke) noexcept {
    AcquireSRWLockShared(&lock);
    uint64_t passed = marker ? Completed(marker) : 0;
    const uint64_t issued = g_fg_present_order_issued.load(std::memory_order_acquire);
    if (passed > issued) passed = issued;
    ReleaseSRWLockShared(&lock);
    if (const uint64_t before = ahead_stamp_value.load(relaxed); passed != before) {
        ahead_stamp_seq.fetch_add(1, std::memory_order_acq_rel);
        ahead_stamp_value.store(passed, relaxed);
        ahead_stamp_qpc.store(woke, relaxed);
        ahead_stamp_seq.fetch_add(1, std::memory_order_release);
        // Every marker this wake found complete (one, unless a wake was missed).
        if (passed != 0) {
            uint64_t value = before < passed && passed - before <= kCycleRing ? before + 1 : passed;
            for (; value <= passed; ++value) GateNoteDone(value, woke);
        }
    }
    if (const HANDLE event = gate_event.load(relaxed)) SetEvent(event);
    uint64_t presented = ahead_presented.load(std::memory_order_acquire);
    if (presented != 0 && presented <= passed &&
        ahead_presented.compare_exchange_strong(presented, 0, std::memory_order_acq_rel)) {
        ahead_at_marker.fetch_add(1, relaxed);
        if (!GateHandRelease(passed)) ReleaseDue(false, true, true);
    }
}
// The completion time stamped for marker `value`: 1 stamped (in *done), 0 not yet, -1 a later marker is stamped already.
int AheadStamped(uint64_t value, int64_t* done) noexcept {
    for (;;) {
        const uint32_t before = ahead_stamp_seq.load(std::memory_order_acquire);
        if (before & 1) { YieldProcessor(); continue; }
        const uint64_t stamped = ahead_stamp_value.load(relaxed);
        const int64_t qpc = ahead_stamp_qpc.load(relaxed);
        if (ahead_stamp_seq.load(std::memory_order_acquire) != before) continue;
        if (stamped < value) return 0;
        if (stamped > value) return -1;
        *done = qpc;
        return 1;
    }
}
// The calling thread sleeps until `target` (QPC), never past `limit`: a high-resolution timer to 0.4 ms before it, the
// rest on the clock.
void AheadSleepUntil(int64_t target, int64_t limit) noexcept {
    if (target > limit) target = limit;
    for (;;) {
        const int64_t left = target - Qpc();
        if (left <= 0) return;
        const int64_t us = left * 1'000'000 / frequency;
        if (us > 600) {
            if (!ahead_timer)
                ahead_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
            if (!ahead_timer) { Sleep(0); continue; }
            LARGE_INTEGER due{};
            due.QuadPart = -((us - 400) * 10);
            SetWaitableTimer(ahead_timer, &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(ahead_timer, 100);
        } else {
            YieldProcessor();
        }
    }
}

// ── Flip gate (fg_present_ahead 3) ──
// The presenting thread, for the Present `arm` describes: the readiness stamp and the gate's Wait on `queue`, in that
// order (the stamp completes when the queue has done everything in front of the gate), and the entry the clock thread
// lets the Wait go by. Only at 2x, while the clock runs, on a queue of the render queue's device.
bool GateEnqueue(ID3D12CommandQueue* queue, const GateArm& arm) noexcept {
    if (g_fg_present_ahead.load(relaxed) < 3 || !gate_running.load(std::memory_order_acquire) ||
        RuntimeLifecycle_IsStopping() || arm.marker == 0) { gate_refused[0].fetch_add(1, relaxed); return false; }
    // g_fg_multiplier counts the generated frames per real one: 1 is 2x, the only arrangement a cycle here describes.
    if (multiplier.load(relaxed) != 1) { gate_refused[1].fetch_add(1, relaxed); return false; }
    const StreamlineQueue_SignalFn native_wait = StreamlineQueue_NativeWait();
    const StreamlineQueue_SignalFn native_signal = StreamlineQueue_NativeSignal();
    if (!queue || !native_wait) { gate_refused[2].fetch_add(1, relaxed); return false; }
    const void* const device = DeviceOf(queue);
    bool done = false;
    AcquireSRWLockShared(&lock);
    const uint32_t tail = gate_tail.load(relaxed);
    if (gate && static_cast<void*>(queue) != bound && device && device == bound_device &&
        tail - gate_head.load(std::memory_order_acquire) < kGateRing) {
        GateEntry entry;
        entry.value = gate_issued.load(relaxed) + 1;
        entry.stamped = gate_ready && native_signal &&
            SUCCEEDED(QueueCall(native_signal, queue, gate_ready, entry.value));
        if (!entry.stamped) gate_unstamped.fetch_add(1, relaxed);
        // A Wait that could not be enqueued still takes its value: the clock signals it in order like the others.
        if (FAILED(QueueCall(native_wait, queue, gate, entry.value))) enqueue_failures.fetch_add(1, relaxed);
        gate_issued.store(entry.value, relaxed);
        entry.marker = arm.marker;
        entry.spacing = arm.spacing;
        entry.enqueued = Qpc();
        entry.real = arm.real;
        entry.note = arm.note;
        entry.released = arm.released;
        entry.issued = arm.issued;
        gate_ring[tail % kGateRing] = entry;
        gate_tail.store(tail + 1, std::memory_order_release);
        done = true;
    }
    ReleaseSRWLockShared(&lock);
    if (done) {
        gate_enqueued.fetch_add(1, relaxed);
        if (const HANDLE event = gate_event.load(relaxed)) SetEvent(event);
    } else {
        gate_refused[3].fetch_add(1, relaxed);
    }
    return done;
}
// The gate's Wait in front of the call, where the thread's last Present made no Signal inside the native call.
void GatePlaceInFront() noexcept {
    if (!gate_in_call && present_queue && GateEnqueue(present_queue, gate_arm)) {
        gate_arm.placed = true;
        gate_front.fetch_add(1, relaxed);
    }
}
// The presenting thread, in front of a native Present of a cycle woken ahead: the gate for that Present. Where the
// thread's last Present made a Signal inside the native call the Wait is left to that Signal's return
// (FgPresentOrder_AfterSignalActive); else it goes in front of the call.
void GateArmFor(bool real, uint64_t marker_value, int64_t spacing, uint8_t note) noexcept {
    // An armed gate counts with the cycles woken ahead, so the Present's return accounts it whatever the key is then.
    if (!gate_arm.armed) g_fg_present_ahead_woken.fetch_add(1, relaxed);
    gate_arm = GateArm{true, real, false, marker_value, spacing, 0, note,
        static_cast<uint32_t>(g_fg_present_order_released.load(relaxed)),
        static_cast<uint32_t>(g_fg_present_order_issued.load(relaxed))};
    gate_arm.open = true;
    GatePlaceInFront();
}
// The presenting thread, in front of a native Present for which nothing is armed anew: where the thread's Present
// before failed with its gate armed and not placed, this call is that frame's again and gets that gate, with the
// frame's marker as it was.
void GateRetry() noexcept {
    if (!gate_arm.armed || gate_arm.open) return;
    gate_arm.open = true;
    gate_arm.signals = 0;
    GatePlaceInFront();
}
// The presenting thread, at the return of a native Present a gate was armed for, whatever the call returned and
// whatever the key is by now. A call that presented, or whose gate went on the queue, ends the arm: a gate armed and
// not placed is counted, and whether the thread made a Signal inside the call decides where the next Present's gate
// goes. A call that failed with its gate not placed leaves the gate armed and closed for the thread's next native
// Present (GateRetry); closed, no Signal outside a Present gets it (FgPresentOrder_AfterSignalActive), and it still
// counts, so that Present's entry and return come here.
void GateAfterPresent(bool presented) noexcept {
    if (!gate_arm.armed) return;
    if (!presented && !gate_arm.placed) {
        gate_arm.open = false;
        return;
    }
    if (!gate_arm.placed) gate_missed.fetch_add(1, relaxed);
    if (presented) gate_in_call = gate_arm.signals != 0;
    gate_arm = GateArm{};
    g_fg_present_ahead_woken.fetch_sub(1, relaxed);
}
// The clock thread. Waits until the readiness stamp of `value` has completed (the queue has done the frame's own
// work in front of its gate) or `deadline`: the time it was seen complete, 0 at the deadline, at a stop or where the
// gate was dropped. Nothing is registered where there is nothing to wait for (a deadline that has passed is a look).
// A registration that failed is replaced by a look every fifth of a millisecond.
int64_t GateWaitReady(HANDLE timer, HANDLE event, uint32_t epoch, uint64_t value, int64_t deadline) noexcept {
    bool registered = false;
    for (;;) {
        AcquireSRWLockShared(&lock);
        const bool live = gate_ready && gate_epoch.load(relaxed) == epoch;
        const uint64_t completed = live ? Completed(gate_ready) : 0;
        const int64_t now = Qpc();
        const bool wait = live && completed < value && now < deadline && !gate_stop.load(relaxed);
        if (wait && !registered) {
            ResetEvent(event);
            registered = SUCCEEDED(RegisterEvent(gate_ready, value, event));
        }
        ReleaseSRWLockShared(&lock);
        if (!live) return 0;
        if (completed >= value) return now;
        if (!wait) return 0;
        // The timer to 0.4 ms before the deadline, the rest on the clock (as AheadSleepUntil).
        int64_t until = deadline - frequency / 2500;
        if (!registered && until > now + frequency / 5000) until = now + frequency / 5000;
        if (until > now) WaitUntil(timer, event, nullptr, now, until);
        else YieldProcessor();
    }
}
// The clock thread's newest decisions, for the lines it writes when a flip came under 4 ms behind the one before.
struct GateSeen {
    bool real = false, after = false, moved = false;
    uint8_t note = 0;
    uint64_t marker = 0;
    uint32_t marker_released = 0, marker_issued = 0;
    int64_t cycle = 0, enqueued = 0, earliest = 0, done = 0, ready = 0, target = 0, released = 0, flip = 0;
};
constexpr unsigned kGateSeen = 8, kGatePairLines = 24;
// One line per decision, the newest five, times in microseconds from the newest flip (0: none). The first
// kGatePairLines pairs of the process.
void GateSayPair(const GateSeen* seen, unsigned next) noexcept {
    if (gate_pair_lines.load(relaxed) >= kGatePairLines) return;
    const unsigned pair = gate_pair_lines.fetch_add(1, relaxed);
    if (pair >= kGatePairLines) return;
    const GateSeen& newest = seen[(next - 1) % kGateSeen];
    const auto us = [&](int64_t qpc) noexcept {
        return qpc != 0 ? static_cast<long long>((qpc - newest.flip) * 1'000'000 / frequency) : 0LL;
    };
    const unsigned count = next < 5 ? next : 5;
    for (unsigned i = count; i > 0; --i) {
        const GateSeen& s = seen[(next - i) % kGateSeen];
        LOG_INFO("FG flip gate pair %u, flip -%u: %s frame, marker %llu (released %u, issued %u, the real frame's "
                 "hold %u), cycle %lld us; enqueued %lld, earliest %lld, generation done %lld, work done %lld, target "
                 "%lld, let go %lld, flip %lld (%s)",
            pair + 1, i - 1, s.real ? "real" : "generated", static_cast<unsigned long long>(s.marker),
            s.marker_released, s.marker_issued, static_cast<unsigned>(s.note),
            static_cast<long long>(s.cycle * 1'000'000 / frequency), us(s.enqueued), us(s.earliest), us(s.done),
            us(s.ready), us(s.target), us(s.released), us(s.flip),
            s.after ? "with its own work, after it was let go"
                : s.moved ? "when let go, moved forward for the next frame" : "when let go");
    }
}
// The clock thread, toward a real flip's time `target` (never past `limit`). Where the next frame's generation (its
// marker is `next`) completes first, the time moves forward as the rule says (RealBefore), never before now. Returns
// the time slept to. The unit's thread sets the clock's event at each marker; a timer to 0.4 ms before the time, the
// rest on the clock.
int64_t GateSleepReal(HANDLE timer, const FgFlipGateRule& rule, uint64_t next, int64_t target, int64_t limit,
    bool* moved) noexcept {
    if (target > limit) target = limit;
    const HANDLE event = gate_event.load(relaxed);
    bool looked = false;
    for (;;) {
        const int64_t now = Qpc();
        if (!looked) {
            if (const int64_t done = GateDoneOf(next); done != 0) {
                looked = true;
                // Forward only, never before now: a time that has passed already stays as it is.
                int64_t sooner = rule.RealBefore(target, done);
                if (sooner < now) sooner = now;
                if (sooner < target) {
                    gate_moved.fetch_add(1, relaxed);
                    gate_moved_us.fetch_add(static_cast<uint64_t>((target - sooner) * 1'000'000 / frequency), relaxed);
                    target = sooner;
                    *moved = true;
                }
            }
        }
        const int64_t left = target - now;
        if (left <= 0 || gate_stop.load(relaxed)) return target;
        if (left * 1'000'000 / frequency > 600 && timer && event)
            WaitUntil(timer, event, nullptr, now, target - frequency / 2500);
        else
            YieldProcessor();
    }
}
// The clock thread: lets each gate value go in order at its flip's time (fg_flip_gate_rule.h), on the unit's own
// measurements: when frame generation completed for a generated frame (the stamp of its marker), when a frame's own
// work was done (the stamp in front of its gate), and so when each flip happened (at its release where the work was
// done by then, else when the work was done). Every value at most kGateMostMs after its enqueue. A release ahead of
// the frame's work costs nothing: the flip waits for the work.
DWORD WINAPI GateClock(void*) noexcept {
    SetThreadDescription(GetCurrentThread(), L"ReLimiter FG flip gate");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    const HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    const HANDLE ready_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    FgFlipGateRule gate_rule;
    GateSeen seen[kGateSeen];
    unsigned seen_next = 0;
    int64_t last_shown = 0;
    uint64_t generated_marker = 0;   // the marker of the cycle's generated frame, 0 none
    while (!gate_stop.load(relaxed)) {
        AcquireSRWLockShared(&lock);
        const uint32_t head = gate_head.load(std::memory_order_acquire);
        const bool any = head != gate_tail.load(std::memory_order_acquire);
        GateEntry entry{};
        if (any) entry = gate_ring[head % kGateRing];
        const uint32_t epoch = gate_epoch.load(relaxed);
        ReleaseSRWLockShared(&lock);
        if (!any || frequency <= 0) {
            WaitForSingleObject(gate_event.load(relaxed), 50);
            continue;
        }
        const int64_t bound_qpc = entry.enqueued + frequency * kGateMostMs / 1000;
        const bool stamps = entry.stamped && ready_event != nullptr;
        GateSeen row;
        row.real = entry.real;
        row.note = entry.note;
        row.marker = entry.marker;
        row.marker_released = entry.released;
        row.marker_issued = entry.issued;
        row.enqueued = entry.enqueued;
        int64_t target = 0, ready = 0;
        if (g_fg_present_ahead.load(relaxed) >= 3 && !RuntimeLifecycle_IsStopping()) {
            if (!entry.real) {
                gate_rule.give_back = g_fg_flip_gate_give_back.load(relaxed);
                generated_marker = entry.marker;
                // The pacer's interval for this frame: where it held the frame's FG input to its grid, or had a plan
                // for it and did not hold (the frame was late for its slot); 0 where it had none.
                const int64_t noted = GateCycleOf(entry.marker);
                const int64_t cycle = noted < 0 ? -noted : noted;
                (noted > 0 ? gate_planned : noted < 0 ? gate_late : gate_unplanned).fetch_add(1, relaxed);
                const int64_t earliest = gate_rule.Begin(cycle);
                row.cycle = gate_rule.cycle;
                row.earliest = earliest;
                gate_half_us.store(gate_rule.cycle * 1'000'000 / frequency / 2, relaxed);
                // The distance from the flip before is still to keep: when the frame's own work is done and when
                // its generation completed say for how long. A frame whose work is not done by then is let go and
                // flips with its work.
                if (earliest > Qpc()) {
                    ready = stamps ? GateWaitReady(timer, ready_event, epoch, entry.value,
                        earliest < bound_qpc ? earliest : bound_qpc) : Qpc();
                    if (ready != 0) {
                        GateWorkSeen(entry.marker, epoch);
                        // The hold's limit counts from generation's completion: the frame's own work may have stood
                        // behind the gate before it. From the work where the completion was not stamped.
                        int64_t done = GateDoneOf(entry.marker);
                        if (done == 0 || done > ready) {
                            done = ready;
                            gate_undated.fetch_add(1, relaxed);
                        }
                        row.done = done;
                        target = gate_rule.Generated(ready, done);
                        if (target > ready) {
                            gate_slack_sum_us.fetch_add(static_cast<uint64_t>((target - ready) * 1'000'000 / frequency), relaxed);
                            gate_slack_count.fetch_add(1, relaxed);
                        }
                    }
                }
            } else {
                row.cycle = gate_rule.cycle;
                target = gate_rule.Real(entry.spacing, frequency / 10);
            }
        }
        if (target > bound_qpc) {
            target = bound_qpc;
            gate_bounded.fetch_add(1, relaxed);
        }
        const bool timed = target > Qpc();
        if (timed) {
            // A real flip gives way to the next frame where that frame's generation completes first.
            if (entry.real && generated_marker != 0)
                target = GateSleepReal(timer, gate_rule, generated_marker + 1, target, bound_qpc, &row.moved);
            else
                AheadSleepUntil(target, bound_qpc);
        }
        // A Signal that failed lets nothing go (as the render queue's release): the entry stays the head and is
        // signalled again until it is let go, the gate is dropped (the drop signals every value) or the thread stops.
        // The release is stamped in front of the Signal, the point the clock aims at the flip's time: the Signal's own
        // duration (12 us in the mean) is the same for every flip and is not lateness.
        int64_t released = 0;
        for (;;) {
            AcquireSRWLockShared(&lock);
            const bool mine = gate && gate_epoch.load(relaxed) == epoch && gate_head.load(relaxed) == head;
            released = Qpc();
            const bool gone = !mine || SUCCEEDED(CpuSignal(gate, entry.value)) || Completed(gate) == UINT64_MAX;
            if (mine && gone) gate_head.store(head + 1, std::memory_order_release);
            ReleaseSRWLockShared(&lock);
            if (gone || gate_stop.load(relaxed)) break;
            release_failures.fetch_add(1, relaxed);
            Sleep(1);
        }
        if (timed) {
            gate_on_target.fetch_add(1, relaxed);
            gate_wake_err_us.fetch_add(released > target ? static_cast<uint64_t>((released - target) * 1'000'000 / frequency) : 0, relaxed);
        } else {
            gate_immediate.fetch_add(1, relaxed);
        }
        // When the flip happened (`shown`, which the pairs are counted by): at its release as the clock made it where
        // the frame's own work was done by then (a generated frame held to a time was ready before it), else when the
        // work is done. The rule is given a flip held to a time at that time, not at the release: 14 to 20% of those
        // releases are 20 to 600 us late (the timer's wake; none over 1 ms in 12,333), and carried into the distance
        // kept they slow the hundredth given back per flip by their mean (12 to 18 us of 83 to 96). Measured inside
        // one launch with the release given to the rule: the generated flips 0.5 ms further behind generation and PC
        // Latency 1.0 ± 0.35 ms higher, for 0.03 to 0.05 ms less displayed change (N.164). The price of the time:
        // the flip behind a late release comes that much sooner after it.
        int64_t shown = released;
        int64_t flip = timed ? target : released;
        bool after = false;
        if (stamps && ready == 0 && GateWaitReady(timer, ready_event, epoch, entry.value, 0) == 0) {
            const int64_t done = GateWaitReady(timer, ready_event, epoch, entry.value, bound_qpc);
            if (done != 0) {
                if (entry.real) {
                    gate_real_late.fetch_add(1, relaxed);
                    gate_real_late_us.fetch_add(static_cast<uint64_t>((done - flip) * 1'000'000 / frequency), relaxed);
                }
                flip = done;
                shown = done;
                ready = done;
                after = true;
            }
        }
        gate_rule.Flipped(entry.real, flip);
        if (entry.real) generated_marker = 0;
        if (!entry.real) {
            GateWorkSeen(entry.marker, epoch);
            if (row.done == 0) row.done = GateDoneOf(entry.marker);
            if (row.done != 0 && flip >= row.done) {
                gate_lag_sum_us.fetch_add(static_cast<uint64_t>((flip - row.done) * 1'000'000 / frequency), relaxed);
                gate_lag_count.fetch_add(1, relaxed);
            }
        }
        row.ready = ready;
        row.target = target;
        row.released = released;
        row.flip = flip;
        row.after = after;
        seen[seen_next++ % kGateSeen] = row;
        if (last_shown != 0 && shown - last_shown < frequency / 250) {
            gate_pairs.fetch_add(1, relaxed);
            GateSayPair(seen, seen_next);
        }
        last_shown = shown;
    }
    if (timer) CloseHandle(timer);
    if (ready_event) CloseHandle(ready_event);
    gate_running.store(false, std::memory_order_release);
    return 0;
}
HRESULT STDMETHODCALLTYPE AheadRegister(ID3D12Fence* fence, UINT64 value, HANDLE event) {
    const AheadRegisterFn native = ahead_native.load(std::memory_order_acquire);
    const int mode = g_fg_present_ahead.load(relaxed);
    if (mode <= 0 || GetCurrentThreadId() != ahead_thread.load(relaxed)) return native(fence, value, event);
    const bool match = event && ahead_wait.fence == fence && ahead_wait.value == value;
    const bool again = ahead.woken && ahead.first == 0 && ahead.answered.fence == fence && ahead.answered.value == value;
    (match ? ahead_matched : ahead_other).fetch_add(1, relaxed);
    if (again) ahead_again.fetch_add(1, relaxed);
    if (ahead_lines.load(relaxed) < kAheadLines && ahead_lines.fetch_add(1, relaxed) < kAheadLines) {
        const DWORD error = GetLastError();
        const int64_t now = Qpc();
        const uint64_t completed = Completed(fence);
        LOG_INFO("FG present ahead: the presenting thread registers an event for fence %p value %llu (completed %llu); "
                 "its newest queue Wait is for fence %p value %llu, %lld us ago%s%s",
            static_cast<void*>(fence), static_cast<unsigned long long>(value),
            static_cast<unsigned long long>(completed), static_cast<void*>(ahead_wait.fence),
            static_cast<unsigned long long>(ahead_wait.value),
            frequency > 0 && ahead_wait.qpc ? static_cast<long long>((now - ahead_wait.qpc) * 1'000'000 / frequency) : -1LL,
            match ? " (the same)" : "", again ? " (again after a wake ahead)" : "");
        SetLastError(error);
    }
    if (match) ahead_wait.fence = nullptr;   // one answer per Wait
    if (mode < 2 || !match || again || !ahead_event.load(relaxed) || RuntimeLifecycle_IsStopping() ||
        g_fg_present_order_queue.load(relaxed) == nullptr) return native(fence, value, event);
    const bool counted = ahead.counted;
    ahead = AheadCycle{};
    ahead.answered = AheadWait{fence, value, 0};
    ahead.wake = Qpc();
    ahead.woken = true;
    ahead.counted = true;
    if (!counted) g_fg_present_ahead_woken.fetch_add(1, relaxed);
    ahead_woke.fetch_add(1, relaxed);
    SetEvent(event);
    return S_OK;
}
// The installer's worker: the detour, once, when the key is first on and a bind has read the runtime's method.
void AheadService() noexcept {
    // fg_present_ahead 3: the flip gate's clock thread (it lives until shutdown; one that ended behind a shutdown that
    // could not wait for it is replaced). A shutdown that is joining it has the lock: the next pass looks again.
    if (g_fg_present_ahead.load(relaxed) >= 3 && !RuntimeLifecycle_IsStopping() && frequency > 0 &&
        TryAcquireSRWLockExclusive(&gate_thread_lock)) {
        if (gate_thread && WaitForSingleObject(gate_thread, 0) == WAIT_OBJECT_0) {
            CloseHandle(gate_thread);
            gate_thread = nullptr;
        }
        bool tried = false, runs = false;
        // Stopping is looked at again under the lock: a shutdown that joined the clock in between gets no new one.
        if (!gate_thread && !RuntimeLifecycle_IsStopping()) {
            if (!gate_event.load(relaxed)) gate_event.store(CreateEventW(nullptr, FALSE, FALSE, nullptr), relaxed);
            if (gate_event.load(relaxed)) {
                gate_stop.store(false, relaxed);
                gate_running.store(true, std::memory_order_release);
                gate_thread = CreateThread(nullptr, 0, &GateClock, nullptr, 0, nullptr);
                if (!gate_thread) gate_running.store(false, relaxed);
                tried = true;
                runs = gate_thread != nullptr;
            }
        }
        ReleaseSRWLockExclusive(&gate_thread_lock);
        if (tried) LOG_INFO("FG flip gate: the clock thread %s", runs ? "runs" : "could not be created");
    }
    if (g_fg_present_ahead.load(relaxed) == 0 || ahead_installed.load(relaxed) || ahead_refused.load(relaxed)) return;
    void* const target = ahead_target.load(relaxed);
    if (!target || RuntimeLifecycle_IsStopping()) return;
    if (!ahead_event.load(relaxed)) ahead_event.store(CreateEventW(nullptr, FALSE, FALSE, nullptr), relaxed);
    bool enabled = false;
    MH_STATUS status = MH_UNKNOWN;
    {
        HookInstallGuard install;
        if (!install) return;
        if (Hooks_PinTargetModule(target)) {
            status = InstallHook(target, reinterpret_cast<void*>(&AheadRegister), &ahead_native);
            Hooks_FlushBatch();
            enabled = status == MH_OK && Hooks_TargetEnabled(target);
        }
    }
    ahead_installed.store(enabled, relaxed);
    ahead_refused.store(!enabled, relaxed);
    LOG_INFO("FG present ahead: the detour of the runtime's fence event registration %p is %s (status %d)", target,
        enabled ? "installed" : "refused", static_cast<int>(status));
}

} // namespace fg_present_order

void FgPresentOrder_NoteSignalActive(void* queue, void* fence) noexcept {
    using namespace fg_present_order;
    if (fence && fence == learned_fence.load(relaxed)) {
        const DWORD thread_id = GetCurrentThreadId();
        if (worker_thread.load(relaxed) != thread_id) worker_thread.store(thread_id, relaxed);
    }
    if (auto* scope = StreamlineInputScope::Current(); scope && !scope->order_queue) scope->order_queue = queue;
}

void FgPresentOrder_AfterWaitActive(ID3D12CommandQueue* queue, ID3D12Fence* fence, FgPresentOrder_FenceFn native_signal,
    FgPresentOrder_FenceFn native_wait) noexcept {
    using namespace fg_present_order;
    if (auto* scope = StreamlineInputScope::Current()) {
        if (scope->order_done || scope->order_queue != queue) return;
        scope->order_done = true;
        learned_queue.store(queue, relaxed);
        learned_fence.store(fence, relaxed);
    } else if (!fence || queue != learned_queue.load(relaxed) || fence != learned_fence.load(relaxed)) {
        return;
    }
    if (!native_signal || !native_wait || RuntimeLifecycle_IsStopping()) return;
    if (queue == failed_queue.load(relaxed) || !presenting.load(relaxed)) return;
    if (issuing.exchange(true, std::memory_order_acquire)) return;
    // The developer key was turned on after the bind: the trailing fence and its event are made now, once for the
    // binding (a bind with the key on made them already).
    if (g_fg_present_order_trailing.load(relaxed) && !trailing_tried.load(relaxed)) {
        AcquireSRWLockExclusive(&lock);
        if (ReadyLocked(queue)) MakeTrailingLocked(queue, bound_device);
        ReleaseSRWLockExclusive(&lock);
    }
    // fg_present_ahead 3: the gate's two fences, once for the binding, on the render queue's device.
    if (g_fg_present_ahead.load(relaxed) >= 3 && !gate_tried.load(relaxed)) {
        AcquireSRWLockExclusive(&lock);
        if (ReadyLocked(queue) && !gate) {
            BindFailure ignored;
            const void* same = nullptr;
            ID3D12Fence* made[2] = {};
            if (CreateFences(queue, made, 2, &ignored, &same) && same == bound_device) {
                gate = made[0];
                gate_ready = made[1];
            } else {
                ReleaseFence(&made[0]);
                ReleaseFence(&made[1]);
            }
        }
        gate_tried.store(true, relaxed);
        ReleaseSRWLockExclusive(&lock);
    }
    BindFailure failure;
    bool bound_now = false;
    AcquireSRWLockShared(&lock);
    bool ready = ReadyLocked(queue);
    if (!ready) {
        ReleaseSRWLockShared(&lock);
        AcquireSRWLockExclusive(&lock);
        if (!ReadyLocked(queue)) bound_now = BindLocked(queue, &failure);
        ReleaseSRWLockExclusive(&lock);
        AcquireSRWLockShared(&lock);
        ready = ReadyLocked(queue);
    }
    if (ready) {
        AcquireSRWLockExclusive(&release_lock);
        const Rule::Enqueue enqueue = rule.OnWait();
        ReleaseSRWLockExclusive(&release_lock);
        if (enqueue != Rule::Enqueue::Nothing) {
            const uint64_t value = g_fg_present_order_issued.load(relaxed) + 1;
            // The marker first: it completes when the queue has passed the Wait this call follows.
            if (SUCCEEDED(QueueCall(native_signal, queue, marker, value))) {
                // Without the Wait the queue is not blocked; the release then only moves the order fence along.
                if (enqueue == Rule::Enqueue::MarkerAndWait && FAILED(QueueCall(native_wait, queue, order, value)))
                    enqueue_failures.fetch_add(1, relaxed);
                g_fg_present_order_issued.store(value, std::memory_order_release);
                if (FAILED(RegisterEvent(marker, value, marker_event))) SetEvent(marker_event);
                const int64_t now = Qpc();
                const int64_t before = frame_qpc.exchange(now, relaxed);
                if (before && frequency > 0) {
                    // A pause or a loading screen is no frame time.
                    const int64_t interval = (now - before) * 1'000'000 / frequency;
                    if (interval >= 2'000 && interval <= 100'000) {
                        const unsigned smoothed = frame_us.load(relaxed);
                        frame_us.store(smoothed ? (smoothed * 7 + static_cast<unsigned>(interval)) / 8
                                                : static_cast<unsigned>(interval), relaxed);
                    }
                }
                // The work submitted from here on is the next frame's.
                BreakFrameStart(value);
                // fg_present_ahead 3: the pacer's target for this frame goes with its marker.
                if (g_fg_present_ahead.load(relaxed) >= 3) GateNoteMarker(value);
            } else {
                enqueue_failures.fetch_add(1, relaxed);
            }
        }
    }
    ReleaseSRWLockShared(&lock);
    issuing.store(false, std::memory_order_release);
    if (bound_now) LOG_INFO("FG present order: observing render queue %p", static_cast<void*>(queue));
    else if (!ready && failure.stage != 0) {
        failed_queue.store(queue, relaxed);
        LOG_WARN("FG present order: the fences for render queue %p could not be created (stage %d, result 0x%08lX); "
                 "ordering stays off for it until it is destroyed or frame generation starts again",
            static_cast<void*>(queue), failure.stage, static_cast<unsigned long>(failure.result));
    }
}

// An outermost submission went on its queue (called with the key on only, directly behind the native call). The thread
// a Present's return is waiting for answers it: its first submission is the anchor, whatever becomes of it (a lock
// that is busy leaves a deferred release to the watchdog, and no later submission of the thread may take it). While the
// trailing rule observes, whether the anchor came before the deadline of the Present's marker, on a queue that suits,
// is the evidence. While it defers, the remembered release waits behind the anchor: the unit's own Signal of `trailed`
// goes on the anchor's queue directly behind it, through the original (no detour sees it), and completes when that
// queue has finished what the thread submitted; the unit's thread then releases the render queue. The release counts
// as enqueued only once the native Signal has succeeded: one the watchdog takes before that is missing, never late.
// The queue is the one the anchor arrived on and is never the render queue; a queue of another device than the render
// queue's does not suit (nothing is signalled across devices): while observing it is no evidence, while deferring the
// release is left to the watchdog. The device is read from the queue at every anchor, once per frame, and never
// remembered (a native queue's destruction does not always reach the unit). No lock is waited for before the Signal:
// an anchor that finds one taken leaves the release to the watchdog.
void FgPresentOrder_AfterExecuteActive(ID3D12CommandQueue* queue, unsigned lists,
    FgPresentOrder_FenceFn native_signal) noexcept {
    using namespace fg_present_order;
    const DWORD thread_id = GetCurrentThreadId();
    if (trail_thread.load(relaxed) != thread_id) return;   // not the thread a Present's return is waiting for
    // ReLimiter's own submissions (the probe's lists) and an empty call are no work of the thread.
    if (!lists || gpu_queue_probe_detail::t_own_submission_depth != 0) return;
    // The claim is this submission's: from here no later one of the thread takes it, whatever this one does.
    DWORD claimed = thread_id;
    if (!trail_thread.compare_exchange_strong(claimed, 0, relaxed)) return;
    if (!native_signal || RuntimeLifecycle_IsStopping()) return;
    // The queue's device, read outside every lock: once per frame, and never remembered.
    const void* const device = DeviceOf(queue);
    if (!TryAcquireSRWLockShared(&lock)) return;
    const int64_t now = Qpc();
    TrailRule::Change change = TrailRule::Change::None;
    uint64_t value = 0, previous = 0;
    if (TryAcquireSRWLockExclusive(&release_lock)) {
        if (trail.thread == thread_id) {
            const bool inside = TrailRule::Followed(now, trail.value, deadline_marker, deadline_qpc);
            const bool suits = QueueUsableLocked(queue, device);
            if (!trail.deferred) {
                // The release was made at the Present's return: one more observation. A block takes the queue of its
                // first anchor that suits and counts only the anchors on that queue; a new queue is taken only when
                // everything signalled on `trailed` so far has completed (a Signal still queued on the queue before
                // could set the fence back).
                bool followed = inside && suits && trailed && trailed_event.load(relaxed);
                if (followed) {
                    void* const taken = trail_queue.load(relaxed);
                    if (!taken) {
                        if (Completed(trailed) >= trailed_value) trail_queue.store(queue, relaxed);
                        else followed = false;
                    } else {
                        followed = taken == static_cast<void*>(queue);
                    }
                }
                ClearTrail();
                change = trailing_rule.OnObserved(followed);
                ApplyTrailChange(change);
            } else {
                // The release is deferred: this is the thread's anchor, whether it suits or not.
                trail.thread = 0;
                if (inside && suits && !trail.enqueued && static_cast<void*>(queue) == trail_queue.load(relaxed) &&
                    trail.value > trailed_value && trail.value > g_fg_present_order_released.load(relaxed) && trailed &&
                    trailed_event.load(relaxed) && !trail_enqueuing.exchange(true, std::memory_order_acquire)) {
                    // The value is taken here and enqueued below as one step (trail_enqueuing is held until the
                    // Signal is on the queue), so the values rise in the queue's order. The release is not enqueued
                    // yet: that is published below, once the native call has succeeded.
                    previous = trailed_value;
                    trailed_value = trail.value;
                    value = trail.value;
                }
            }
        }
        ReleaseSRWLockExclusive(&release_lock);
    }
    if (value != 0) {
        const HANDLE event = trailed_event.load(relaxed);
        if (SUCCEEDED(QueueCall(native_signal, queue, trailed, value))) {
            // The Signal is on the queue: only now is the release enqueued. One the watchdog took in the gap before
            // was missing, and its record is gone. The registration follows the record, so a completion that is
            // there already wakes the unit's thread to a release that says enqueued.
            AcquireSRWLockExclusive(&release_lock);
            const bool awaited = trail.deferred && trail.value == value &&
                value > g_fg_present_order_released.load(relaxed);
            if (awaited) {
                trail.enqueued = true;
                trail_signalled.store(true, relaxed);
            }
            ReleaseSRWLockExclusive(&release_lock);
            // A registration that fails leaves the release to the watchdog's deadline.
            if (awaited && FAILED(RegisterEvent(trailed, value, event))) SetEvent(event);
        } else {
            // Nothing was enqueued: the value is not taken, and the watchdog finds the release missing.
            enqueue_failures.fetch_add(1, relaxed);
            AcquireSRWLockExclusive(&release_lock);
            if (trailed_value == value) trailed_value = previous;
            ReleaseSRWLockExclusive(&release_lock);
        }
        trail_enqueuing.store(false, std::memory_order_release);
    }
    ReleaseSRWLockShared(&lock);
    ReportTrailChange(change);
}

void FgPresentOrder_BeforeExecuteActive(ID3D12CommandQueue* queue, FgPresentOrder_FenceFn native_signal,
    FgPresentOrder_FenceFn native_wait) noexcept {
    using namespace fg_present_order;
    if (!native_signal || !native_wait || RuntimeLifecycle_IsStopping()) return;
    // The frame being submitted: this submission's number in it, and whether it gets a break.
    uint64_t frame = 0;
    unsigned index = 0;
    bool at_break = false, whole = false;
    AcquireSRWLockExclusive(&window_lock);
    if (window_current) {
        BreakFrame& record = window_frames[window_current & 3];
        if (record.frame == window_current) {
            frame = record.frame;
            index = ++record.count;
            whole = record.whole;
            at_break = whole ? index % record.every == 0
                             : BreakRule::At(record.window, record.windows, record.every, index);
        }
    }
    ReleaseSRWLockExclusive(&window_lock);
    if (!at_break) return;
    if (!TryAcquireSRWLockShared(&lock)) return;
    if (bound == queue && breaker && !breaking.exchange(true, std::memory_order_acquire)) {
        const uint64_t point = break_value.load(relaxed) + 1;
        // The Wait only behind its own Signal: a Wait without one would hold the queue for good.
        if (SUCCEEDED(QueueCall(native_signal, queue, breaker, point))) {
            break_value.store(point, relaxed);
            if (!whole) {
                AcquireSRWLockExclusive(&window_lock);
                BreakFrame& record = window_frames[frame & 3];
                if (record.frame == frame && record.point_count < kBreakPoints)
                    record.points[record.point_count++] = {point, index};
                ReleaseSRWLockExclusive(&window_lock);
            }
            if (FAILED(QueueCall(native_wait, queue, breaker, point))) enqueue_failures.fetch_add(1, relaxed);
            breaks.fetch_add(1, relaxed);
        } else {
            enqueue_failures.fetch_add(1, relaxed);
        }
        breaking.store(false, std::memory_order_release);
    }
    ReleaseSRWLockShared(&lock);
}

void FgPresentOrder_ReleaseActive(bool watchdog) noexcept {
    fg_present_order::ReleaseDue(watchdog, false);
}

void FgPresentOrder_Service(bool fg_presenting, int fg_multiplier) noexcept {
    using namespace fg_present_order;
    AheadService();
    multiplier.store(fg_multiplier, relaxed);
    // Frame generation started again: a queue whose fences failed is tried once more.
    if (fg_presenting && !presenting.exchange(true, relaxed)) failed_queue.store(nullptr, relaxed);
    else if (!fg_presenting) presenting.store(false, relaxed);
    if (!bound_live.load(relaxed)) return;
    // The developer key was turned off: the evidence of the trailing release starts over when it is turned on again.
    if (!g_fg_present_order_trailing.load(relaxed) && trailing_engaged.load(relaxed)) EndTrailing();
    if (g_fg_present_order.load(relaxed) && fg_presenting && fg_multiplier == bound_multiplier.load(relaxed)) return;
    // The binding's ground is gone (the key was turned off, frame generation stopped presenting, or its multiplier
    // changed): the queue's waits are released and the evidence starts again with the next followed Wait.
    AcquireSRWLockExclusive(&lock);
    void* const queue = bound;
    const char* reason = nullptr;
    if (queue) {
        reason = !g_fg_present_order.load(relaxed) ? "the key was turned off"
            : !fg_presenting ? "frame generation no longer presents" : "frame generation's multiplier changed";
        DropLocked();
    }
    ReleaseSRWLockExclusive(&lock);
    if (reason) LOG_INFO("FG present order: %s; render queue %p is let go", reason, queue);
}

void FgPresentOrder_OnQueueDestroy(void* native_queue) noexcept {
    using namespace fg_present_order;
    if (!native_queue) return;
    void* expected = native_queue;
    if (learned_queue.compare_exchange_strong(expected, nullptr, relaxed)) learned_fence.store(nullptr, relaxed);
    expected = native_queue;
    failed_queue.compare_exchange_strong(expected, nullptr, relaxed);
    AcquireSRWLockExclusive(&lock);
    const bool dropped = bound == native_queue;
    if (dropped) {
        DropLocked();
    } else {
        // The queue the trailing Signals went on is gone: its evidence goes with it, a Stopped verdict too (a release
        // that waits for such a Signal stays with the watchdog), and so does its fence: a Signal of it still queued
        // must not reach a queue that is taken next, which gets a fresh fence at the next followed Wait.
        AcquireSRWLockExclusive(&release_lock);
        const bool renew = trail_queue.load(relaxed) == native_queue;
        if (renew) {
            ResetTrailing();
            trailed_value = 0;
        }
        ReleaseSRWLockExclusive(&release_lock);
        if (renew) {
            ReleaseFence(&trailed);
            trailing_tried.store(false, relaxed);
        }
    }
    ReleaseSRWLockExclusive(&lock);
    if (dropped) LOG_INFO("FG present order: render queue %p destroyed, fences released", native_queue);
}

bool FgPresentOrder_Shutdown(unsigned long timeout_ms) noexcept {
    using namespace fg_present_order;
    // The flip gate's clock first (it takes `lock` shared); the drop below signals every gate value. A clock that
    // does not end in time is not stopped: the caller hears that, and the thread ends at its next look at the flag.
    bool clock_stopped = true;
    AcquireSRWLockExclusive(&gate_thread_lock);
    if (gate_thread) {
        gate_stop.store(true, relaxed);
        if (const HANDLE event = gate_event.load(relaxed)) SetEvent(event);
        clock_stopped = WaitForSingleObject(gate_thread, timeout_ms) == WAIT_OBJECT_0;
        if (clock_stopped) {
            CloseHandle(gate_thread);
            gate_thread = nullptr;
        }
    }
    ReleaseSRWLockExclusive(&gate_thread_lock);
    // The key goes first: from here BindLocked refuses, so nothing is bound behind the drop below.
    g_fg_present_order.store(false, relaxed);
    AcquireSRWLockExclusive(&lock);
    DropLocked();
    const HANDLE worker = thread;
    ReleaseSRWLockExclusive(&lock);
    if (!worker) return clock_stopped;
    SetEvent(stop_event);
    if (WaitForSingleObject(worker, timeout_ms) == WAIT_TIMEOUT) return false;
    // The events go under the exclusive lock: a release that is still running reads them under the shared one.
    AcquireSRWLockExclusive(&lock);
    DropLocked();
    CloseHandle(thread); thread = nullptr;
    CloseHandle(stop_event); CloseHandle(marker_event); CloseHandle(released_event);
    if (HANDLE const event = trailed_event.exchange(nullptr, relaxed)) CloseHandle(event);
    stop_event = marker_event = released_event = nullptr;
    ReleaseSRWLockExclusive(&lock);
    LOG_INFO("FG present order: stopped; %llu markers released by a Present, %llu by the watchdog, %llu enqueue "
             "failures, %llu release failures",
        static_cast<unsigned long long>(by_present.load()), static_cast<unsigned long long>(by_watchdog.load()),
        static_cast<unsigned long long>(enqueue_failures.load()), static_cast<unsigned long long>(release_failures.load()));
    return clock_stopped;
}

void FgPresentOrder_NoteQueueWaitActive(ID3D12CommandQueue* queue, ID3D12Fence* fence, uint64_t value) noexcept {
    using namespace fg_present_order;
    ahead_wait = AheadWait{fence, value, Qpc(), queue};
}

// fg_present_ahead 3: an outermost Signal went on `queue`. Inside a native Present of a cycle woken ahead it is
// DXGI's, behind its own command list: the gate's Wait goes directly behind the first one.
void FgPresentOrder_AfterSignalActive(ID3D12CommandQueue* queue) noexcept {
    using namespace fg_present_order;
    if (!gate_arm.armed || !gate_arm.open) return;
    ++gate_arm.signals;
    if (!gate_arm.placed && GateEnqueue(queue, gate_arm)) {
        gate_arm.placed = true;
        gate_behind.fetch_add(1, relaxed);
    }
}

void FgPresentOrder_NotePlanActive(bool held, double interval_us) noexcept {
    using namespace fg_present_order;
    if (auto* scope = StreamlineInputScope::Current()) {
        const int64_t ticks = interval_us > 0.0 && frequency > 0
            ? static_cast<int64_t>(interval_us * static_cast<double>(frequency) / 1e6) : 0;
        scope->order_cycle = held ? ticks : -ticks;
    }
}

void FgPresentOrder_NotePresentQueueActive(void* queue) noexcept {
    fg_present_order::present_queue = static_cast<ID3D12CommandQueue*>(queue);
}

// A native Present is about to be made by a thread in a cycle woken ahead. The cycle's first is the generated frame's:
// its time is taken. The second is the real frame's: it is held until it stands as far behind frame generation's
// completion as DLSS-G put it behind the generated frame's Present. fg_present_ahead 3: each of the two gets the flip
// gate, and the real frame's hold stays: it keeps the thread from running Presents ahead (without it three to four
// stood queued, each behind its gate, and DLSS-G's generator waited for the thread's queue in 9 cycles of 10; packet
// trace of 2026-11-16, N.159).
void FgPresentOrder_AheadBeforePresent() noexcept {
    using namespace fg_present_order;
    if (ahead.woken) {
        if (ahead.first == 0) {
            ahead.first = Qpc();
            // fg_present_ahead 3: the generated frame's marker. The thread's generated Presents take the markers one
            // by one, and a marker is released when its frame's generation is done: the oldest one not yet released
            // is this frame's, unless it is the one the thread's generated Present before this one took (that
            // frame's generation is not done yet), and then it is the next.
            if (g_fg_present_ahead.load(relaxed) >= 3) {
                const uint64_t released = g_fg_present_order_released.load(relaxed);
                const uint64_t issued = g_fg_present_order_issued.load(std::memory_order_acquire);
                if (issued > released) {
                    uint64_t value = released + 1;
                    if (gate_marker >= value && gate_marker < issued) {
                        value = gate_marker + 1;
                        gate_ambiguous.fetch_add(1, relaxed);
                    }
                    gate_marker = value;
                    GateArmFor(false, value, 0, 0);
                }
            }
        } else {
            GateRetry();
        }
        return;
    }
    if (!ahead.real) {
        GateRetry();
        return;
    }
    const int64_t entry = Qpc();
    const uint64_t value = ahead.value;
    const int64_t spacing = entry - ahead.first, wake = ahead.wake;
    AheadEnd();
    // How the hold ended, for the gate's pair lines: 0 the completion was stamped when the Present came, 1 its event
    // was waited for and the stamp found, 2 the event came without a stamp, 3 not held, 4 the wait was given up.
    const auto hold = [&]() noexcept -> uint8_t {
        if (value == 0 || frequency <= 0 || g_fg_present_ahead.load(relaxed) < 2 || RuntimeLifecycle_IsStopping() ||
            spacing <= 0 || spacing > frequency / 10) {
            ahead_unheld.fetch_add(1, relaxed);
            return 3;
        }
        uint8_t how = 0;
        int64_t done = 0;
        int stamped = AheadStamped(value, &done);
        if (stamped == 0) {
            how = 1;
            // Frame generation is not done yet: the event registered for the marker at the generated Present's return.
            if (const HANDLE event = ahead_event.load(relaxed))
                if (WaitForSingleObject(event, kAheadHoldMs) != WAIT_OBJECT_0) {
                    ahead_gave_up.fetch_add(1, relaxed);
                    how = 4;
                }
            // The unit's thread stamps the same completion: a short look for its stamp, else this thread's own clock.
            const int64_t woke = Qpc();
            for (int i = 0; i < 4096 && (stamped = AheadStamped(value, &done)) == 0; ++i) YieldProcessor();
            if (stamped == 0) {
                done = woke;
                stamped = 1;
                if (how == 1) how = 2;
            }
        }
        if (stamped < 0) {
            ahead_unheld.fetch_add(1, relaxed);
            return 3;
        }
        AheadSleepUntil(done + spacing, entry + frequency / 20);
        const auto micros = [](int64_t ticks) noexcept {
            return ticks > 0 ? static_cast<uint64_t>(ticks) * 1'000'000 / static_cast<uint64_t>(frequency) : 0ull;
        };
        const uint64_t us = micros(Qpc() - entry);
        ahead_held.fetch_add(1, relaxed);
        ahead_held_us.fetch_add(us, relaxed);
        ahead_spacing_us.fetch_add(micros(spacing), relaxed);
        ahead_lead_us.fetch_add(micros(done - wake), relaxed);
        uint64_t most = ahead_held_max_us.load(relaxed);
        while (us > most && !ahead_held_max_us.compare_exchange_weak(most, us, relaxed)) {}
        return how;
    };
    const uint8_t how = hold();
    if (g_fg_present_ahead.load(relaxed) >= 3) GateArmFor(true, value, spacing, how);
}

// A native Present of a thread in a cycle woken ahead, or with a gate armed, has returned; `presented`: it was not a
// test and returned S_OK. The gate armed for it is accounted first, on every return. True: it was the generated
// frame's, it presented and returned before frame generation was done, so nothing is released here. Where the
// trailing rule defers, the release is left as a deferred one: the unit's Signal goes behind the thread's next
// submission and completes when its queue has finished the generated frame's work, so the render queue cannot take
// the GPU in front of that work (released at the marker, it did in 1 frame of 200 at a cap, and the generated frame
// was then shown 4 to 7 ms late; packet traces of 2026-11-16). Elsewhere the marker's value is left for the unit's
// thread, which releases when the marker completes. False: the caller goes on as for any Present.
bool FgPresentOrder_AheadAfterPresent(bool presented) noexcept {
    using namespace fg_present_order;
    GateAfterPresent(presented);
    if (!presented || !ahead.woken || ahead.first == 0) return false;
    ahead.woken = false;
    ahead.real = true;
    const HANDLE event = ahead_event.load(relaxed);
    uint64_t value = 0;
    bool early = false;
    AcquireSRWLockShared(&lock);
    if (event && marker && order && g_fg_present_ahead.load(relaxed) >= 2 && !RuntimeLifecycle_IsStopping()) {
        const uint64_t released = g_fg_present_order_released.load(relaxed);
        if (g_fg_present_order_issued.load(std::memory_order_acquire) > released) {
            value = released + 1;
            if (Completed(marker) < value) {
                ResetEvent(event);
                early = SUCCEEDED(RegisterEvent(marker, value, event));
            }
        }
    }
    ReleaseSRWLockShared(&lock);
    ahead.value = value;
    if (!early) {
        ahead_behind.fetch_add(1, relaxed);
        return false;
    }
    // fg_present_ahead 3: the gate's Wait stands on the presenting thread's queue, so a Signal of the unit's behind the
    // thread's next submission would complete only at the flip: the release is made at the marker.
    if (g_fg_present_ahead.load(relaxed) < 3 && g_fg_present_order_trailing.load(relaxed)) {
        bool deferred = false;
        AcquireSRWLockShared(&lock);
        AcquireSRWLockExclusive(&release_lock);
        const uint64_t released = g_fg_present_order_released.load(relaxed);
        if (!(trail.deferred && trail.value > released) && value > released &&
            g_fg_present_order_queue.load(relaxed) != nullptr && trailing_rule.Defers() && DeferrableLocked()) {
            trail = Trail{GetCurrentThreadId(), Qpc(), value, true, false};
            trail_thread.store(trail.thread, relaxed);
            deferred = true;
        }
        ReleaseSRWLockExclusive(&release_lock);
        ReleaseSRWLockShared(&lock);
        if (deferred) {
            ahead_early.fetch_add(1, relaxed);
            return true;
        }
    }
    ahead_presented.store(value, std::memory_order_release);
    // The marker may have completed meanwhile and the unit's thread woken for it before the value was left: then this
    // Present releases, as one that returns after the completion does.
    if (Pending()) {
        uint64_t expected = value;
        if (ahead_presented.compare_exchange_strong(expected, 0, std::memory_order_acq_rel)) {
            ahead_behind.fetch_add(1, relaxed);
            return false;
        }
    }
    ahead_early.fetch_add(1, relaxed);
    return true;
}
#endif
