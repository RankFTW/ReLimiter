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
// Compares IUnknown identity and releases temporary QueryInterface references.
bool NvAPI_IsSameDevice(IUnknown* left, IUnknown* right) noexcept;

// published_device receives an identity token for the owned global
// publication; the token itself does not carry an additional COM reference.
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

// Forward a marker call through the real trampoline
NvAPI_Status InvokeSetLatencyMarker(IUnknown* dev, NV_LATENCY_MARKER_PARAMS* params);

// Forward a sleep mode call through the real trampoline
NvAPI_Status InvokeSetSleepMode(IUnknown* dev, NV_SET_SLEEP_MODE_PARAMS* params);

// Restore the game's original sleep mode params (call on shutdown)
void RestoreGameSleepMode();
