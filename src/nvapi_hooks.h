#pragma once

#include "nvapi_types.h"
#include <atomic>

// NvAPI hook trampolines (accessible to other modules)
extern PFN_NvAPI_D3D_SetSleepMode      s_orig_sleep_mode;
extern PFN_NvAPI_D3D_Sleep             s_orig_sleep;
extern PFN_NvAPI_D3D_SetLatencyMarker  s_orig_set_latency_marker;

// AddRef-owned device publication captured from NvAPI calls. Consumers still
// dereference only through NvAPI_AcquireFeedbackDevice for generation safety.
extern std::atomic<IUnknown*> g_dev;
inline std::atomic<bool> g_reflex_sleep_bypass{false};
// Developer A/B bypass_low_latency_off: while the Sleep is bypassed, Hook_Sleep
// has the driver's low latency mode turned off (SetSleepMode forwarded with
// bLowLatencyMode and bLowLatencyBoost 0) so ReLimiter is the only pacer.
inline std::atomic<bool> g_bypass_low_latency_off{false};
// marker_forward_order == after_wait: direct SIMULATION_START/RENDERSUBMIT_START
// reach the driver after the source wait instead of on hook entry.
inline std::atomic<bool> g_marker_forward_after_wait{false};

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
// so one pacer runs through the overload pause. In passthrough (Dynamic MFG,
// uncapped, no presenting swapchain) and
// for the Reflex driver limiter the driver paces the game, so its Sleep runs.
inline bool NvAPI_ShouldBypassSleep(bool enabled,int target_fps,bool source_paced) noexcept {
	return enabled && target_fps>0 && source_paced;
}

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

// Both arguments must be COM devices (never Vulkan/OpenGL/DX9 native handles).
// Compares IUnknown identity after bounded, explicit ReShade unwrap only.
// Releases all temporary QueryInterface references; never compares adapter LUIDs.
bool NvAPI_IsSameDevice(IUnknown* left, IUnknown* right) noexcept;

// published_device receives an identity token for the owned global
// publication; the token itself does not carry an additional COM reference.
// Only a new publication returns true/sets the token. Equivalent aliases leave
// the original callable device, epoch, sequence and restoration state untouched.
bool NvAPI_CaptureFeedbackDevice(
	IUnknown* device,
	NvAPIFeedbackDevice* published_device = nullptr) noexcept;
NvAPIFeedbackDevice NvAPI_AcquireFeedbackDevice() noexcept;
bool NvAPI_IsFeedbackDeviceCurrent(
	const NvAPIFeedbackDevice& feedback_device) noexcept;
void NvAPI_ReleaseFeedbackDevice(IUnknown* device) noexcept;
void NvAPI_ClearFeedbackDevice(
	const NvAPIFeedbackDevice& expected_device) noexcept;
void NvAPI_ClearFeedbackDevice(IUnknown* expected_device = nullptr) noexcept;

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
	Failed        // no trampoline, or the driver call failed
};

// SetSleepMode on the presenting device with the game's captured parameters
// and minimum_interval_us. Scheduler owner.
DriverLimiterApply NvAPI_ApplyDriverLimiter(uint32_t minimum_interval_us) noexcept;
// Restores the game's parameters (RestoreGameSleepMode). Scheduler owner.
void NvAPI_ReleaseDriverLimiter() noexcept;
