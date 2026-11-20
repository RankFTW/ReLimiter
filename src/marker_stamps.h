#pragma once

// The game's own SIMULATION_START, SIMULATION_END, RENDERSUBMIT_START and RENDERSUBMIT_END
// markers, stamped locally by ReLimiter's marker hooks (NvAPI SetLatencyMarker, Streamline's
// slPCLSetMarker) on the thread that sends them (developer key unified_queue_upstream,
// instrumentation_state.h). The driver's own report of the same markers reaches ReLimiter 2.5-3
// frames late; these stamps are in time for the next frame.
//
// A stamp belongs to a frame of one marker domain and one producer epoch, the fields a
// PacingFrameKey carries: a numeric frame id alone does not name a frame (the two hooks have
// their own id spaces, a synthetic id restarts with the producer epoch). The hook captures the
// producer epoch and the ring's reset generation at ENTRY (MarkerStamps_Capture), before any step
// that can be descheduled across a lifetime change, and publishes under that frozen lifetime
// (MarkerStamps_Publish); a reader asks for the complete key.
//
// Per marker: the clock (QPC) and the sending thread's CPU cycle counter and thread id, so that
// the CPU time the thread spent between a span's start and end marker is known
// (MarkerStamps_SpanCpuUs). A marker seen twice for one frame keeps the chronologically latest
// sample (the larger QPC, whichever thread claimed the slot last), its QPC, cycles and thread id
// always from that one sample.
//
// A ring of kMarkerStampSlots slots; a later frame taking a slot replaces what it held.
// Lock-free and allocation-free: a writer claims a slot with one compare-exchange on its sequence
// word (odd while it stores) and drops the stamp when the claim keeps failing, a reader takes a
// consistent copy or reports nothing. Never blocks, no Windows dependency, any thread.

#include "instrumentation_state.h"
#include "pacing_telemetry_types.h"

#include <atomic>
#include <cstdint>

enum class MarkerStampKind : uint8_t {
    SimulationEnd = 0,
    RenderSubmitStart = 1,
    RenderSubmitEnd = 2,
    SimulationStart = 3
};

// The marker domains: the values of PacingIdentityDomain / FrameIdentityDomain.
constexpr uint8_t kMarkerDomainNvApi = 1;
constexpr uint8_t kMarkerDomainPcl = 2;

constexpr uint32_t kMarkerStampSlots = 256;
constexpr uint32_t kMarkerStampKinds = 4;

// The complete identity of a frame's stamps (PacingFrameKey's domain, producer_epoch, frame_id).
struct MarkerStampKey {
    uint8_t domain = 0;
    uint32_t producer_epoch = 0;
    uint64_t frame_id = 0;
};

// A frame's stamps in QPC ticks; 0 = not seen. cycles and tid are indexed by MarkerStampKind: the
// sending thread's cycle counter (QueryThreadCycleTime) and thread id at that marker; a marker
// without a thread sample has tid 0 (and cycles 0).
struct MarkerStamps {
    uint8_t domain = 0;
    uint32_t producer_epoch = 0;
    uint64_t frame_id = 0;
    int64_t sim_end_qpc = 0;
    int64_t render_start_qpc = 0;
    int64_t render_end_qpc = 0;
    int64_t sim_start_qpc = 0;
    uint64_t cycles[kMarkerStampKinds] = {};
    uint32_t tid[kMarkerStampKinds] = {};
};

// What a hook reads at a marker, on the calling thread. The thread sample is valid when tid is
// not 0 and cycles is not 0.
struct MarkerClockSample {
    int64_t qpc = 0;
    uint64_t cycles = 0;
    uint32_t tid = 0;
};

// The ring's reset generation. A hook freezes it at entry and passes it to
// MarkerStamps_Record, which drops the stamp when MarkerStamps_Reset ran since: a hook that
// resumes after a lifecycle reset never publishes into the new generation.
uint32_t MarkerStamps_Generation() noexcept;

// Stamps one marker of the frame `key` names, read in ring generation `generation`. Ignored: a
// frame id of 0, a domain of 0, a QPC that is not positive (0 is the "not seen" value), a
// SimulationStart without a thread id, a generation that is not the ring's current one. A marker
// of the same frame stamped before keeps the sample with the larger QPC (an older or equal one
// is ignored).
void MarkerStamps_Record(const MarkerStampKey& key, uint32_t generation, MarkerStampKind kind,
                         const MarkerClockSample& sample) noexcept;

// True when the ring holds the frame of exactly this key (domain, producer epoch and frame id;
// any stamp may be 0), copied consistently into *out. False for a frame that was replaced, reset
// away, never stamped, of another domain or epoch, or is being written right now; never waits.
// *out is left unchanged when false.
bool MarkerStamps_Get(const MarkerStampKey& key, MarkerStamps* out) noexcept;

// Forgets every stamp (wait-free: later reads see an empty ring, and hooks that were admitted
// before it drop their stamps). For a controller lifecycle reset, where frame ids restart.
void MarkerStamps_Reset() noexcept;

// CPU time in µs the sending thread spent between a span's start and end marker: the cycle
// difference over rate_hz (cycles per second). False (nothing written) unless both markers
// have a thread sample, both came from the SAME thread id (across threads the difference is
// two unrelated counters), the end is not before the start and the rate is known (> 0).
bool MarkerStamps_SpanCpuUs(const MarkerStamps& stamps, MarkerStampKind start, MarkerStampKind end,
                            double rate_hz, double* cpu_us) noexcept;

// A hook's frozen lifetime and sample of one marker (MarkerStamps_Capture). Only `live` is
// initialized: with the key off that is all the capture touches.
struct MarkerStampCapture {
    bool live = false;
    MarkerStampKind kind;
    uint32_t generation;     // the ring's reset generation at capture
    uint32_t producer_epoch; // the hook's producer epoch at capture
    int64_t qpc;
    uint64_t cycles;
    uint32_t tid;
};

// The hooks' first call, at hook ENTRY, before any admission step that can be descheduled across
// a lifetime change (a device check, a token, a lock): freezes the ring's reset generation, the
// hook's producer epoch (`producer_epoch()`, the current epoch of its domain) and the marker's
// sample (`sample()`, a MarkerClockSample: the arrival time and the thread), each called here and
// only here and in that order. With the key off (g_unified_queue_upstream 0) it is one relaxed load
// and a branch that is not taken: no clock, epoch or thread read, nothing else touched. marker_type
// is the NV_LATENCY_MARKER_TYPE / Streamline PCL marker value (SIMULATION_START 0, SIMULATION_END 1,
// RENDERSUBMIT_START 2, RENDERSUBMIT_END 3); every other type is not captured. Call it on the thread
// that sends the marker.
template <typename Epoch, typename Sample>
inline MarkerStampCapture MarkerStamps_Capture(uint32_t marker_type, Epoch&& producer_epoch,
                                               Sample&& sample) noexcept
{
    MarkerStampCapture capture;
    capture.live = false;
    if (g_unified_queue_upstream.load(std::memory_order_relaxed) == 0) return capture;
    if (marker_type > 3u) return capture;
    // NV values 0..3 are SimulationStart, SimulationEnd, RenderSubmitStart, RenderSubmitEnd.
    capture.kind = marker_type == 0u ? MarkerStampKind::SimulationStart
                                     : static_cast<MarkerStampKind>(marker_type - 1u);
    capture.generation = MarkerStamps_Generation();
    capture.producer_epoch = static_cast<uint32_t>(producer_epoch());
    const MarkerClockSample read = sample();
    capture.qpc = read.qpc;
    capture.cycles = read.cycles;
    capture.tid = read.tid;
    capture.live = true;
    return capture;
}

// The hooks' second call, once the marker is admitted as the game's own: publishes the capture
// under the frame's key (the captured producer epoch, `domain`, `frame_id`). Dropped when the
// producer epoch is no longer the captured one (`current_epoch()`, read here and only here) or, in
// MarkerStamps_Record, when the ring was reset since the capture: a call that was suspended across
// another lifetime stamps nothing into it. A capture that is not live does nothing.
template <typename Epoch>
inline void MarkerStamps_Publish(const MarkerStampCapture& capture, uint8_t domain, uint64_t frame_id,
                                 Epoch&& current_epoch) noexcept
{
    if (!capture.live) return;
    if (static_cast<uint32_t>(current_epoch()) != capture.producer_epoch) return;
    MarkerStamps_Record(MarkerStampKey{ domain, capture.producer_epoch, frame_id }, capture.generation,
                        capture.kind, MarkerClockSample{ capture.qpc, capture.cycles, capture.tid });
}

// The same lookup by a pacing record's key, the domain taken as it is (a PCL id is never looked up
// as an NvAPI id, a key of any other domain finds nothing).
static_assert(static_cast<uint8_t>(PacingIdentityDomain::NvApiFrame) == kMarkerDomainNvApi &&
              static_cast<uint8_t>(PacingIdentityDomain::PclFrame) == kMarkerDomainPcl,
              "the stamp ring's domain values are the identity domains'");
inline bool MarkerStamps_Get(const PacingFrameKey& key, MarkerStamps* out) noexcept
{
    return MarkerStamps_Get(MarkerStampKey{ static_cast<uint8_t>(key.domain), key.producer_epoch, key.frame_id }, out);
}
