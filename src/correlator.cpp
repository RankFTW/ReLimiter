#include "correlator.h"
#include "swapchain_manager.h"
#include "logger.h"

#include <Windows.h>
#include <dxgi.h>

#include <array>
#include <atomic>

DXGIStatsSource g_correlator;

namespace
{
    std::atomic<uint64_t> s_legacy_swapchain{0};
    std::atomic<uint64_t> s_presenting_swapchain{0};
    std::atomic<uint64_t> s_publication_sequence{0};
    std::atomic<uint32_t> s_publication_readers{0};
    SRWLOCK s_publication_writer_lock = SRWLOCK_INIT;

    constexpr size_t kRetiredSwapchainCapacity = 256;
    std::array<IUnknown*, kRetiredSwapchainCapacity> s_retired_swapchains{};
    SRWLOCK s_retired_lock = SRWLOCK_INIT;

    bool SafeAddRef(IUnknown* object) noexcept
    {
        if (!object) return false;
        __try {
            object->AddRef();
            return true;
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    void SafeRelease(IUnknown* object) noexcept
    {
        if (!object) return;
        __try { object->Release(); }
        __except(EXCEPTION_EXECUTE_HANDLER) { }
    }

    void TryDrainRetiredSwapchains() noexcept
    {
        if (s_publication_readers.load(std::memory_order_acquire) != 0) return;

        std::array<IUnknown*, kRetiredSwapchainCapacity> retired{};
        AcquireSRWLockExclusive(&s_retired_lock);
        if (s_publication_readers.load(std::memory_order_acquire) == 0) {
            retired = s_retired_swapchains;
            s_retired_swapchains.fill(nullptr);
        }
        ReleaseSRWLockExclusive(&s_retired_lock);

        // COM Release is intentionally outside every ReLimiter lock. A proxy
        // may reenter swapchain lifecycle code from its final release.
        for (IUnknown* object : retired) SafeRelease(object);
    }

    void RetireSwapchain(IUnknown* object) noexcept
    {
        if (!object) return;
        bool retained = false;
        AcquireSRWLockExclusive(&s_retired_lock);
        for (IUnknown*& slot : s_retired_swapchains) {
            if (!slot) {
                slot = object;
                retained = true;
                break;
            }
        }
        ReleaseSRWLockExclusive(&s_retired_lock);
        if (!retained) {
            // Do not release beneath an announced reader. The fixed queue is
            // intentionally generous; overflow leaks one COM ref until process
            // exit rather than risking a use-after-release.
            OutputDebugStringA("ReLimiter: correlator retired-ref queue overflow.\n");
            return;
        }
        TryDrainRetiredSwapchains();
    }

    bool PublishSwapchain(
        std::atomic<uint64_t>* destination,
        IDXGISwapChain* swapchain) noexcept
    {
        IDXGISwapChain* published = swapchain;
        if (published && !SafeAddRef(published)) published = nullptr;

        AcquireSRWLockExclusive(&s_publication_writer_lock);
        uint64_t sequence =
            s_publication_sequence.load(std::memory_order_relaxed);
        if ((sequence & 1u) != 0) ++sequence;
        s_publication_sequence.store(sequence + 1, std::memory_order_release);
        IDXGISwapChain* previous = reinterpret_cast<IDXGISwapChain*>(
            destination->load(std::memory_order_relaxed));
        if (previous == published) {
            s_publication_sequence.store(sequence + 2, std::memory_order_release);
            ReleaseSRWLockExclusive(&s_publication_writer_lock);
            SafeRelease(published);
            return false;
        }
        destination->store(
            reinterpret_cast<uint64_t>(published),
            std::memory_order_relaxed);
        s_publication_sequence.store(sequence + 2, std::memory_order_release);
        ReleaseSRWLockExclusive(&s_publication_writer_lock);

        RetireSwapchain(previous);
        return true;
    }

    HRESULT SafeQueryFrameStatistics(
        IDXGISwapChain* swapchain,
        DXGI_FRAME_STATISTICS* stats) noexcept
    {
        if (!swapchain || !stats) return E_INVALIDARG;
        __try { return swapchain->GetFrameStatistics(stats); }
        __except(EXCEPTION_EXECUTE_HANDLER) { return E_FAIL; }
    }
}

void Correlator_SetLegacySwapchain(IDXGISwapChain* swapchain) noexcept
{
    PublishSwapchain(&s_legacy_swapchain, swapchain);
    if (swapchain)
        g_correlator.permanently_disabled.store(false, std::memory_order_release);
}

bool Correlator_SetPresentingSwapchain(IDXGISwapChain* swapchain) noexcept
{
    const bool changed = PublishSwapchain(&s_presenting_swapchain, swapchain);
    if (swapchain)
        g_correlator.permanently_disabled.store(false, std::memory_order_release);
    if (changed) {
        LOG_INFO(
            "DXGIStatsSource: presenting swapchain changed to %p (stats local)",
            swapchain);
    }
    return changed;
}

IDXGISwapChain* Correlator_AcquireSwapchain() noexcept
{
    for (int attempt = 0; attempt < 3; ++attempt) {
        const uint64_t before =
            s_publication_sequence.load(std::memory_order_acquire);
        if ((before & 1u) != 0) continue;
        s_publication_readers.fetch_add(1, std::memory_order_acq_rel);
        if (before != s_publication_sequence.load(std::memory_order_acquire)) {
            s_publication_readers.fetch_sub(1, std::memory_order_release);
            TryDrainRetiredSwapchains();
            continue;
        }

        IDXGISwapChain* swapchain = reinterpret_cast<IDXGISwapChain*>(
            s_presenting_swapchain.load(std::memory_order_relaxed));
        if (!swapchain) {
            swapchain = reinterpret_cast<IDXGISwapChain*>(
                s_legacy_swapchain.load(std::memory_order_relaxed));
        }
        if (swapchain && !SafeAddRef(swapchain)) swapchain = nullptr;
        const uint64_t after =
            s_publication_sequence.load(std::memory_order_acquire);
        s_publication_readers.fetch_sub(1, std::memory_order_release);
        TryDrainRetiredSwapchains();

        if (swapchain && before == after && (after & 1u) == 0)
            return swapchain;
        SafeRelease(swapchain);
    }
    return nullptr;
}

HRESULT Correlator_QuerySwapchainFrameStatistics(
    DXGI_FRAME_STATISTICS* stats, uint64_t* native_swapchain) noexcept
{
    if (!stats || !native_swapchain) return E_INVALIDARG;
    *native_swapchain = 0;
    const ActiveAPI api = SwapMgr_GetActiveAPI();
    if (g_correlator.permanently_disabled.load(std::memory_order_acquire) ||
        !SwapMgr_IsValid() || (api != ActiveAPI::DX11 && api != ActiveAPI::DX12)) {
        return E_FAIL;
    }

    IDXGISwapChain* swapchain = Correlator_AcquireSwapchain();
    if (!swapchain) return E_FAIL;
    *native_swapchain = reinterpret_cast<uint64_t>(swapchain);
    const HRESULT result = SafeQueryFrameStatistics(swapchain, stats);
    SafeRelease(swapchain);
    return result;
}

void DXGIStatsSource::OnSwapchainDestroyed()
{
    permanently_disabled.store(true, std::memory_order_release);
    PublishSwapchain(&s_presenting_swapchain, nullptr);
    PublishSwapchain(&s_legacy_swapchain, nullptr);
    LOG_INFO("DXGIStatsSource: swapchain destroyed, owned refs retired");
}
