#include "correlator.h"
#include "health.h"
#include "wake_guard.h"
#include "display_state.h"
#include "logger.h"
#include <Windows.h>
#include <dxgi.h>

DXGIStatsSource g_correlator;
IDXGISwapChain* g_stats_swapchain = nullptr;
IDXGISwapChain* g_presenting_swapchain = nullptr;
static bool s_stats_swapchain_is_unwrapped = false;

// ── Streamline unwrap ──
using slGetNativeInterface_pfn = int (*)(void* proxy, void** native);

static IDXGISwapChain* TryStreamlineUnwrap(IDXGISwapChain* sc) {
    // DISABLED: Streamline unwrap causes reference count inconsistency
    // that prevents proper swapchain teardown, leading to E_ACCESSDENIED
    // on swapchain recreation and Streamline crash in sl.common.dll.
    // The old correlator never unwrapped — stats work fine without it.
    return nullptr;

    static HMODULE sl_mod = nullptr;
    static slGetNativeInterface_pfn sl_func = nullptr;
    static bool sl_checked = false;

    if (!sl_checked) {
        sl_checked = true;
        sl_mod = GetModuleHandleW(L"sl.interposer.dll");
        if (sl_mod) {
            sl_func = reinterpret_cast<slGetNativeInterface_pfn>(
                GetProcAddress(sl_mod, "slGetNativeInterface"));
            if (sl_func)
                LOG_INFO("Streamline unwrap function resolved");
            else
                LOG_INFO("Streamline loaded but slGetNativeInterface not found");
        }
    }

    if (!sl_func) return nullptr;

    void* native = nullptr;
    if (sl_func(sc, &native) == 0 && native && native != sc) {
        IDXGISwapChain* real_sc = nullptr;
        IUnknown* unk = static_cast<IUnknown*>(native);
        if (SUCCEEDED(unk->QueryInterface(__uuidof(IDXGISwapChain),
                                           reinterpret_cast<void**>(&real_sc)))) {
            LOG_INFO("Streamline unwrap: proxy=%p -> real=%p", sc, real_sc);
            // Keep the AddRef from QueryInterface — caller's lifetime is
            // tied to the swapchain. Released in OnSwapchainDestroyed.
            return real_sc;
        }
    }

    return nullptr;
}

// ── IDXGIOutput fallback ──
static IDXGIOutput* TryGetOutput(IDXGISwapChain* sc) {
    if (!sc) return nullptr;
    IDXGIOutput* output = nullptr;
    if (SUCCEEDED(sc->GetContainingOutput(&output)) && output)
        return output;
    return nullptr;
}

// ── Resolution: find a source that responds to GetFrameStatistics ──
IDXGISwapChain* TryResolveStatsSwapchain() {
    IDXGISwapChain* sc = g_presenting_swapchain ? g_presenting_swapchain : g_swapchain;
    if (!sc) return nullptr;

    // 1. Try the presenting swapchain directly
    {
        DXGI_FRAME_STATISTICS test = {};
        HRESULT hr = sc->GetFrameStatistics(&test);
        if (SUCCEEDED(hr)) {
            LOG_INFO("Stats resolved: presenting swapchain %p works directly", sc);
            g_stats_swapchain = sc;
            s_stats_swapchain_is_unwrapped = false;
            g_correlator.use_output_stats = false;
            g_correlator.resolved = true;
            return sc;
        }
        LOG_INFO("Presenting swapchain %p GetFrameStatistics failed hr=0x%08X, trying unwrap",
                 sc, hr);
    }

    // 2. Try Streamline unwrap
    IDXGISwapChain* unwrapped = TryStreamlineUnwrap(sc);
    if (unwrapped) {
        DXGI_FRAME_STATISTICS test = {};
        HRESULT hr = unwrapped->GetFrameStatistics(&test);
        if (SUCCEEDED(hr)) {
            LOG_INFO("Stats resolved: unwrapped swapchain %p works", unwrapped);
            g_stats_swapchain = unwrapped;
            s_stats_swapchain_is_unwrapped = true;
            g_correlator.use_output_stats = false;
            g_correlator.resolved = true;
            return unwrapped;
        }
        LOG_INFO("Unwrapped swapchain %p GetFrameStatistics failed hr=0x%08X, trying output",
                 unwrapped, hr);
    }

    // 3. Try IDXGIOutput::GetFrameStatistics (windowed flip-model fallback)
    IDXGISwapChain* candidates[] = { unwrapped, sc };
    for (auto* candidate : candidates) {
        if (!candidate) continue;
        IDXGIOutput* output = TryGetOutput(candidate);
        if (output) {
            DXGI_FRAME_STATISTICS test = {};
            HRESULT hr = output->GetFrameStatistics(&test);
            if (SUCCEEDED(hr)) {
                LOG_INFO("Stats resolved: IDXGIOutput %p from swapchain %p works",
                         output, candidate);
                if (g_correlator.cached_output)
                    g_correlator.cached_output->Release();
                g_correlator.cached_output = output;
                g_correlator.use_output_stats = true;
                g_correlator.resolved = true;
                g_stats_swapchain = candidate;
                return candidate;
            }
            output->Release();
        }
    }

    LOG_WARN("Stats resolution failed: no source responds to GetFrameStatistics");
    // If we unwrapped but didn't store it, release the AddRef
    if (unwrapped && unwrapped != g_stats_swapchain) {
        unwrapped->Release();
    }
    return nullptr;
}

// ── DXGIStatsSource methods ──

HRESULT DXGIStatsSource::QueryFrameStatistics(DXGI_FRAME_STATISTICS& stats) {
    if (permanently_disabled.load(std::memory_order_acquire))
        return E_FAIL;

    // Lazy resolution with backoff
    if (!resolved) {
        resolve_fail_count++;
        if (resolve_fail_count >= 3) {
            if (++resolve_backoff_counter < 60)
                return E_FAIL;
            resolve_backoff_counter = 0;
        }
        TryResolve();
        if (!resolved) return E_FAIL;
    }

    __try {
        // Re-check after capturing pointers — OnSwapchainDestroyed may have
        // run between our first check and here, freeing the swapchain.
        IDXGIOutput* output = cached_output;
        if (use_output_stats && output) {
            if (permanently_disabled.load(std::memory_order_acquire))
                return E_FAIL;
            return output->GetFrameStatistics(&stats);
        }

        IDXGISwapChain* sc = g_stats_swapchain;
        if (!sc)
            sc = g_presenting_swapchain ? g_presenting_swapchain : g_swapchain;
        if (!sc) return E_FAIL;

        if (permanently_disabled.load(std::memory_order_acquire))
            return E_FAIL;

        HRESULT hr = sc->GetFrameStatistics(&stats);
        if (FAILED(hr)) {
            resolved = false;
            resolve_fail_count = 0;
        }
        return hr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_WARN("DXGIStatsSource: SEH exception in QueryFrameStatistics (0x%08X), disabling",
                 GetExceptionCode());
        permanently_disabled.store(true, std::memory_order_relaxed);
        return E_FAIL;
    }
}

bool DXGIStatsSource::IsStatsAvailable() {
    DXGI_FRAME_STATISTICS test = {};
    return SUCCEEDED(QueryFrameStatistics(test));
}

void DXGIStatsSource::TryResolve() {
    TryResolveStatsSwapchain();
}

void DXGIStatsSource::OnPresentingSwapchainChanged() {
    if (s_stats_swapchain_is_unwrapped && g_stats_swapchain) {
        g_stats_swapchain->Release();
        s_stats_swapchain_is_unwrapped = false;
    }
    g_stats_swapchain = nullptr;
    use_output_stats = false;
    if (cached_output) {
        cached_output->Release();
        cached_output = nullptr;
    }
    permanently_disabled.store(false, std::memory_order_relaxed);
    resolved = false;
    resolve_fail_count = 0;
    resolve_backoff_counter = 0;

    LOG_WARN("DXGIStatsSource: presenting swapchain changed to %p (stats deferred)",
             g_presenting_swapchain);
}

void DXGIStatsSource::OnSwapchainDestroyed() {
    permanently_disabled.store(true, std::memory_order_release);
    if (s_stats_swapchain_is_unwrapped && g_stats_swapchain) {
        g_stats_swapchain->Release();
        s_stats_swapchain_is_unwrapped = false;
    }
    g_stats_swapchain = nullptr;
    g_presenting_swapchain = nullptr;
    use_output_stats = false;
    if (cached_output) {
        cached_output->Release();
        cached_output = nullptr;
    }
    resolved = false;
    LOG_INFO("DXGIStatsSource: swapchain destroyed, all DXGI pointers cleared");
}
