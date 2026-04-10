#include "frame_latency_controller.h"
#include "logger.h"
#include "streamline_hooks.h"
#include <Windows.h>
#include <dxgi.h>
#include <dxgi1_3.h>

// ── File-static state ──
static bool s_applied = false;  // true after frame latency set for current swapchain

// ── Streamline native interface resolution ──
// slGetNativeInterface signature: int(void* proxy, void** native)
// Returns 0 (sl::Result::eOk) on success.
using slGetNativeInterface_pfn = int (*)(void* proxy, void** native);

/// Resolve slGetNativeInterface from sl.interposer.dll if loaded.
/// Caches the result — only checks once.
static slGetNativeInterface_pfn GetStreamlineUnwrapFunc() {
    static slGetNativeInterface_pfn s_func = nullptr;
    static bool s_checked = false;

    if (!s_checked) {
        s_checked = true;
        HMODULE sl_mod = GetModuleHandleW(L"sl.interposer.dll");
        if (sl_mod) {
            s_func = reinterpret_cast<slGetNativeInterface_pfn>(
                GetProcAddress(sl_mod, "slGetNativeInterface"));
            if (s_func)
                LOG_INFO("FLC: Streamline slGetNativeInterface resolved");
            else
                LOG_WARN("FLC: sl.interposer.dll loaded but slGetNativeInterface not found");
        }
    }
    return s_func;
}

/// Attempt to unwrap a Streamline proxy swapchain to get the native IDXGISwapChain2.
/// Returns the native IDXGISwapChain2* (caller must Release), or nullptr if not proxied.
static IDXGISwapChain2* UnwrapStreamlineProxy(IDXGISwapChain* sc) {
    auto sl_unwrap = GetStreamlineUnwrapFunc();
    if (!sl_unwrap) return nullptr;

    void* native = nullptr;
    int result = sl_unwrap(sc, &native);
    if (result != 0 || !native) {
        LOG_WARN("FLC: slGetNativeInterface failed (result=%d, native=%p)", result, native);
        return nullptr;
    }

    // If Streamline returns the same pointer, the swapchain isn't proxied —
    // it's already the native interface. This is normal, not an error.
    if (native == sc) {
        LOG_INFO("FLC: swapchain is not proxied (slGetNativeInterface returned same pointer)");
        return nullptr;
    }

    IDXGISwapChain2* sc2 = nullptr;
    IUnknown* unk = static_cast<IUnknown*>(native);
    if (SUCCEEDED(unk->QueryInterface(__uuidof(IDXGISwapChain2),
                                       reinterpret_cast<void**>(&sc2)))) {
        return sc2;  // caller must Release
    }

    LOG_WARN("FLC: native interface QI for IDXGISwapChain2 failed");
    return nullptr;
}

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
                // Capture game's requested latency before overriding
                UINT current_latency = 0;
                device1->GetMaximumFrameLatency(&current_latency);
                if (current_latency > 0) {
                    g_game_requested_latency.store(current_latency, std::memory_order_relaxed);
                    LOG_INFO("FLC: DX11 game requested latency = %u", current_latency);
                }

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

// ── DX12: IDXGISwapChain2::SetMaximumFrameLatency(1) on waitable swapchains ──
static void ApplyDX12FrameLatency(uint64_t native_handle) {
    auto* sc = reinterpret_cast<IDXGISwapChain*>(native_handle);

    __try {
        // Check if swapchain was created with DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT
        DXGI_SWAP_CHAIN_DESC desc = {};
        HRESULT hr = sc->GetDesc(&desc);
        if (FAILED(hr)) {
            LOG_WARN("FLC: DX12 GetDesc failed hr=0x%08X", hr);
            return;
        }

        if (!(desc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)) {
            // Non-waitable swapchain — skip DXGI calls, rely on Reflex bLowLatencyMode
            LOG_INFO("FLC: DX12 non-waitable swapchain, skipping DXGI frame latency (Reflex manages queue depth)");
            return;
        }

        // Waitable swapchain — need IDXGISwapChain2::SetMaximumFrameLatency(1)
        // If Streamline is loaded, unwrap the proxy first
        IDXGISwapChain2* sc2 = nullptr;
        bool from_streamline = false;

        if (GetStreamlineUnwrapFunc()) {
            sc2 = UnwrapStreamlineProxy(sc);
            if (sc2) {
                from_streamline = true;
                LOG_INFO("FLC: DX12 using Streamline-unwrapped native interface for frame latency");
            }
        }

        // If no Streamline proxy (or unwrap failed), QI directly
        if (!sc2) {
            hr = sc->QueryInterface(__uuidof(IDXGISwapChain2),
                                     reinterpret_cast<void**>(&sc2));
            if (FAILED(hr) || !sc2) {
                LOG_WARN("FLC: DX12 QI for IDXGISwapChain2 failed hr=0x%08X", hr);
                return;
            }
        }

        // Capture game's requested latency before overriding.
        // DX12 waitable swapchains default to 3 if the game hasn't explicitly set it.
        // We can't query the current value from IDXGISwapChain2, so use the DXGI default.
        uint32_t prev_latency = g_game_requested_latency.load(std::memory_order_relaxed);
        if (prev_latency == 0) {
            g_game_requested_latency.store(3, std::memory_order_relaxed);
            LOG_INFO("FLC: DX12 waitable swapchain — defaulting game latency to 3");
        }

        hr = sc2->SetMaximumFrameLatency(1);
        if (SUCCEEDED(hr))
            LOG_INFO("FLC: DX12 waitable frame latency set to 1 via IDXGISwapChain2%s",
                     from_streamline ? " (Streamline native)" : "");
        else
            LOG_WARN("FLC: DX12 IDXGISwapChain2::SetMaximumFrameLatency failed hr=0x%08X", hr);

        sc2->Release();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_WARN("FLC: exception in DX12 frame latency path (handle=0x%llX)", native_handle);
    }
}

// ── Public API ──

void FLC_OnSwapchainInit(uint64_t native_handle, ActiveAPI api) {
    if (s_applied) return;  // already applied for this swapchain lifetime

    if (!native_handle) {
        LOG_WARN("FLC: init called with null handle");
        return;
    }

    // ── DMFG passthrough: preserve game's requested queue depth ──
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
            ApplyDX12FrameLatency(native_handle);
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
