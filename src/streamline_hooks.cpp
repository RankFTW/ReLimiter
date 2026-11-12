#include "streamline_hooks.h"
#include "streamline_discovery.h"
#include "streamline_gpu_handoff.h"
#include "streamline_queue_hooks.h"
#include "fg_state_policy.h"
#include "ngx_hooks.h"
#include "hooks.h"
#include "loadlib_hooks.h"
#include "config.h"
#include "dlss_presets.h"
#include "flush.h"
#include "feedback.h"
#include "runtime_lifecycle.h"
#include "logger.h"
#include "swapchain_manager.h"
#include <algorithm>
#include <cmath>
#include <cstring>

// ── Shared FG state ──
std::atomic<int>  g_fg_multiplier{0};
std::atomic<bool> g_fg_active{false};
std::atomic<bool> g_fg_presenting{false};

// Public DLSSGMode: 0=eOff, 1=eOn, 2=eAuto, 3=eDynamic.
std::atomic<int>  g_fg_mode{0};
static std::atomic<bool> s_fg_mode_observed{false};

// Game's requested MaxFrameLatency, captured by FLC vtable hook
std::atomic<uint32_t> g_game_requested_latency{0};

// Driver-reported actual FG multiplier from GetState
std::atomic<int> g_fg_actual_multiplier{0};

// Track numFramesActuallyPresented to detect real FG frame production.
// Per the Streamline SDK header (sl_dlss_g.h), this field reports the
// "number of frames presented since the last slDLSSGGetState call" —
// it's a per-call delta, NOT a monotonically increasing counter.
// When FG is actively generating, this will be > 1 (e.g. 2 for FG 2×).
// When FG is off or stalled, this will be 0 or 1.

// ── Streamline types ──
using sl_Result = int;
constexpr sl_Result sl_eOk = 0;

// Function pointer types for Streamline API
using PFN_slGetFeatureFunction = sl_Result(__cdecl*)(uint32_t feature, const char* name, void** outPtr);

// Correct signatures — GetState has 3 parameters (vp, state, options)
using PFN_slDLSSGSetOptions = sl_Result(__cdecl*)(const void* vp, const void* opts);
using PFN_slDLSSGGetState   = sl_Result(__cdecl*)(const void* vp, void* state, const void* options);

// ── Trampolines ──
static std::atomic<PFN_slGetFeatureFunction> s_orig_slGetFeatureFunction{nullptr};
static std::atomic<PFN_slDLSSGSetOptions> s_orig_SetOptions{nullptr};
static std::atomic<PFN_slDLSSGGetState> s_orig_GetState{nullptr};

// ── Detour: SetOptions — forward first, then read on success ──
// ── Track whether SetOptions has ever configured FG ──
// Games that load Streamline for Reflex only (e.g. MH Stories 3) never
// call slDLSSGSetOptions. Without this guard, Detour_GetState reads
// uninitialized memory from the state struct and falsely reports
// "FG active", triggering flushes and state transitions that crash.
static std::atomic<bool> s_setoptions_ever_called{false};

// ── Deferred FG inference ──
// SetOptions with numFrames > 0 sets the multiplier and starts a
// confirmation window. If GetState confirms numFramesActuallyPresented > 1
// within the window, FG is real. If the window expires without
// confirmation, the inference is revoked (game uses Streamline for
// Reflex only, e.g. MH Stories 3). Compatibility mode promotes only with
// independent current-lifetime NGX or Reflex AI evidence.
static std::atomic<bool> s_fg_inference_pending{false};
static std::atomic<int64_t> s_fg_inference_start_qpc{0};
static std::atomic<bool> s_fg_confirmed_by_getstate{false};
static std::atomic<uint64_t> s_fg_inference_viewport{0};
static std::atomic<uint64_t> s_configured_viewport{0};
static std::atomic<uint32_t> s_inference_getstate_call_count{0};
static std::atomic<uint64_t> s_options_publication_sequence{0};
static std::atomic<uint64_t> s_streamline_lifetime_generation{1};
static std::atomic<uint32_t> s_options_state_readers{0};
static constexpr int64_t FG_CONFIRMATION_WINDOW_US = 3000000; // 3 seconds

static uint64_t BeginOptionsStateWrite() noexcept {
    uint64_t sequence =
        s_options_publication_sequence.load(std::memory_order_acquire);
    for (;;) {
        if ((sequence & 1u) != 0) {
            SwitchToThread();
            sequence = s_options_publication_sequence.load(
                std::memory_order_acquire);
            continue;
        }
        if (s_options_publication_sequence.compare_exchange_weak(
                sequence,
                sequence + 1,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            while (s_options_state_readers.load(
                       std::memory_order_acquire) != 0) {
                SwitchToThread();
            }
            return sequence + 1;
        }
    }
}

static void EndOptionsStateWrite(uint64_t write_sequence) noexcept {
    s_options_publication_sequence.store(
        write_sequence + 1,
        std::memory_order_release);
}

static uint64_t GetViewportKey(const void* viewport) noexcept {
    if (!viewport) return 0;
    uint32_t value = 0;
    __try {
        // sl::ViewportHandle derives from BaseStructure (32 bytes on x64),
        // followed by its stable uint32_t logical handle value.
        value = *reinterpret_cast<const uint32_t*>(
            reinterpret_cast<const uint8_t*>(viewport) + 32);
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return static_cast<uint64_t>(value) + 1;
}

static void CancelDeferredFGInference() noexcept {
    s_fg_inference_pending.store(false, std::memory_order_relaxed);
    s_fg_inference_viewport.store(0, std::memory_order_relaxed);
    s_fg_confirmed_by_getstate.store(false, std::memory_order_relaxed);
    s_inference_getstate_call_count.store(0, std::memory_order_relaxed);
}

static void BeginDeferredFGInference(
    uint64_t viewport_key,
    bool compatibility_mode) noexcept {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    s_fg_inference_viewport.store(viewport_key, std::memory_order_relaxed);
    s_fg_inference_start_qpc.store(now.QuadPart, std::memory_order_relaxed);
    s_inference_getstate_call_count.store(0, std::memory_order_relaxed);
    s_fg_confirmed_by_getstate.store(false, std::memory_order_relaxed);
    s_fg_inference_pending.store(true, std::memory_order_release);
    LOG_INFO(
        compatibility_mode
            ? "FG presenting deferred (compat) — waiting for passive runtime evidence"
            : "FG presenting deferred — waiting for GetState confirmation");
}

static void ApplyExplicitFGModeTransition(
    bool mode_changed,
    uint32_t mode) noexcept {
    if (!mode_changed) return;
    CancelDeferredFGInference();
    ResetReflexAiEvidence();
    if (mode != 0) return;

    // A current explicit Streamline Off report owns this mode lifetime. Stale
    // NGX creation/runtime state must not keep presenting true and then prevent
    // a later On/Auto transition from rearming inference.
    g_fg_active.store(false, std::memory_order_relaxed);
    g_fg_presenting.store(false, std::memory_order_relaxed);
    g_fg_actual_multiplier.store(0, std::memory_order_relaxed);
}

static void UpdateConfiguredFGActivity(bool configuration_changed, uint32_t mode, int generated_frames) noexcept {
    if (!configuration_changed) return;
    const bool active = FrameGeneration_ConfiguredActivity(false, true, mode, generated_frames);
    if (g_fg_active.exchange(active, std::memory_order_relaxed) != active) {
        LOG_INFO("FG active (inferred from SetOptions): %s", active ? "yes" : "no");
        OnFGStateChange();
    }
}

static sl_Result __cdecl Detour_SetOptions(const void* vp, const void* opts) {
    const bool runtime_stopping = RuntimeLifecycle_EnterCallback();
    __try {
    if (runtime_stopping)
        return s_orig_SetOptions.load(std::memory_order_acquire)
            ? s_orig_SetOptions.load(std::memory_order_acquire)(vp, opts) : -1;

    const uint64_t lifetime_generation =
        s_streamline_lifetime_generation.load(std::memory_order_acquire);
    sl_Result result;
    __try {
        result = s_orig_SetOptions.load(std::memory_order_acquire)(vp, opts);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        LOG_WARN("Detour_SetOptions: SEH exception in trampoline (0x%08X)", GetExceptionCode());
        return -1;
    }

    uint64_t options_write_sequence = BeginOptionsStateWrite();
    if (lifetime_generation !=
        s_streamline_lifetime_generation.load(std::memory_order_acquire)) {
        EndOptionsStateWrite(options_write_sequence);
        return result;
    }
    __try {
        if (result == sl_eOk && opts) {
            const uint64_t viewport_key = GetViewportKey(vp);
            const uint64_t previous_viewport =
                s_configured_viewport.exchange(
                    viewport_key, std::memory_order_relaxed);
            const bool viewport_changed =
                viewport_key != 0 && previous_viewport != viewport_key;
            // Read mode field at offset +32 (DLSSGMode: 0=eOff, 1=eOn, 2=eAuto)
            uint32_t mode = (std::min)(
                *reinterpret_cast<const uint32_t*>(
                    reinterpret_cast<const uint8_t*>(opts) + 32),
                3u);
            bool mode_changed = false;

            // Only update g_fg_mode from Streamline if the user hasn't manually
            // enabled Dynamic MFG passthrough. The user toggle takes priority.
            if (!g_config.dynamic_mfg_passthrough) {
                const uint32_t s_prev_mode = s_fg_mode_observed.load(std::memory_order_relaxed)
                    ? static_cast<uint32_t>(g_fg_mode.load(std::memory_order_relaxed)) : UINT32_MAX;
                mode_changed = mode != s_prev_mode;
                g_fg_mode.store(static_cast<int>(mode), std::memory_order_relaxed);
                s_fg_mode_observed.store(true, std::memory_order_release);
                if (mode_changed) {
                    const char* mode_str = (mode == 0) ? "Off"
                                         : (mode == 1) ? "On"
                                         : (mode == 2) ? "Auto"
                                         : (mode == 3) ? "Dynamic"
                                         : "Unknown";
                    LOG_INFO("DLSSG mode changed: %u -> %u (%s)%s",
                             s_prev_mode == UINT32_MAX ? 0 : s_prev_mode, mode, mode_str,
                             mode == 2 ? " — Dynamic MFG active" : "");
                    OnFGStateChange();
                }
            }
            ApplyExplicitFGModeTransition(mode_changed, mode);

            int numFrames = std::clamp(
                *reinterpret_cast<const int*>(
                    reinterpret_cast<const uint8_t*>(opts) + 36),
                0,
                5);
            int prev = g_fg_multiplier.exchange(numFrames, std::memory_order_relaxed);
            s_setoptions_ever_called.store(true, std::memory_order_relaxed);
            const bool multiplier_changed = numFrames != prev;
            if (mode_changed || multiplier_changed || viewport_changed)
                StreamlineHandoff_Invalidate(mode_changed ? "fg_mode_changed" : multiplier_changed ? "fg_multiplier_changed" : "fg_viewport_changed");
            UpdateConfiguredFGActivity(mode_changed || multiplier_changed || viewport_changed, mode, numFrames);
            if (multiplier_changed) {
                LOG_INFO("FG multiplier changed: %d -> %d (divisor=%d)", prev, numFrames, numFrames + 1);

                if (numFrames == 0) {
                    // FG disabled via SetOptions.
                    // If NGX just created FG (within last 2 seconds), the game may be
                    // sending contradictory signals — trust NGX and keep FG active.
                    // Otherwise, honor the disable.
                    CancelDeferredFGInference();
                    bool prev_presenting = g_fg_presenting.load(std::memory_order_relaxed);
                    if (prev_presenting) {
                        if (NGXHooks_IsFGCreated()) {
                            LOG_INFO("FG presenting: SetOptions says off but NGX FG active — keeping");
                        } else {
                            g_fg_presenting.store(false, std::memory_order_relaxed);
                            g_fg_actual_multiplier.store(0, std::memory_order_relaxed);
                            LOG_INFO("FG presenting (from SetOptions): yes -> no");
                        }
                    }
                }

                OnFGStateChange();
            }
            if (mode != 0 && numFrames > 0 &&
                !g_fg_presenting.load(std::memory_order_relaxed) &&
                (mode_changed || multiplier_changed || viewport_changed)) {
                BeginDeferredFGInference(viewport_key, false);
            }
        }
        EndOptionsStateWrite(options_write_sequence);
        options_write_sequence = 0;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        if (options_write_sequence != 0) {
            EndOptionsStateWrite(options_write_sequence);
        }
        LOG_WARN("Detour_SetOptions: SEH exception caught, skipping state read");
    }
    return result;
    }
    __finally {
        RuntimeLifecycle_LeaveCallback();
    }
}

static sl_Result __cdecl Detour_GetState(const void* vp, void* state, const void* options) {
    const bool runtime_stopping = RuntimeLifecycle_EnterCallback();
    __try {
    if (runtime_stopping)
        return s_orig_GetState.load(std::memory_order_acquire)
            ? s_orig_GetState.load(std::memory_order_acquire)(vp, state, options) : -1;

    sl_Result result;
    __try {
        result = s_orig_GetState.load(std::memory_order_acquire)(vp, state, options);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        LOG_WARN("Detour_GetState: SEH exception in trampoline (0x%08X)", GetExceptionCode());
        return -1;
    }

    // Never hold ReLimiter publication ownership across the foreign call.
    // Capture a stable logical viewport and options generation only for
    // post-call state extraction.
    const uint64_t viewport_key = GetViewportKey(vp);
    const uint64_t options_sequence =
        s_options_publication_sequence.load(std::memory_order_acquire);
    bool options_state_reader = false;
    if (viewport_key != 0 && (options_sequence & 1u) == 0 &&
        s_setoptions_ever_called.load(std::memory_order_relaxed) &&
        s_configured_viewport.load(std::memory_order_relaxed) == viewport_key) {
        s_options_state_readers.fetch_add(1, std::memory_order_acq_rel);
        if (options_sequence == s_options_publication_sequence.load(
                std::memory_order_acquire) &&
            s_configured_viewport.load(std::memory_order_relaxed) == viewport_key) {
            options_state_reader = true;
        }
        else {
            s_options_state_readers.fetch_sub(1, std::memory_order_release);
        }
    }
    __try {
        if (result == sl_eOk && state) {
            // Only trust GetState if SetOptions has actually configured FG.
            // Games that load Streamline for Reflex only never call SetOptions,
            // so the state struct may contain uninitialized data.
            if (!options_state_reader)
                return result;
            if (options_sequence != s_options_publication_sequence.load(
                    std::memory_order_acquire) ||
                s_configured_viewport.load(std::memory_order_relaxed) != viewport_key) {
                s_options_state_readers.fetch_sub(
                    1,
                    std::memory_order_release);
                options_state_reader = false;
                return result;
            }

            const bool inference_call =
                s_fg_inference_pending.load(std::memory_order_relaxed) &&
                s_fg_inference_viewport.load(std::memory_order_relaxed) == viewport_key;
            if (inference_call) {
                s_inference_getstate_call_count.fetch_add(
                    1,
                    std::memory_order_relaxed);
            }

            const uint8_t* p = reinterpret_cast<const uint8_t*>(state);

            int status = *reinterpret_cast<const int*>(p + 40);
            bool active = (status == 0);
            bool prev_active = g_fg_active.exchange(active, std::memory_order_relaxed);
            if (active != prev_active) {
                LOG_INFO("FG active: %s -> %s", prev_active ? "yes" : "no", active ? "yes" : "no");
                OnFGStateChange();
            }

            // offset 48: numFramesActuallyPresented
            size_t version = *reinterpret_cast<const size_t*>(p + 24);
            if (version >= 1) {
                uint32_t frames_presented = *reinterpret_cast<const uint32_t*>(p + 48);
                bool presenting = (frames_presented > 1);

                // During the FG CreateFeature cooldown, don't let GetState revoke
                // g_fg_presenting. Some games (e.g. Neverness to Everness) call
                // CreateFeature(FG) every frame, and GetState reports
                // numFramesActuallyPresented=1 during the first frame after each
                // creation. Without this guard, fg_presenting toggles on/off every
                // frame, causing constant scheduler flushes and FG tier flipping.
                if (!presenting && NGXHooks_InFGCreateCooldown()) {
                    // Still update the multiplier for telemetry, but don't revoke presenting
                    int clamped_mult = static_cast<int>((std::min)(frames_presented, 6u));
                    g_fg_actual_multiplier.store(clamped_mult, std::memory_order_relaxed);
                } else {
                bool prev_presenting = g_fg_presenting.exchange(presenting, std::memory_order_relaxed);

                // Store actual runtime multiplier, clamped to [0, 6].
                // This is the driver's real dynamic FG ratio — independent of render pacing.
                int clamped_mult = static_cast<int>((std::min)(frames_presented, 6u));
                int prev_mult = g_fg_actual_multiplier.exchange(clamped_mult, std::memory_order_relaxed);
                if (clamped_mult != prev_mult) {
                    LOG_INFO("FG actual multiplier changed: %d -> %d (raw numFramesActuallyPresented=%u)",
                             prev_mult, clamped_mult, frames_presented);
                }

                // If GetState confirms FG is presenting, mark confirmed
                // so the deferred inference from SetOptions is validated.
                if (presenting && inference_call) {
                    s_fg_confirmed_by_getstate.store(true, std::memory_order_relaxed);
                    s_fg_inference_pending.store(false, std::memory_order_relaxed);
                }

                // If GetState says NOT presenting and inference is pending,
                // check if the confirmation window has expired.
                if (!presenting && inference_call &&
                    s_fg_inference_pending.load(std::memory_order_relaxed)) {
                    LARGE_INTEGER now, freq;
                    QueryPerformanceCounter(&now);
                    QueryPerformanceFrequency(&freq);
                    int64_t start = s_fg_inference_start_qpc.load(std::memory_order_relaxed);
                    int64_t elapsed_us = (now.QuadPart - start) * 1000000 / freq.QuadPart;
                    if (elapsed_us > FG_CONFIRMATION_WINDOW_US) {
                        // Window expired, GetState says not presenting — revoke
                        s_fg_inference_pending.store(false, std::memory_order_relaxed);
                        LOG_INFO("FG inference revoked — GetState reports not presenting after %.1fs",
                                 elapsed_us / 1000000.0);
                    }
                }

                if (presenting != prev_presenting) {
                    LOG_INFO("FG presenting: %s -> %s (numFramesActuallyPresented=%u)",
                             prev_presenting ? "yes" : "no", presenting ? "yes" : "no",
                             frames_presented);
                    OnFGStateChange();
                }
                } // end else (not in cooldown)
            } else {
                static bool s_version_warned = false;
                if (!s_version_warned) {
                    s_version_warned = true;
                    LOG_WARN("GetState: version=%zu at offset 24 — "
                             "numFramesActuallyPresented read skipped", version);
                }
            }
        }
        if (options_state_reader) {
            s_options_state_readers.fetch_sub(
                1,
                std::memory_order_release);
            options_state_reader = false;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        if (options_state_reader) {
            s_options_state_readers.fetch_sub(
                1,
                std::memory_order_release);
        }
        LOG_WARN("Detour_GetState: SEH exception caught, skipping state read");
    }
    return result;
    }
    __finally {
        RuntimeLifecycle_LeaveCallback();
    }
}

// ── Detour: slGetFeatureFunction — use MinHook on resolved pointers ──
// ── Streamline Compatibility: passive FG state observation ──
// Compatibility mode never calls non-thread-safe slDLSSGGetState out of band.
// It observes the game's SetOptions call and uses deferred/Reflex evidence.
static std::atomic<uint64_t> s_captured_viewport{0};

// Compat-mode SetOptions detour — only captures viewport, forwards unmodified
static sl_Result __cdecl Detour_SetOptions_Compat(const void* vp, const void* opts) {
    const bool runtime_stopping = RuntimeLifecycle_EnterCallback();
    __try {
    if (runtime_stopping)
        return s_orig_SetOptions.load(std::memory_order_acquire)
            ? s_orig_SetOptions.load(std::memory_order_acquire)(vp, opts) : -1;

    const uint64_t lifetime_generation =
        s_streamline_lifetime_generation.load(std::memory_order_acquire);
    // Forward first; publication ownership is only for local post-call state.
    sl_Result result;
    __try {
        result = s_orig_SetOptions.load(std::memory_order_acquire)(vp, opts);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        LOG_WARN("Detour_SetOptions_Compat: SEH exception (0x%08X)", GetExceptionCode());
        return -1;
    }

    uint64_t options_write_sequence = BeginOptionsStateWrite();
    if (lifetime_generation !=
        s_streamline_lifetime_generation.load(std::memory_order_acquire)) {
        EndOptionsStateWrite(options_write_sequence);
        return result;
    }
    // Read FG state from opts (same as normal Detour_SetOptions)
    __try {
        if (result == sl_eOk && opts) {
            const uint64_t viewport_key = GetViewportKey(vp);
            const uint64_t previous_viewport =
                s_captured_viewport.exchange(
                    viewport_key, std::memory_order_relaxed);
            s_configured_viewport.store(
                viewport_key, std::memory_order_relaxed);
            const bool viewport_changed =
                viewport_key != 0 && previous_viewport != viewport_key;
            if (viewport_changed) {
                LOG_INFO(
                    "Streamline compat: logical viewport updated (%llu)",
                    viewport_key - 1);
            }
            uint32_t mode = (std::min)(
                *reinterpret_cast<const uint32_t*>(
                    reinterpret_cast<const uint8_t*>(opts) + 32),
                3u);
            bool mode_changed = false;
            if (!g_config.dynamic_mfg_passthrough) {
                const uint32_t s_prev_mode = s_fg_mode_observed.load(std::memory_order_relaxed)
                    ? static_cast<uint32_t>(g_fg_mode.load(std::memory_order_relaxed)) : UINT32_MAX;
                mode_changed = mode != s_prev_mode;
                g_fg_mode.store(static_cast<int>(mode), std::memory_order_relaxed);
                s_fg_mode_observed.store(true, std::memory_order_release);
                if (mode_changed) {
                    const char* mode_str = (mode == 0) ? "Off"
                                         : (mode == 1) ? "On"
                                         : (mode == 2) ? "Auto"
                                         : (mode == 3) ? "Dynamic"
                                         : "Unknown";
                    LOG_INFO("DLSSG mode changed (compat): %u -> %u (%s)",
                             s_prev_mode == UINT32_MAX ? 0 : s_prev_mode, mode, mode_str);
                    OnFGStateChange();
                }
            }
            ApplyExplicitFGModeTransition(mode_changed, mode);

            int numFrames = std::clamp(
                *reinterpret_cast<const int*>(
                    reinterpret_cast<const uint8_t*>(opts) + 36),
                0,
                5);
            int prev = g_fg_multiplier.exchange(numFrames, std::memory_order_relaxed);
            s_setoptions_ever_called.store(true, std::memory_order_relaxed);
            const bool multiplier_changed = numFrames != prev;
            if (mode_changed || multiplier_changed || viewport_changed)
                StreamlineHandoff_Invalidate(mode_changed ? "fg_mode_changed" : multiplier_changed ? "fg_multiplier_changed" : "fg_viewport_changed");
            UpdateConfiguredFGActivity(mode_changed || multiplier_changed || viewport_changed, mode, numFrames);
            if (multiplier_changed) {
                LOG_INFO("FG multiplier changed (compat): %d -> %d (divisor=%d)",
                         prev, numFrames, numFrames + 1);
                if (numFrames == 0) {
                    CancelDeferredFGInference();
                    if (!NGXHooks_IsFGCreated()) {
                        g_fg_presenting.store(false, std::memory_order_relaxed);
                        g_fg_actual_multiplier.store(0, std::memory_order_relaxed);
                    }
                }
                OnFGStateChange();
            }
            if (mode != 0 && numFrames > 0 &&
                !g_fg_presenting.load(std::memory_order_relaxed) &&
                (mode_changed || multiplier_changed || viewport_changed)) {
                BeginDeferredFGInference(viewport_key, true);
            }
        }
        EndOptionsStateWrite(options_write_sequence);
        options_write_sequence = 0;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        if (options_write_sequence != 0) {
            EndOptionsStateWrite(options_write_sequence);
        }
    }

    return result;
    }
    __finally {
        RuntimeLifecycle_LeaveCallback();
    }
}

bool Streamline_HasModeData() {
    return s_fg_mode_observed.load(std::memory_order_acquire);
}

void Streamline_ResetLifetime() {
    StreamlineHandoff_Invalidate("streamline_lifetime_reset");
    s_streamline_lifetime_generation.fetch_add(1, std::memory_order_acq_rel);
    const uint64_t write_sequence = BeginOptionsStateWrite();
    s_setoptions_ever_called.store(false, std::memory_order_relaxed);
    s_configured_viewport.store(0, std::memory_order_relaxed);
    s_captured_viewport.store(0, std::memory_order_relaxed);
    s_fg_inference_pending.store(false, std::memory_order_relaxed);
    s_fg_inference_start_qpc.store(0, std::memory_order_relaxed);
    s_fg_inference_viewport.store(0, std::memory_order_relaxed);
    s_fg_confirmed_by_getstate.store(false, std::memory_order_relaxed);
    s_inference_getstate_call_count.store(0, std::memory_order_relaxed);
    s_fg_mode_observed.store(false, std::memory_order_relaxed);
    g_fg_mode.store(0, std::memory_order_relaxed);
    g_fg_multiplier.store(0, std::memory_order_relaxed);
    g_fg_actual_multiplier.store(0, std::memory_order_relaxed);
    g_fg_active.store(false, std::memory_order_relaxed);
    g_fg_presenting.store(false, std::memory_order_relaxed);
    EndOptionsStateWrite(write_sequence);
    NGXHooks_ClearFGCreated();
}

void StreamlineCompat_Poll() {
    // Intentional no-op. Compatibility mode observes the game's SetOptions
    // boundary and never calls non-thread-safe GetState independently.
}
static sl_Result __cdecl Detour_slGetFeatureFunction(uint32_t feature, const char* name, void** outPtr) {
    const bool runtime_stopping = RuntimeLifecycle_EnterCallback();
    __try {
    if (runtime_stopping) {
        return s_orig_slGetFeatureFunction.load(std::memory_order_acquire)
            ? s_orig_slGetFeatureFunction.load(std::memory_order_acquire)(feature, name, outPtr)
            : -1;
    }

    sl_Result result;
    __try {
        result = s_orig_slGetFeatureFunction.load(std::memory_order_acquire)(feature, name, outPtr);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        LOG_WARN("Detour_slGetFeatureFunction: SEH exception in trampoline (0x%08X)", GetExceptionCode());
        return -1;
    }
    if (result != sl_eOk || !outPtr || !*outPtr) return result;

    // This entry can itself be called from DllMain. Never install here or
    // publish into the same original slot from competing lazy callers. The
    // worker re-resolves the address instead of retaining an unpinned pointer.
    if (name &&
        ((strcmp(name, "slDLSSGSetOptions") == 0 &&
          !s_orig_SetOptions.load(std::memory_order_acquire)) ||
         (strcmp(name, "slDLSSGGetState") == 0 &&
          !s_orig_GetState.load(std::memory_order_acquire)))) {
        LoadLibraryHooks_RequestInstall();
    }

    return result;
    }
    __finally {
        RuntimeLifecycle_LeaveCallback();
    }
}

// ── Deferred FG inference check (called each frame from scheduler) ──
void CheckDeferredFGInference() {
    const uint64_t options_sequence =
        s_options_publication_sequence.load(std::memory_order_acquire);
    if ((options_sequence & 1u) != 0) return;

    s_options_state_readers.fetch_add(1, std::memory_order_acq_rel);
    if (options_sequence != s_options_publication_sequence.load(
            std::memory_order_acquire)) {
        s_options_state_readers.fetch_sub(1, std::memory_order_release);
        return;
    }
    const bool pending =
        s_fg_inference_pending.load(std::memory_order_relaxed);
    const int64_t expected_start =
        s_fg_inference_start_qpc.load(std::memory_order_relaxed);
    const uint64_t expected_viewport =
        s_fg_inference_viewport.load(std::memory_order_relaxed);
    s_options_state_readers.fetch_sub(1, std::memory_order_release);
    if (!pending || expected_start <= 0 || !expected_viewport) return;

    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    if (freq.QuadPart <= 0 || now.QuadPart <= expected_start) return;
    const double elapsed_us =
        static_cast<double>(now.QuadPart - expected_start) * 1'000'000.0 /
        static_cast<double>(freq.QuadPart);
    if (!std::isfinite(elapsed_us) ||
        elapsed_us <= static_cast<double>(FG_CONFIRMATION_WINDOW_US)) return;

    const uint64_t write_sequence = BeginOptionsStateWrite();
    bool promoted = false;
    uint32_t window_getstate_calls = 0;
    bool vulkan_without_evidence_source = false;
    bool compat_without_evidence = false;
    if (s_fg_inference_pending.load(std::memory_order_relaxed) &&
        s_fg_inference_start_qpc.load(std::memory_order_relaxed) ==
            expected_start &&
        s_fg_inference_viewport.load(std::memory_order_relaxed) ==
            expected_viewport) {
        window_getstate_calls =
            s_inference_getstate_call_count.load(std::memory_order_relaxed);
        const bool confirmed =
            s_fg_confirmed_by_getstate.load(std::memory_order_relaxed);
        const bool passive_runtime_evidence =
            NGXHooks_IsFGCreated() || GetCurrentReflexAiFrameTimeUs() > 0.0;
        // NGX FG creation and the Reflex AI frame time are observed only
        // through D3D. On Vulkan neither can arrive, so their absence is no
        // evidence against frame generation: trust SetOptions after the window.
        vulkan_without_evidence_source = SwapMgr_GetActiveAPI() == ActiveAPI::Vulkan;
        s_fg_inference_pending.store(false, std::memory_order_relaxed);
        s_fg_inference_viewport.store(0, std::memory_order_relaxed);
        if (!confirmed && window_getstate_calls == 0) {
            if (!g_config.streamline_compat || passive_runtime_evidence ||
                vulkan_without_evidence_source) {
                g_fg_presenting.store(true, std::memory_order_relaxed);
                promoted = true;
            } else {
                compat_without_evidence = true;
            }
        }
    }
    EndOptionsStateWrite(write_sequence);

    if (promoted) {
        LOG_INFO(!g_config.streamline_compat
            ? "FG presenting promoted (no GetState calls observed in confirmation window, %.1fs elapsed)"
            : vulkan_without_evidence_source
                ? "FG presenting promoted (compatibility mode on Vulkan, which has no NGX or Reflex AI evidence; "
                  "SetOptions trusted, %.1fs elapsed)"
                : "FG presenting promoted (passive runtime evidence; GetState not observed in compatibility mode, "
                  "%.1fs elapsed)",
            elapsed_us / 1000000.0);
        OnFGStateChange();
    } else if (compat_without_evidence) {
        LOG_INFO("FG presenting NOT promoted (compatibility mode) — no NGX FG creation or Reflex AI frame time "
                 "within %.1fs", elapsed_us / 1000000.0);
    } else if (window_getstate_calls > 0) {
        LOG_INFO("FG presenting NOT promoted — GetState called %u times but never confirmed (%.1fs)",
                 window_getstate_calls, elapsed_us / 1000000.0);
    }
}

// ── Entry point: owned installer worker only, never under loader lock ──
bool HookStreamlinePCL(HMODULE hInterposer) {
    // Only the loader-install worker writes these. Original slots are atomic
    // because game detours read them immediately after each target is enabled.
    static bool feature_installed = false;
    static bool options_installed = false;
    static bool state_installed = false;
    static void* bound_options = nullptr;
    static void* bound_state = nullptr;
    static bool replacement_reported = false;
    static bool options_failure_logged = false;
    static bool state_failure_logged = false;
    if (RuntimeLifecycle_IsStopping()) return false;

    auto pGetFeatureFunction = reinterpret_cast<PFN_slGetFeatureFunction>(
        GetProcAddress(hInterposer, "slGetFeatureFunction"));
    if (!pGetFeatureFunction) return false;

    // Inside a batch InstallHook only queues: each group below is enabled by one flush, and the flags, logs and
    // bound pointers are read from Hooks_TargetEnabled afterwards, never from the queue status.
    void* const feature_target = reinterpret_cast<void*>(pGetFeatureFunction);
    if (!feature_installed) {
        const MH_STATUS status = InstallHook(
            feature_target,
            reinterpret_cast<void*>(&Detour_slGetFeatureFunction),
            &s_orig_slGetFeatureFunction);
        Hooks_FlushBatch();
        feature_installed = status == MH_OK && Hooks_TargetEnabled(feature_target);
        if (!feature_installed) return false;
    }

    const auto resolve = s_orig_slGetFeatureFunction.load(std::memory_order_acquire);
    // Keep observing the selected implementation after initial installation.
    // Resolving GetState is safe in compatibility mode; invoking/hooking it is
    // still prohibited there. Discovery does not authorize private GPU hooks.
    const auto functions = StreamlineDiscovery_Resolve(hInterposer, resolve);
    // DLSS-G resolves only after slInit loaded its plugin.
    const bool ready = functions.options_result == sl_eOk && functions.set_options != nullptr;
    if (!replacement_reported &&
        ((bound_options && functions.set_options && bound_options != functions.set_options) ||
         (bound_state && functions.get_state && bound_state != functions.get_state))) {
        replacement_reported = true;
        StreamlineQueue_BlockFeature();
        LOG_WARN("Streamline: selected feature implementation changed; existing public hooks retained, "
            "not rebound; generic GPU binding disabled until restart.");
    }
    if (options_installed && (g_config.streamline_compat || state_installed)) return ready;
    void* options_target = nullptr;
    void* state_target = nullptr;
    MH_STATUS options_status = MH_OK;
    MH_STATUS state_status = MH_OK;
    if (!options_installed) {
        void* fn = functions.set_options;
        sl_Result r = functions.options_result;
        if (r == sl_eOk && fn) {
            options_target = fn;
            options_status = InstallHook(fn,
                g_config.streamline_compat
                    ? reinterpret_cast<void*>(&Detour_SetOptions_Compat)
                    : reinterpret_cast<void*>(&Detour_SetOptions),
                &s_orig_SetOptions);
        }
    }
    // Compatibility mode must not invoke GetState independently or hook it.
    if (!g_config.streamline_compat && !state_installed) {
        void* fn = functions.get_state;
        sl_Result r = functions.state_result;
        if (r == sl_eOk && fn) {
            state_target = fn;
            state_status = InstallHook(fn, reinterpret_cast<void*>(&Detour_GetState), &s_orig_GetState);
        }
    }
    Hooks_FlushBatch();
    if (options_target) {
        options_installed = options_status == MH_OK && Hooks_TargetEnabled(options_target);
        if (options_installed) {
            bound_options = options_target;
            LOG_INFO("Streamline: hooked slDLSSGSetOptions");
        } else if (!options_failure_logged) {
            options_failure_logged = true;
            LOG_WARN("Streamline: slDLSSGSetOptions at %p could not be hooked (%s); frame generation "
                "is not detected through Streamline. Retrying.", options_target,
                options_status == MH_OK ? "enable failed" : MH_StatusToString(options_status));
        }
    }
    if (state_target) {
        state_installed = state_status == MH_OK && Hooks_TargetEnabled(state_target);
        if (state_installed) {
            bound_state = state_target;
            LOG_INFO("Streamline: hooked slDLSSGGetState");
        } else if (!state_failure_logged) {
            state_failure_logged = true;
            LOG_WARN("Streamline: slDLSSGGetState at %p could not be hooked (%s). Retrying.",
                state_target, state_status == MH_OK ? "enable failed" : MH_StatusToString(state_status));
        }
    }
    return ready;
}

// ── DMFG detection functions ──

bool IsFGDllLoaded() {
    if (GetModuleHandleW(L"nvngx_dlssg.dll"))  return true;
    if (GetModuleHandleW(L"_nvngx_dlssg.dll")) return true;
    if (GetModuleHandleW(L"sl.dlss_g.dll"))    return true;
    if (GetModuleHandleW(L"dlss-g.dll"))       return true;
    return false;
}

bool IsDmfgSession() {
    return g_game_requested_latency.load(std::memory_order_relaxed) >= 4
        && !IsFGDllLoaded();
}

bool IsDmfgActive() {
    // Auto-detect from DRS profile: Mode Override == 4 means Dynamic MFG is set.
    // Falls back to manual toggle for backward compatibility with users who have
    // dynamic_mfg_passthrough=true in their INI from older versions.
    DLSSPresets p = DLSSPresets_Get();
    if (p.available && p.mfg_mode_override == 4)
        return true;
    return g_config.dynamic_mfg_passthrough;
}

bool IsNvSmoothMotionActive() {
    return GetModuleHandleW(L"nvpresent64.dll") != nullptr;
}
