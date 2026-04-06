#include "correlator.h"
#include "fg_divisor.h"
#include "health.h"
#include "pcl_hooks.h"
#include "wake_guard.h"
#include "display_state.h"
#include "logger.h"
#include <Windows.h>
#include <dxgi.h>

PresentCorrelator g_correlator;
IDXGISwapChain* g_stats_swapchain = nullptr;

// g_presenting_swapchain: transitional — previously defined in dllmain.cpp.
// Consumed only by correlator for stats resolution fallback.
// New code should use SwapMgr_GetNativeHandle() instead.
IDXGISwapChain* g_presenting_swapchain = nullptr;

// ── Streamline unwrap ──
// Streamline wraps the real DXGI swapchain in a proxy. The proxy's
// GetFrameStatistics returns DXGI_ERROR_INVALID_CALL because it doesn't
// forward the call to the real swapchain. We try slGetNativeInterface
// to unwrap it, same pattern as the reference project's device_unwrap.
using slGetNativeInterface_pfn = int (*)(void* proxy, void** native);

static IDXGISwapChain* TryStreamlineUnwrap(IDXGISwapChain* sc) {
    if (!sc) return nullptr;

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
            real_sc->Release(); // QI AddRef'd; we borrow the pointer
            return reinterpret_cast<IDXGISwapChain*>(native);
        }
    }

    return nullptr;
}

// ── IDXGIOutput fallback ──
// In windowed flip-model (FLIP_DISCARD / FLIP_SEQUENTIAL), the swapchain's
// GetFrameStatistics may return DXGI_ERROR_INVALID_CALL, but the output's
// GetFrameStatistics works. This is the last-resort path.
static IDXGIOutput* TryGetOutput(IDXGISwapChain* sc) {
    if (!sc) return nullptr;
    IDXGIOutput* output = nullptr;
    if (SUCCEEDED(sc->GetContainingOutput(&output)) && output)
        return output; // caller must Release
    return nullptr;
}

// ── Resolution: find a source that actually responds to GetFrameStatistics ──
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
            g_correlator.use_output_stats = false;
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
            g_correlator.use_output_stats = false;
            return unwrapped;
        }
        LOG_INFO("Unwrapped swapchain %p GetFrameStatistics failed hr=0x%08X, trying output",
                 unwrapped, hr);
    }

    // 3. Try IDXGIOutput::GetFrameStatistics (windowed flip-model fallback)
    // Try the unwrapped swapchain's output first, then the presenting swapchain's.
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
                // Release any previously cached output
                if (g_correlator.cached_output)
                    g_correlator.cached_output->Release();
                g_correlator.cached_output = output; // takes ownership
                g_correlator.use_output_stats = true;
                g_stats_swapchain = candidate;
                return candidate;
            }
            output->Release();
        }
    }

    LOG_WARN("Stats resolution failed: no source responds to GetFrameStatistics");
    return nullptr;
}

// ── QueryFrameStatistics: single entry point for all stats queries ──
// Wrapped in C++ try/catch because DXGI can throw C++ exceptions (0xE06D7363)
// when the swapchain is in a transitional state (FG reconfiguration, device
// removal, mode switch). SEH (__try/__except) does not catch these.
HRESULT PresentCorrelator::QueryFrameStatistics(DXGI_FRAME_STATISTICS& stats) {
    // Acquire pairs with the release in OnSwapchainDestroyed — guarantees
    // we see the nulled pointers if we see permanently_disabled=true.
    if (permanently_disabled.load(std::memory_order_acquire)) return E_FAIL;

    try {
        // Snapshot pointers locally — they could be nulled by OnSwapchainDestroyed
        // on another thread between our check above and the dereference below.
        IDXGIOutput* output = cached_output;
        if (use_output_stats && output) {
            return output->GetFrameStatistics(&stats);
        }

        IDXGISwapChain* sc = g_stats_swapchain;
        if (!sc)
            sc = g_presenting_swapchain ? g_presenting_swapchain : g_swapchain;
        if (!sc) return E_FAIL;

        return sc->GetFrameStatistics(&stats);
    } catch (...) {
        LOG_WARN("Correlator: C++ exception in QueryFrameStatistics, disabling");
        permanently_disabled.store(true, std::memory_order_relaxed);
        return E_FAIL;
    }
}

// ── OnPresent / OnFGPresent ──

void PresentCorrelator::OnPresent(uint64_t frameID, int64_t deadline) {
    if (permanently_disabled) return;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    Submission sub = {};
    sub.frameID = frameID;
    sub.submit_qpc = now.QuadPart;
    sub.sequence = next_seq++;
    sub.scheduled_deadline = deadline;
    sub.is_fg = false;

    submissions.Push(sub);
}

void PresentCorrelator::OnFGPresent() {
    if (permanently_disabled) return;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    Submission sub = {};
    sub.frameID = 0;
    sub.submit_qpc = now.QuadPart;
    sub.sequence = next_seq++;
    sub.scheduled_deadline = 0;
    sub.is_fg = true;

    submissions.Push(sub);
}

// ── RetireOne ──

bool PresentCorrelator::RetireOne(Retired& out) {
    if (permanently_disabled) return false;

    DXGI_FRAME_STATISTICS stats = {};
    HRESULT hr = QueryFrameStatistics(stats);
    if (FAILED(hr)) return false;

    if (last_retired_seq >= next_seq)
        return false;

    uint64_t dxgi_count = first_present_count + last_retired_seq;
    if (dxgi_count > stats.PresentCount)
        return false;

    uint64_t gap = stats.PresentCount - dxgi_count;
    const auto& sub = submissions.Get(last_retired_seq);

    double est_refresh = g_estimated_refresh_us.load(std::memory_order_relaxed);

    // FG-aware scanout estimation. With FG active, the gap between our
    // submission and the current PresentCount includes FG-generated presents.
    // For real frames, the scanout estimate (SyncQPCTime - gap * refresh)
    // is accurate enough to drive the deadline bias feedback, because the
    // FG presents are evenly spaced at refresh intervals after the real
    // present. Mark real-frame retirements as "exact" when FG is active
    // so the feedback loop gets data to work with.
    bool fg_active = ComputeFGDivisorRaw() > 1;
    bool usable_scanout = (gap == 0) || (!sub.is_fg && fg_active);

    if (usable_scanout) {
        int64_t scanout = (gap == 0)
            ? stats.SyncQPCTime.QuadPart
            : stats.SyncQPCTime.QuadPart -
              static_cast<int64_t>(gap) * us_to_qpc(est_refresh);
        out.frameID = sub.frameID;
        out.scheduled_deadline = sub.scheduled_deadline;
        out.actual_scanout_qpc = scanout;
        out.submit_qpc = sub.submit_qpc;
        out.is_fg = sub.is_fg;
        out.scanout_exact = true;
    } else {
        int64_t estimated = stats.SyncQPCTime.QuadPart -
            static_cast<int64_t>(gap) * us_to_qpc(est_refresh);
        out.frameID = sub.frameID;
        out.scheduled_deadline = sub.scheduled_deadline;
        out.actual_scanout_qpc = estimated;
        out.submit_qpc = sub.submit_qpc;
        out.is_fg = sub.is_fg;
        out.scanout_exact = false;
    }

    last_retired_seq++;
    return true;
}

// ── Calibrate ──

void PresentCorrelator::Calibrate() {
    if (permanently_disabled) return;
    if (suspend_until_qpc != 0) return; // in cooldown — CheckOverflow handles expiry

    // Backoff: after repeated failures, don't retry every frame.
    // Try every ~60 frames (~1 second at 60fps) to avoid hammering
    // a swapchain that isn't ready yet.
    if (calibrate_fail_count >= 3) {
        if (++calibrate_backoff_counter < 60)
            return;
        calibrate_backoff_counter = 0;
    }

    // Try QueryFrameStatistics directly — it falls back through
    // g_stats_swapchain → g_presenting_swapchain → g_swapchain.
    // Don't eagerly resolve on first call; the rendering pipeline may
    // not be fully up yet and probing will fail prematurely.
    DXGI_FRAME_STATISTICS stats = {};
    HRESULT hr = QueryFrameStatistics(stats);
    if (FAILED(hr)) {
        calibrate_fail_count++;
        if (calibrate_fail_count <= 5)
            LOG_WARN("Correlator Calibrate: GetFrameStatistics failed hr=0x%08X "
                     "(attempt %d)", hr, calibrate_fail_count);

        // Periodically retry stats source resolution. The presenting swapchain
        // may not support GetFrameStatistics immediately — DXGI needs ~16-24
        // frames after the first present before stats become available, and
        // Streamline proxies never support it (need unwrap). Retry every 120
        // calibration attempts (~2s at 60fps) to catch the window when the
        // swapchain becomes ready, without hammering it every frame.
        if (calibrate_fail_count % 120 == 3) {
            LOG_WARN("Correlator: re-attempting stats source resolution (fail_count=%d)",
                     calibrate_fail_count);
            g_stats_swapchain = nullptr;
            use_output_stats = false;
            if (cached_output) {
                cached_output->Release();
                cached_output = nullptr;
            }
            TryResolveStatsSwapchain();
        }
        return;
    }

    // Require two consecutive successful reads with advancing PresentCount
    // before declaring calibration complete. A single success may be transient
    // (the DXGI runtime can return S_OK with stale data during pipeline warmup).
    if (calibrate_last_present_count != 0 &&
        stats.PresentCount > calibrate_last_present_count) {
        // PresentCount advanced between two successful reads — source is live
        calibrate_fail_count = 0;
        calibrate_backoff_counter = 0;
        first_present_count = stats.PresentCount - next_seq;

        // Present-based enforcement (DX11, non-Reflex DX12): OnPresent is
        // called from the present callback BEFORE the actual DXGI present
        // completes, so our submission count runs 1 ahead of PresentCount.
        // Subtract 1 to align retirement. Marker-based enforcement (DX12
        // Reflex) calls OnPresent at PRESENT_START after the present, so
        // no offset needed.
        if (!AreNvAPIMarkersFlowing() && !PCL_MarkersFlowing())
            first_present_count--;

        needs_recalibration = false;
        LOG_WARN("Correlator calibrated: first_present_count=%llu, PresentCount=%u, "
                 "next_seq=%llu, SyncQPC=%lld, source=%s",
                 first_present_count, stats.PresentCount, next_seq,
                 stats.SyncQPCTime.QuadPart,
                 use_output_stats ? "IDXGIOutput" : "IDXGISwapChain");
        if (suspend_generation > 0)
            LOG_INFO("Correlator active (recovered after %d suspend cycle%s)",
                     suspend_generation, suspend_generation > 1 ? "s" : "");
    } else {
        // First success or PresentCount stale — record and wait for next call
        if (calibrate_last_present_count == 0) {
            LOG_INFO("Correlator Calibrate: first successful read, PresentCount=%u "
                     "(waiting for advancement)", stats.PresentCount);
        }
        calibrate_last_present_count = stats.PresentCount;
        // Reset backoff so we retry quickly now that reads are succeeding
        calibrate_backoff_counter = 0;
        calibrate_fail_count = 0;
    }
}

// ── CheckOverflow ──

void PresentCorrelator::CheckOverflow() {
    if (permanently_disabled) return;

    // Suspend-with-backoff: if we're in a cooldown period, skip until it expires.
    if (suspend_until_qpc != 0) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (now.QuadPart < suspend_until_qpc)
            return;
        // Cooldown expired — resume and retry
        suspend_until_qpc = 0;
        LOG_INFO("Correlator: suspend expired (gen=%d), retrying calibration",
                 suspend_generation);
        Reset();
        return; // let next frame start fresh
    }

    uint64_t pending = next_seq - last_retired_seq;

    if (pending > 28) {
        overflow_count++;
        if (overflow_count <= 3)
            LOG_WARN("Correlator overflow: pending=%llu (>28), resetting", pending);
        Reset();

        // After 3 consecutive overflows, try re-resolving the stats source.
        if (overflow_count >= 3) {
            resolve_attempts++;
            if (resolve_attempts <= 2) {
                LOG_WARN("Correlator: %d consecutive overflows, re-resolving stats source "
                         "(attempt %d/2)", overflow_count, resolve_attempts);
                g_stats_swapchain = nullptr;
                use_output_stats = false;
                if (cached_output) {
                    cached_output->Release();
                    cached_output = nullptr;
                }
                TryResolveStatsSwapchain();
                overflow_count = 0;
            } else {
                // Suspend with exponential backoff instead of permanent disable.
                // 5s * 2^gen, capped at ~30s.
                int gen = (suspend_generation < 5) ? suspend_generation : 5;
                double suspend_sec = 5.0 * (1 << gen);
                LARGE_INTEGER now;
                QueryPerformanceCounter(&now);
                suspend_until_qpc = now.QuadPart + us_to_qpc(suspend_sec * 1e6);
                suspend_generation++;
                overflow_count = 0;
                resolve_attempts = 0;
                LOG_WARN("Correlator: overflow suspend for %.0fs (gen=%d)",
                         suspend_sec, suspend_generation);
            }
        }
        return;
    }

    if (pending > 0) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        int64_t oldest_age = now.QuadPart - submissions.Get(last_retired_seq).submit_qpc;
        double age_us = qpc_to_us(oldest_age);

        if (pending > 2 && age_us > 500000.0) {
            stale_count++;
            if (stale_count <= 3)
                LOG_WARN("Correlator stale: pending=%llu, oldest_age=%.0fus, resetting "
                         "(stale_count=%d)", pending, age_us, stale_count);

            // After 10 stale resets, suspend with exponential backoff instead
            // of permanently disabling. This lets the correlator recover if
            // the stats source becomes healthy later (FG pipeline settles,
            // compositor state changes, etc.).
            if (stale_count >= 10) {
                int gen = (suspend_generation < 5) ? suspend_generation : 5;
                double suspend_sec = 5.0 * (1 << gen);
                suspend_until_qpc = now.QuadPart + us_to_qpc(suspend_sec * 1e6);
                suspend_generation++;
                stale_count = 0;
                LOG_WARN("Correlator: stale suspend for %.0fs (gen=%d)",
                         suspend_sec, suspend_generation);
                Reset();
                return;
            }

            Reset();
        }
    } else {
        if (!needs_recalibration) {
            overflow_count = 0;
            stale_count = 0;
            // Successful retirement — reset suspend backoff so next issue
            // starts with a short cooldown again.
            suspend_generation = 0;
        }
    }
}

// ── Reset ──

void PresentCorrelator::Reset() {
    if (permanently_disabled) return;
    LOG_WARN("Correlator reset (recalibration needed)");
    submissions.Clear();
    next_seq = 0;
    last_retired_seq = 0;
    needs_recalibration = true;
    calibrate_last_present_count = 0;
    calibrate_fail_count = 0;
    calibrate_backoff_counter = 0;
    // Clear suspend so calibration retries immediately.
    // Failure counters (stale_count, overflow_count, resolve_attempts)
    // are NOT reset here — they accumulate across resets to detect
    // repeating stale/overflow patterns in CheckOverflow().
    // External callers that want a full wipe (Flush, OnPresentingSwapchainChanged)
    // clear those separately.
    suspend_until_qpc = 0;
    suspend_generation = 0;
}

void PresentCorrelator::OnPresentingSwapchainChanged() {
    // The presenting swapchain changed — the previous stats source is invalid.
    // Clear all cached state and re-enable so we re-attempt calibration
    // with the new (correct) swapchain.
    g_stats_swapchain = nullptr;
    use_output_stats = false;
    if (cached_output) {
        cached_output->Release();
        cached_output = nullptr;
    }
    permanently_disabled.store(false, std::memory_order_relaxed);
    overflow_count = 0;
    stale_count = 0;
    resolve_attempts = 0;
    calibrate_fail_count = 0;
    calibrate_backoff_counter = 0;
    calibrate_last_present_count = 0;
    suspend_until_qpc = 0;
    suspend_generation = 0;
    Reset();

    // Don't eagerly resolve the stats source here. TryResolveStatsSwapchain
    // makes COM calls (GetFrameStatistics, slGetNativeInterface, GetContainingOutput)
    // on the game's render thread during present. When called right after a
    // swapchain recreate (FG toggle), these interfere with the game's worker
    // threads. The correlator's Calibrate() will lazily resolve with backoff.

    LOG_WARN("Correlator: presenting swapchain changed to %p (stats deferred)",
             g_presenting_swapchain);
}

void PresentCorrelator::OnSwapchainDestroyed() {
    // Full teardown: disable FIRST so the render thread's QueryFrameStatistics
    // bails out before we null/release any pointers. The store-release pairs
    // with the load-acquire in QueryFrameStatistics to guarantee ordering.
    permanently_disabled.store(true, std::memory_order_release);

    // Now safe to null pointers and release COM objects — render thread
    // will see permanently_disabled=true and won't touch them.
    g_stats_swapchain = nullptr;
    g_presenting_swapchain = nullptr;
    use_output_stats = false;
    if (cached_output) {
        cached_output->Release();
        cached_output = nullptr;
    }
    LOG_INFO("Correlator: swapchain destroyed, all DXGI pointers cleared");
}
