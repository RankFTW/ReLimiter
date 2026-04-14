#include "streamline_hooks.h"
#include "hooks.h"
#include "config.h"
#include "flush.h"
#include "dlss_ngx_interceptor.h"
#include "logger.h"
#include <cstring>

// ── Shared FG state ──
std::atomic<int>  g_fg_multiplier{0};
std::atomic<bool> g_fg_active{false};
std::atomic<bool> g_fg_presenting{false};

// DMFG state — DLSSGMode: 0=eOff, 1=eOn (static FG), 2=eAuto (Dynamic MFG)
std::atomic<int>  g_fg_mode{0};

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
static PFN_slGetFeatureFunction s_orig_slGetFeatureFunction = nullptr;
static PFN_slDLSSGSetOptions    s_orig_SetOptions           = nullptr;
static PFN_slDLSSGGetState      s_orig_GetState             = nullptr;

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
// Reflex only, e.g. MH Stories 3). For games that never call GetState
// (e.g. HFW), the inference sticks after the window.
static std::atomic<bool> s_fg_inference_pending{false};
static std::atomic<int64_t> s_fg_inference_start_qpc{0};
static std::atomic<bool> s_fg_confirmed_by_getstate{false};
static std::atomic<uint32_t> s_getstate_call_count{0};
static constexpr int64_t FG_CONFIRMATION_WINDOW_US = 3000000; // 3 seconds

static sl_Result __cdecl Detour_SetOptions(const void* vp, const void* opts) {
    sl_Result result;
    __try {
        result = s_orig_SetOptions(vp, opts);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        LOG_WARN("Detour_SetOptions: SEH exception in trampoline (0x%08X)", GetExceptionCode());
        return -1;
    }
    __try {
        if (result == sl_eOk && opts) {
            // Read mode field at offset +32 (DLSSGMode: 0=eOff, 1=eOn, 2=eAuto)
            uint32_t mode = *reinterpret_cast<const uint32_t*>(
                reinterpret_cast<const uint8_t*>(opts) + 32);

            // Only update g_fg_mode from Streamline if the user hasn't manually
            // enabled Dynamic MFG passthrough. The user toggle takes priority.
            if (!g_config.dynamic_mfg_passthrough) {
                static uint32_t s_prev_mode = UINT32_MAX;
                g_fg_mode.store(static_cast<int>(mode), std::memory_order_relaxed);
                if (mode != s_prev_mode) {
                    const char* mode_str = (mode == 0) ? "Off"
                                         : (mode == 1) ? "On"
                                         : (mode == 2) ? "Auto(Dynamic)"
                                         : "Unknown";
                    LOG_INFO("DLSSG mode changed: %u -> %u (%s)%s",
                             s_prev_mode == UINT32_MAX ? 0 : s_prev_mode, mode, mode_str,
                             mode == 2 ? " — Dynamic MFG active" : "");
                    s_prev_mode = mode;
                }
            }

            int numFrames = *reinterpret_cast<const int*>(
                reinterpret_cast<const uint8_t*>(opts) + 36);
            int prev = g_fg_multiplier.exchange(numFrames, std::memory_order_relaxed);
            s_setoptions_ever_called.store(true, std::memory_order_relaxed);
            if (numFrames != prev) {
                LOG_INFO("FG multiplier changed: %d -> %d (divisor=%d)", prev, numFrames, numFrames + 1);

                // Set active state from SetOptions — the game configured FG.
                bool inferred_active = (numFrames > 0);
                bool prev_active = g_fg_active.load(std::memory_order_relaxed);
                if (inferred_active != prev_active) {
                    g_fg_active.store(inferred_active, std::memory_order_relaxed);
                    LOG_INFO("FG active (inferred from SetOptions): %s -> %s",
                             prev_active ? "yes" : "no",
                             inferred_active ? "yes" : "no");
                }

                // Don't set g_fg_presenting immediately — defer to GetState
                // confirmation. Games like MH Stories 3 call SetOptions with
                // numFrames=1 internally but never actually produce FG frames.
                // Start a confirmation window; if GetState doesn't confirm
                // within it, presenting stays false.
                if (numFrames > 0 && !g_fg_presenting.load(std::memory_order_relaxed)) {
                    LARGE_INTEGER now;
                    QueryPerformanceCounter(&now);
                    s_fg_inference_start_qpc.store(now.QuadPart, std::memory_order_relaxed);
                    s_fg_inference_pending.store(true, std::memory_order_relaxed);
                    s_fg_confirmed_by_getstate.store(false, std::memory_order_relaxed);
                    LOG_INFO("FG presenting deferred — waiting for GetState confirmation");
                } else if (numFrames == 0) {
                    // FG disabled — clear everything
                    s_fg_inference_pending.store(false, std::memory_order_relaxed);
                    bool prev_presenting = g_fg_presenting.load(std::memory_order_relaxed);
                    if (prev_presenting) {
                        g_fg_presenting.store(false, std::memory_order_relaxed);
                        LOG_INFO("FG presenting (from SetOptions): yes -> no");
                    }
                }

                OnFGStateChange();
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        LOG_WARN("Detour_SetOptions: SEH exception caught, skipping state read");
    }
    return result;
}

static sl_Result __cdecl Detour_GetState(const void* vp, void* state, const void* options) {
    sl_Result result;
    __try {
        result = s_orig_GetState(vp, state, options);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        LOG_WARN("Detour_GetState: SEH exception in trampoline (0x%08X)", GetExceptionCode());
        return -1;
    }
    __try {
        if (result == sl_eOk && state) {
            // Only trust GetState if SetOptions has actually configured FG.
            // Games that load Streamline for Reflex only never call SetOptions,
            // so the state struct may contain uninitialized data.
            if (!s_setoptions_ever_called.load(std::memory_order_relaxed))
                return result;

            s_getstate_call_count.fetch_add(1, std::memory_order_relaxed);

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
                if (presenting && s_fg_inference_pending.load(std::memory_order_relaxed)) {
                    s_fg_confirmed_by_getstate.store(true, std::memory_order_relaxed);
                    s_fg_inference_pending.store(false, std::memory_order_relaxed);
                }

                // If GetState says NOT presenting and inference is pending,
                // check if the confirmation window has expired.
                if (!presenting && s_fg_inference_pending.load(std::memory_order_relaxed)) {
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
            } else {
                static bool s_version_warned = false;
                if (!s_version_warned) {
                    s_version_warned = true;
                    LOG_WARN("GetState: version=%zu at offset 24 — "
                             "numFramesActuallyPresented read skipped", version);
                }
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        LOG_WARN("Detour_GetState: SEH exception caught, skipping state read");
    }
    return result;
}

// ── Detour: slGetFeatureFunction — use MinHook on resolved pointers ──
// Guard against double-call: only hook each function once.
static sl_Result __cdecl Detour_slGetFeatureFunction(uint32_t feature, const char* name, void** outPtr) {
    sl_Result result;
    __try {
        result = s_orig_slGetFeatureFunction(feature, name, outPtr);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        LOG_WARN("Detour_slGetFeatureFunction: SEH exception in trampoline (0x%08X)", GetExceptionCode());
        return -1;
    }
    if (result != sl_eOk || !outPtr || !*outPtr) return result;

    if (name && strcmp(name, "slDLSSGSetOptions") == 0 && !s_orig_SetOptions) {
        InstallHook(*outPtr, reinterpret_cast<void*>(&Detour_SetOptions),
                    reinterpret_cast<void**>(&s_orig_SetOptions));
    }
    else if (name && strcmp(name, "slDLSSGGetState") == 0 && !s_orig_GetState) {
        InstallHook(*outPtr, reinterpret_cast<void*>(&Detour_GetState),
                    reinterpret_cast<void**>(&s_orig_GetState));
    }

    // ── Adaptive DLSS Scaling: hook slDLSSSetOptions for DLSS-SR ──
    // When the game resolves slDLSSSetOptions, we hook it to override
    // outputWidth/outputHeight for the k*D upscale.
    if (name && strcmp(name, "slDLSSSetOptions") == 0) {
        NGXInterceptor_HookDLSSSetOptions(*outPtr);
    }

    return result;
}

// ── Deferred FG inference check (called each frame from scheduler) ──
void CheckDeferredFGInference() {
    if (!s_fg_inference_pending.load(std::memory_order_relaxed))
        return;

    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    int64_t start = s_fg_inference_start_qpc.load(std::memory_order_relaxed);
    int64_t elapsed_us = (now.QuadPart - start) * 1000000 / freq.QuadPart;

    if (elapsed_us > FG_CONFIRMATION_WINDOW_US) {
        s_fg_inference_pending.store(false, std::memory_order_relaxed);

        if (s_fg_confirmed_by_getstate.load(std::memory_order_relaxed)) {
            // GetState confirmed — already set, nothing to do
            return;
        }

        // Window expired without GetState confirming.
        // Check: did GetState ever fire during the window?
        // If not, promote the inference (HFW case — game never calls GetState).
        // If GetState did fire and said not presenting, don't promote (MH Stories 3).
        uint32_t getstate_calls = s_getstate_call_count.load(std::memory_order_relaxed);
        if (getstate_calls == 0) {
            // GetState was never called — game doesn't poll it.
            // Promote the inference (HFW case).
            g_fg_presenting.store(true, std::memory_order_relaxed);
            LOG_INFO("FG presenting promoted (GetState never called, %.1fs elapsed)",
                     elapsed_us / 1000000.0);
            OnFGStateChange();
        } else {
            // GetState IS being called but never confirmed presenting.
            // This is the MH Stories 3 case — revoke.
            LOG_INFO("FG presenting NOT promoted — GetState called %u times but never confirmed (%.1fs)",
                     getstate_calls, elapsed_us / 1000000.0);
        }
    }
}

// ── Entry point: called when sl.interposer.dll is loaded ──
void HookStreamlinePCL(HMODULE hInterposer) {
    auto pGetFeatureFunction = reinterpret_cast<PFN_slGetFeatureFunction>(
        GetProcAddress(hInterposer, "slGetFeatureFunction"));
    if (!pGetFeatureFunction) return;

    InstallHook(
        reinterpret_cast<void*>(pGetFeatureFunction),
        reinterpret_cast<void*>(&Detour_slGetFeatureFunction),
        reinterpret_cast<void**>(&s_orig_slGetFeatureFunction));

    // Proactively resolve SetOptions/GetState in case the game already
    // called slGetFeatureFunction before we hooked it (deferred hook
    // installation means Streamline is already fully initialized).
    if (s_orig_slGetFeatureFunction && !s_orig_SetOptions) {
        void* fn = nullptr;
        sl_Result r = s_orig_slGetFeatureFunction(1000 /*eDLSS_G*/, "slDLSSGSetOptions", &fn);
        if (r == sl_eOk && fn) {
            InstallHook(fn, reinterpret_cast<void*>(&Detour_SetOptions),
                        reinterpret_cast<void**>(&s_orig_SetOptions));
            LOG_INFO("Streamline: proactively hooked slDLSSGSetOptions");
        }
    }
    if (s_orig_slGetFeatureFunction && !s_orig_GetState) {
        void* fn = nullptr;
        sl_Result r = s_orig_slGetFeatureFunction(1000 /*eDLSS_G*/, "slDLSSGGetState", &fn);
        if (r == sl_eOk && fn) {
            InstallHook(fn, reinterpret_cast<void*>(&Detour_GetState),
                        reinterpret_cast<void**>(&s_orig_GetState));
            LOG_INFO("Streamline: proactively hooked slDLSSGGetState");
        }
    }

    // ── Adaptive DLSS Scaling: proactively resolve slDLSSSetOptions ──
    if (s_orig_slGetFeatureFunction) {
        void* fn = nullptr;
        sl_Result r = s_orig_slGetFeatureFunction(0 /*kFeatureDLSS*/, "slDLSSSetOptions", &fn);
        if (r == sl_eOk && fn) {
            NGXInterceptor_HookDLSSSetOptions(fn);
        }
    }
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
    return g_fg_mode.load(std::memory_order_relaxed) == 2
        || IsDmfgSession();
}

bool IsNvSmoothMotionActive() {
    return GetModuleHandleW(L"nvpresent64.dll") != nullptr;
}
