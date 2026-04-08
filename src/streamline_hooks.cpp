#include "streamline_hooks.h"
#include "hooks.h"
#include "flush.h"
#include "logger.h"
#include <cstring>

// ── Shared FG state ──
std::atomic<int>  g_fg_multiplier{0};
std::atomic<bool> g_fg_active{false};
std::atomic<bool> g_fg_presenting{false};

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
            int numFrames = *reinterpret_cast<const int*>(
                reinterpret_cast<const uint8_t*>(opts) + 36);
            int prev = g_fg_multiplier.exchange(numFrames, std::memory_order_relaxed);
            if (numFrames != prev) {
                LOG_INFO("FG multiplier changed: %d -> %d (divisor=%d)", prev, numFrames, numFrames + 1);

                // Infer FG presenting state from SetOptions when the game
                // never calls GetState (e.g. Horizon Forbidden West).
                // SetOptions with numFramesToGenerate > 0 is authoritative
                // evidence that FG is enabled. GetState can still refine
                // this if the game does call it later.
                bool inferred_presenting = (numFrames > 0);
                bool prev_presenting = g_fg_presenting.load(std::memory_order_relaxed);
                if (inferred_presenting != prev_presenting) {
                    g_fg_presenting.store(inferred_presenting, std::memory_order_relaxed);
                    LOG_INFO("FG presenting (inferred from SetOptions): %s -> %s",
                             prev_presenting ? "yes" : "no",
                             inferred_presenting ? "yes" : "no");
                }
                bool inferred_active = (numFrames > 0);
                bool prev_active = g_fg_active.load(std::memory_order_relaxed);
                if (inferred_active != prev_active) {
                    g_fg_active.store(inferred_active, std::memory_order_relaxed);
                    LOG_INFO("FG active (inferred from SetOptions): %s -> %s",
                             prev_active ? "yes" : "no",
                             inferred_active ? "yes" : "no");
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

    return result;
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
}
