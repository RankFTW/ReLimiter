#pragma once

#include "nvapi_types.h"
#include <atomic>

// NvAPI hook trampolines (accessible to other modules)
extern PFN_NvAPI_D3D_SetSleepMode      s_orig_sleep_mode;
extern PFN_NvAPI_D3D_Sleep             s_orig_sleep;
extern PFN_NvAPI_D3D_SetLatencyMarker  s_orig_set_latency_marker;

// The feedback-device publication captured from NvAPI calls is an owned NATIVE device (nvapi_hooks.cpp): consumers
// reach it only through NvAPI_AcquireFeedbackDevice (generation safety) or NvAPI_HasFeedbackDevice (a value).
// The driver's sleep status as the pacing CSV's driver_sleep_interval_us and
// driver_use_game_sleep columns: bit 63 set = a result, bits 40..62 = the
// feedback-device publication the result belongs to (SleepStatusToken), bit 32 =
// bUseGameSleep, low 32 bits = sleepIntervalUs. g_sleep_status_newest is the
// newest successful NvAPI_D3D_GetSleepStatus result (the game's own call or the
// poll). Its device token is validated at Sleep return; the pending source-row
// result (PacingPendingSleep) is invalidated on every device and readout change.
constexpr uint32_t kSleepStatusTokenMask = 0x7FFFFFu; // bits 40..62 of the word
constexpr uint64_t SleepStatusWord(uint32_t interval_us, bool use_game_sleep, uint32_t token = 0) noexcept {
	return (uint64_t{1} << 63) | ((uint64_t{token} & kSleepStatusTokenMask) << 40) |
	       (use_game_sleep ? (uint64_t{1} << 32) : 0) | interval_us;
}
constexpr uint32_t SleepStatusToken(uint64_t word) noexcept {
	return static_cast<uint32_t>((word >> 40) & kSleepStatusTokenMask);
}
inline std::atomic<uint32_t> g_sleep_status_token{0};
inline std::atomic<uint64_t> g_sleep_status_newest{0};

// OSD readouts, EMA-smoothed (µs). CPU latency: SIM_START → RENDERSUBMIT_END
// of the same frame, written by the marker hooks. Pacer latency: time spent in
// NvAPI_D3D_Sleep, written by Hook_Sleep. Moved from predictor.h.
inline std::atomic<double> g_cpu_latency_us{0.0};
inline std::atomic<double> g_pacer_latency_us{0.0};
// Updates g_cpu_latency_us (EMA alpha 0.16) from frame_id's RENDERSUBMIT_END
// and the SIM_START of the same frame in g_marker_log. Scheduler owner only
// (the marker log's writer); used by the NvAPI and PCL marker hooks.
void NvAPI_RecordCpuLatency(uint64_t frame_id, int64_t render_submit_end_qpc) noexcept;
// Reflex Sleep bypass: skip the driver's Sleep while a cap is set and
// ReLimiter's own wait paces the game (Scheduler_PacesSource), held or not,
// so one pacer runs. In passthrough (Dynamic MFG,
// uncapped, no presenting swapchain) and
// for the Reflex driver limiter the driver paces the game, so its Sleep runs.
inline bool NvAPI_ShouldBypassSleep(bool enabled,int target_fps,bool source_paced) noexcept {
	return enabled && target_fps>0 && source_paced;
}

// A feedback-device publication token. `device` is the published device's canonical identity: a NATIVE Direct3D
// device object, owned by the publication (one reference, held while it is published). A token obtained from
// NvAPI_AcquireFeedbackDevice carries one more reference of its own, which the caller releases
// (NvAPI_ReleaseFeedbackDevice); every other token is a value only and must never be dereferenced.
struct NvAPIFeedbackDevice {
	IUnknown* device = nullptr;
	uint32_t producer_epoch = 0;
	// Advances only when the published device changes, never for rejected clears
	// or duplicate captures. Writer/snapshot exclusion has a separate sequence.
	uint64_t publication_sequence = 0;
};

// Pure publication policy shared by current checks, conditional clears, and
// restoration. Pointer equality alone cannot distinguish A -> B -> A reuse.
inline bool NvAPI_IsSameFeedbackPublication(
	const NvAPIFeedbackDevice& expected,
	const NvAPIFeedbackDevice& current) noexcept {
	return expected.device != nullptr &&
		(expected.publication_sequence & 1u) == 0 &&
		expected.device == current.device &&
		expected.producer_epoch == current.producer_epoch &&
		expected.publication_sequence == current.publication_sequence;
}

// A device is published as the feedback device by its NATIVE identity. ReLimiter never calls AddRef, Release or
// QueryInterface (other than IID_UnwrappedObject) on a device pointer that a game or plugin passed in: that pointer
// can be a ReShade device proxy whose reference count takes ReShade's process-wide device lock (src/AGENTS.md,
// "ReShade's device lock"). An incoming pointer is unwrapped once to the native device it wraps
// (feedback_device_identity.h) or compared by value as the publication's alias.
//
// True when something is published (a value read, no reference).
bool NvAPI_HasFeedbackDevice() noexcept;

// Publishes the native device behind `device` (an incoming pointer: the game's own, a proxy, a wrapper) when it is not
// the published one. published_device receives an identity token for the owned global publication; the token itself
// does not carry an additional COM reference. Only a new publication returns true/sets the token. An equivalent alias
// (the same native device) leaves the original publication, epoch, sequence and restoration state untouched.
bool NvAPI_CaptureFeedbackDevice(
	IUnknown* device,
	NvAPIFeedbackDevice* published_device = nullptr) noexcept;
// The published native device with its own reference (release with NvAPI_ReleaseFeedbackDevice), for ReLimiter's own
// NvAPI calls. The reference is on a native object. Empty when nothing is published or a transition keeps moving.
NvAPIFeedbackDevice NvAPI_AcquireFeedbackDevice() noexcept;
bool NvAPI_IsFeedbackDeviceCurrent(
	const NvAPIFeedbackDevice& feedback_device) noexcept;
// Releases a reference on a NATIVE object (ReLimiter owns no other kind).
void NvAPI_ReleaseFeedbackDevice(IUnknown* device) noexcept;
void NvAPI_ClearFeedbackDevice(
	const NvAPIFeedbackDevice& expected_device) noexcept;
// `expected_device` names the publication by the pointer it was captured with (the native device or the alias).
void NvAPI_ClearFeedbackDevice(IUnknown* expected_device = nullptr) noexcept;
// destroy_device: clears the publication when `identity` (the canonical identity of the destroyed device's native
// object, DeviceIdentity_OfNative(get_native())) is the published device; a value check otherwise, with no writer
// section, no reference and no COM call. Runs under ReShade's device lock: it never touches a device proxy. The
// publication's own reference (on the native) is released at once, or retired to the next drain.
void NvAPI_OnDeviceDestroyed(uint64_t identity) noexcept;

// Game's requested sleep interval (from SetSleepMode)
extern std::atomic<uint32_t> g_game_requested_interval;

// PRESENT_START gate timing — now declared in presentation_gate.h
// (g_last_gate_sleep_us moved to presentation_gate module)

// Install NvAPI hooks. Call after MH_Initialize().
void InstallNvAPIHooks();

// Developer flip_metering_off: hides NvAPI_D3D12_SetFlipConfig from every later
// nvapi_QueryInterface request in the process (64-bit). Call once during init,
// after MH_Initialize() and before the game's frame generation resolves NvAPI
// functions; interception lasts until the addon shuts down.
void InstallFlipMeteringBlock();

// Inside a HookBatch the two installers above only queue their hooks. Call these after the batch applied: each
// logs what is really installed (and begins the NvAPI producer epoch) from Hooks_TargetEnabled. Outside a batch
// the installers call them themselves, so they need no second call; a second call without an install is a no-op.
void NvAPIHooks_ReportInstalled();
void ReportFlipMeteringBlock();

// Forward a marker call through the real trampoline
NvAPI_Status InvokeSetLatencyMarker(IUnknown* dev, NV_LATENCY_MARKER_PARAMS* params);

// Forward a sleep mode call through the real trampoline
NvAPI_Status InvokeSetSleepMode(IUnknown* dev, NV_SET_SLEEP_MODE_PARAMS* params);

// Restore the game's original sleep mode params (call on shutdown)
void RestoreGameSleepMode();

// Reflex driver limiter. While nonzero,
// Hook_SetSleepMode forwards game calls with this minimumIntervalUs. Written
// by the scheduler owner (sleep_mode.cpp).
inline std::atomic<uint32_t> g_driver_limiter_interval_us{0};

enum class DriverLimiterApply : uint8_t {
	Applied = 0,
	NoGameParams, // the game's SetSleepMode parameters were never captured
	NoDevice,     // no current feedback device, or it is not the one the parameters belong to
	Failed,       // no trampoline, or the driver call failed
	Busy          // the forward lock was held: nothing sent, repaired by the next call
};

// SetSleepMode on the presenting device with the game's captured parameters
// and minimum_interval_us. Scheduler owner.
DriverLimiterApply NvAPI_ApplyDriverLimiter(uint32_t minimum_interval_us) noexcept;
// Restores the game's parameters (RestoreGameSleepMode). Scheduler owner.
void NvAPI_ReleaseDriverLimiter() noexcept;

// Unified pacer (the derived owner: 64-bit, no test mode).
// True while the Sleep hook runs the unified path and its self-check found the driver
// pacing on its own (a Sleep over 200 us in at least 16 of the last 64; cleared below 3
// of 64). Diagnosis only: nothing else reads it.
inline std::atomic<bool> g_unified_driver_pacing{false};

// The Sleep-mode tuple: one applied record {publication, owner, low latency off, interval,
// epoch} under the forward lock, against the desired tuple (UnifiedPacer_SleepModeDesired:
// the key, Dynamic MFG, the limiter interval and the game's captured parameters). The
// record is kept only while the key is on (or a reconcile is pending): with the key off
// and settled nothing is recorded
// and every key-off path is as before. The epoch is bumped on key toggles and Dynamic MFG
// transitions; it matters only inside the key-on tuple. The bump also restarts the Sleep
// self-check. Scheduler owner.
void NvAPI_BumpSleepModeEpoch() noexcept;
// The key the clamp will publish (Config_ApplyPacingPipeline calls it BEFORE
// PacingPipeline_SetConfig): the key is noted, making the reconcile pending, before any
// reader can observe it. An unchanged key is one load and a branch. Any thread.
void NvAPI_PrepareUnifiedKey(bool clamped_key) noexcept;
// The published pending state, one load: true while the key is on or being published, or
// while a restoration is owed (a Unified or Limiter tuple was applied and the game's
// parameters are not back): callers then reconcile instead of the key-off paths. Any thread.
bool NvAPI_SleepModeReconcilePending() noexcept;
// Forwards the desired tuple when it differs from the applied record (an invalid record
// differs). Never blocks or spins: a held forward lock, captured parameters held by another
// thread, a missing parameter set or a failed call leaves it pending for the next call (no
// backoff). Any thread (the Sleep hook, the owner's OnMarker).
void NvAPI_ReconcileSleepMode() noexcept;
