#include "frame_latency_controller.h"
#include "streamline_hooks.h"
#include "logger.h"
#include <Windows.h>
#include <dxgi.h>

// ── File-static state ──
static bool s_applied = false;  // true after frame latency set for current swapchain

// ── DX11: IDXGIDevice1::SetMaximumFrameLatency(1) ──
static void ApplyDX11FrameLatency(uint64_t native_handle) {
    auto* sc = reinterpret_cast<IDXGISwapChain*>(native_handle);

    __try {
        // Get the device from the swapchain, then QI for IDXGIDevice1
        IUnknown* deviceUnk = nullptr;
        HRESULT hr = sc->GetDevice(__uuidof(IUnknown), reinterpret_cast<void**>(&deviceUnk));
        if (SUCCEEDED(hr) && deviceUnk) {
            IDXGIDevice1* device1 = nullptr;
            hr = deviceUnk->QueryInterface(__uuidof(IDXGIDevice1),
                                            reinterpret_cast<void**>(&device1));
            deviceUnk->Release();

            if (SUCCEEDED(hr) && device1) {
                hr = device1->SetMaximumFrameLatency(1);
                if (SUCCEEDED(hr))
                    LOG_INFO("FLC: DX11 frame latency set to 1 via IDXGIDevice1");
                else
                    LOG_WARN("FLC: DX11 IDXGIDevice1::SetMaximumFrameLatency failed hr=0x%08X", hr);
                device1->Release();
            } else {
                LOG_WARN("FLC: DX11 QI for IDXGIDevice1 failed hr=0x%08X", hr);
            }
        } else {
            LOG_WARN("FLC: DX11 GetDevice failed hr=0x%08X", hr);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_WARN("FLC: exception in DX11 frame latency path (handle=0x%llX)", native_handle);
    }
}

// ── DX12: skip entirely ──
// DX12 waitable swapchains are not safe to modify — some games (e.g. GoWR)
// crash when SetMaximumFrameLatency changes the queue depth. Non-waitable
// swapchains already rely on Reflex bLowLatencyMode. Skip all DX12 FLC.
static void ApplyDX12FrameLatency() {
    LOG_INFO("FLC: DX12 skipped (queue depth managed by game/Reflex)");
}

// ── Public API ──

void FLC_OnSwapchainInit(uint64_t native_handle, ActiveAPI api) {
    if (s_applied) return;  // already applied for this swapchain lifetime

    if (!native_handle) {
        LOG_WARN("FLC: init called with null handle");
        return;
    }

    // DMFG passthrough: preserve game's requested queue depth
    if (IsDmfgActive()) {
        uint32_t game_lat = g_game_requested_latency.load(std::memory_order_relaxed);
        LOG_INFO("FLC: DMFG passthrough — preserving game latency %u (skipping override to 1)", game_lat);
        s_applied = true;
        return;
    }

    switch (api) {
        case ActiveAPI::DX11:
            ApplyDX11FrameLatency(native_handle);
            break;

        case ActiveAPI::DX12:
            ApplyDX12FrameLatency();
            break;

        case ActiveAPI::Vulkan:
            // No-op: Vulkan does not use DXGI latency APIs
            LOG_INFO("FLC: Vulkan — no DXGI frame latency to apply");
            break;

        case ActiveAPI::OpenGL:
            // No-op: OpenGL does not use DXGI latency APIs
            LOG_INFO("FLC: OpenGL — no DXGI frame latency to apply");
            break;

        case ActiveAPI::None:
            LOG_WARN("FLC: init called with ActiveAPI::None");
            break;
    }

    s_applied = true;  // mark as applied regardless of success (don't retry)
}

void FLC_OnSwapchainDestroy() {
    s_applied = false;
    LOG_INFO("FLC: reset (will re-apply on next swapchain init)");
}
