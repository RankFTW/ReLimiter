#include "health.h"
#include "correlator.h"
#include "swapchain_manager.h"
#include "wake_guard.h"
#include <Windows.h>
#include <atomic>

// ── DXGI stats freshness ──
static int s_frames_since_dxgi_update = 0;
static bool s_dxgi_updated_this_frame = false;

// ── Marker flow ──
static int64_t s_last_enforcement_marker_qpc = 0;

// ── Swapchain validity ──
extern IDXGISwapChain* g_swapchain;
static std::atomic<bool> s_swapchain_invalid{false};
static std::atomic<bool> s_device_lost{false};
static std::atomic<bool> s_occluded{false};

// ── Vulkan swapchain validity ──
static std::atomic<bool> s_vk_swapchain_valid{false};

bool IsDXGIStatsFresh() {
    // Vulkan/DX11 don't use DXGI stats — report as fresh to avoid Tier2a
    ActiveAPI api = SwapMgr_GetActiveAPI();
    if (api == ActiveAPI::Vulkan || api == ActiveAPI::DX11 || api == ActiveAPI::OpenGL)
        return true;
    return s_frames_since_dxgi_update < 10;
}

bool AreMarkersFlowing() {
    if (s_last_enforcement_marker_qpc == 0) return false;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return qpc_to_us(now.QuadPart - s_last_enforcement_marker_qpc) < 500000.0;
}

bool IsCorrelatorValid() {
    // Vulkan/DX11/OpenGL path doesn't use the DXGI correlator — report as valid
    ActiveAPI api = SwapMgr_GetActiveAPI();
    if (api == ActiveAPI::Vulkan || api == ActiveAPI::DX11 || api == ActiveAPI::OpenGL)
        return true;
    return !g_correlator.needs_recalibration &&
           (g_correlator.next_seq - g_correlator.last_retired_seq) < 32;
}

bool IsNvAPIAvailable() {
    HMODULE nvapi = GetModuleHandleW(L"nvapi64.dll");
    return nvapi != nullptr;
}

bool IsSwapchainValid() {
    // Vulkan/DX11 path: use generic swapchain validity flag
    ActiveAPI api = SwapMgr_GetActiveAPI();
    if (api == ActiveAPI::Vulkan)
        return s_vk_swapchain_valid.load(std::memory_order_relaxed);

    // OpenGL: swapchain is valid as long as SwapMgr reports valid
    if (api == ActiveAPI::OpenGL)
        return SwapMgr_IsValid();

    // DX12 and DX11 both use g_swapchain (set by OnInitSwapchain)
    return g_swapchain != nullptr &&
           !s_swapchain_invalid.load(std::memory_order_relaxed) &&
           !s_device_lost.load(std::memory_order_relaxed) &&
           !s_occluded.load(std::memory_order_relaxed);
}

void SetVkSwapchainValid(bool v) {
    s_vk_swapchain_valid.store(v, std::memory_order_relaxed);
}

// ── NvAPI marker flow (game-originated markers only) ──
static int64_t s_last_nvapi_marker_qpc = 0;

void RecordEnforcementMarker() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    s_last_enforcement_marker_qpc = now.QuadPart;
}

void RecordNvAPIMarker() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    s_last_nvapi_marker_qpc = now.QuadPart;
}

bool AreNvAPIMarkersFlowing() {
    if (s_last_nvapi_marker_qpc == 0) return false;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return qpc_to_us(now.QuadPart - s_last_nvapi_marker_qpc) < 500000.0;
}

void TickHealthFrame() {
    if (s_dxgi_updated_this_frame) {
        s_frames_since_dxgi_update = 0;
        s_dxgi_updated_this_frame = false;
    } else {
        s_frames_since_dxgi_update++;
    }
}

void RecordDXGIStatsUpdate() {
    s_dxgi_updated_this_frame = true;
}

// ── Setters for external events ──
void SetSwapchainInvalid(bool v) {
    s_swapchain_invalid.store(v, std::memory_order_relaxed);
}

void SetDeviceLost(bool v) {
    s_device_lost.store(v, std::memory_order_relaxed);
}

void SetOccluded(bool v) {
    s_occluded.store(v, std::memory_order_relaxed);
}
