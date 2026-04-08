#include "tier.h"
#include "health.h"
#include "correlator.h"
#include "swapchain_manager.h"
#include "enforcement_dispatcher.h"
#include "logger.h"

// File-static T3 diagnostic counter — reset on recovery so we log again
// after each transition, not just the first 3 ever.
static int s_t3_diag_count = 0;

Tier CheckTier() {
    // Tier 4: Suspended
    if (!IsSwapchainValid())
        return Tier4;

    // Tier 3: Safety mode — no markers
    if (!AreMarkersFlowing()) {
        if (s_t3_diag_count++ < 3)
            LOG_WARN("Tier3: markers not flowing (swapchain=%d, nvapi=%d)",
                     IsSwapchainValid(), IsNvAPIAvailable());
        return Tier3;
    }

    // Recovered from T3 — reset diagnostic counter for next episode
    s_t3_diag_count = 0;

    // Vulkan/DX11 path: no DXGI correlator or DXGI stats — skip T2b/T2a/T1 checks.
    // Marker-based enforcement (NvAPI/PCL) also skips the correlator — scanout
    // estimation is too noisy with FG. These paths get T2a (stale feedback)
    // which is correct: pacing works fine without correlator data, the tier
    // just reflects that scanout feedback isn't available.
    ActiveAPI active_api = SwapMgr_GetActiveAPI();
    if (active_api == ActiveAPI::Vulkan || active_api == ActiveAPI::DX11 || active_api == ActiveAPI::OpenGL) {
        return Tier2a;
    }

    // Marker-based paths: correlator intentionally disabled, skip T2b check
    EnforcementPath path = EnfDisp_GetActivePath();
    if (path == EnforcementPath::NvAPIMarkers || path == EnforcementPath::PCLMarkers) {
        if (!IsNvAPIAvailable())
            return Tier1;
        return Tier0;
    }

    // Tier 2b: Invalid feedback — DXGI stats unavailable
    if (!IsCorrelatorValid())
        return Tier2b;

    // Tier 2a: Stale feedback — DXGI stats went stale
    if (!IsDXGIStatsFresh())
        return Tier2a;

    // Tier 1: No auxiliary telemetry — NvAPI unavailable
    if (!IsNvAPIAvailable())
        return Tier1;

    // Tier 0: Full
    return Tier0;
}

static const char* TierName(Tier t) {
    switch (t) {
    case Tier0:  return "T0-Full";
    case Tier1:  return "T1-NoAux";
    case Tier2a: return "T2a-Stale";
    case Tier2b: return "T2b-Invalid";
    case Tier3:  return "T3-Safety";
    case Tier4:  return "T4-Suspended";
    }
    return "T?";
}

void OnTierChange(Tier old_tier, Tier new_tier) {
    if (new_tier > old_tier) {
        LOG_WARN("Degraded: %s -> %s", TierName(old_tier), TierName(new_tier));
        if (old_tier <= Tier0 && new_tier >= Tier2a) {
            // Entering degraded feedback — snapshot last good margin
        }
    } else {
        LOG_INFO("Recovered: %s -> %s", TierName(old_tier), TierName(new_tier));
    }
}

void UpdateTier() {
    Tier new_tier = CheckTier();
    if (new_tier != g_current_tier) {
        Tier old_tier = g_current_tier;
        g_current_tier = new_tier;
        OnTierChange(old_tier, new_tier);
    }
}
