#include "flip_model.h"
#include "config.h"
#include "logger.h"
#include <Windows.h>
#include <dxgi.h>
#include <dxgi1_5.h>
#include <atomic>

// DXGI swap effect values (from dxgi.h / dxgitype.h)
// DXGI_SWAP_EFFECT_DISCARD         = 0  (bitblt)
// DXGI_SWAP_EFFECT_SEQUENTIAL      = 1  (bitblt)
// DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL = 3  (flip)
// DXGI_SWAP_EFFECT_FLIP_DISCARD    = 4  (flip)

#ifndef DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING
#define DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING 2048
#endif

static std::atomic<bool> s_applied{false};
static std::atomic<bool> s_tearing_supported{false};
static std::atomic<bool> s_tearing_checked{false};

static bool IsBitbltSwapEffect(uint32_t swap_effect) {
    return swap_effect == DXGI_SWAP_EFFECT_DISCARD ||
           swap_effect == DXGI_SWAP_EFFECT_SEQUENTIAL;
}

static const char* SwapEffectName(uint32_t se) {
    switch (se) {
        case DXGI_SWAP_EFFECT_DISCARD:         return "DISCARD (bitblt)";
        case DXGI_SWAP_EFFECT_SEQUENTIAL:      return "SEQUENTIAL (bitblt)";
        case DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL: return "FLIP_SEQUENTIAL";
        case DXGI_SWAP_EFFECT_FLIP_DISCARD:    return "FLIP_DISCARD";
        default:                               return "Unknown";
    }
}

void FlipModel_CheckTearingSupport(void* native_swapchain) {
    if (s_tearing_checked.load(std::memory_order_relaxed)) return;
    s_tearing_checked.store(true, std::memory_order_relaxed);

    if (!native_swapchain) return;

    __try {
        auto* sc = static_cast<IDXGISwapChain*>(native_swapchain);

        // Walk up: SwapChain → Device → Adapter → Factory5
        IDXGIDevice* dxgi_device = nullptr;
        HRESULT hr = sc->GetDevice(__uuidof(IDXGIDevice),
                                    reinterpret_cast<void**>(&dxgi_device));
        if (FAILED(hr) || !dxgi_device) return;

        IDXGIAdapter* adapter = nullptr;
        hr = dxgi_device->GetAdapter(&adapter);
        dxgi_device->Release();
        if (FAILED(hr) || !adapter) return;

        IDXGIFactory5* factory5 = nullptr;
        hr = adapter->GetParent(__uuidof(IDXGIFactory5),
                                 reinterpret_cast<void**>(&factory5));
        adapter->Release();
        if (FAILED(hr) || !factory5) return;

        BOOL allow_tearing = FALSE;
        hr = factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                                            &allow_tearing, sizeof(allow_tearing));
        factory5->Release();

        if (SUCCEEDED(hr) && allow_tearing) {
            s_tearing_supported.store(true, std::memory_order_relaxed);
            LOG_INFO("FlipModel: DXGI factory supports ALLOW_TEARING");
        } else {
            LOG_INFO("FlipModel: DXGI factory does NOT support ALLOW_TEARING (hr=0x%08X, val=%d)",
                     hr, allow_tearing);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_WARN("FlipModel: exception checking tearing support");
    }
}

bool FlipModel_TryUpgrade(uint32_t& swap_effect, uint32_t& flags,
                           uint32_t& buffer_count, bool& fullscreen_state) {
    s_applied.store(false, std::memory_order_relaxed);

    if (!g_config.flip_model_override) return false;

    if (!IsBitbltSwapEffect(swap_effect)) {
        LOG_INFO("FlipModel: swapchain already uses %s (buffers=%u, flags=0x%X), no override needed",
                 SwapEffectName(swap_effect), buffer_count, flags);

        // Even if already flip model, ensure ALLOW_TEARING flag is present for VRR.
        // Some games create flip model swapchains without this flag, which prevents
        // DXGI_PRESENT_ALLOW_TEARING from working in Present() calls.
        if (!(flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING)) {
            flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
            LOG_INFO("FlipModel: added ALLOW_TEARING flag to existing flip model swapchain (flags: 0x%X)",
                     flags);

            // Flip model is incompatible with exclusive fullscreen
            if (fullscreen_state) {
                fullscreen_state = false;
                LOG_INFO("FlipModel: forced windowed mode (tearing flag incompatible with exclusive fullscreen)");
            }

            s_applied.store(true, std::memory_order_relaxed);
            return true;
        }

        return false;
    }

    uint32_t old_effect = swap_effect;
    uint32_t old_buffers = buffer_count;
    uint32_t old_flags = flags;

    // Upgrade to FLIP_DISCARD
    swap_effect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    // Flip model requires at least 2 back buffers
    if (buffer_count < 2)
        buffer_count = 2;

    // Add ALLOW_TEARING flag for VRR support.
    // This flag is required on the swapchain for DXGI_PRESENT_ALLOW_TEARING
    // to work in Present() calls. Without it, VRR won't engage.
    flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

    // Flip model is incompatible with exclusive fullscreen.
    // Force windowed mode — the game can still be borderless fullscreen.
    if (fullscreen_state) {
        fullscreen_state = false;
        LOG_INFO("FlipModel: forced windowed mode (flip model incompatible with exclusive fullscreen)");
    }

    LOG_INFO("FlipModel: upgraded %s → FLIP_DISCARD (buffers: %u→%u, flags: 0x%X→0x%X)",
             SwapEffectName(old_effect), old_buffers, buffer_count, old_flags, flags);

    s_applied.store(true, std::memory_order_relaxed);
    return true;
}

bool FlipModel_WasApplied() {
    return s_applied.load(std::memory_order_relaxed);
}
