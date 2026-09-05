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
static std::atomic<int64_t> s_last_enforcement_marker_qpc{0};

// ── Swapchain validity ──
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
    const int64_t last_marker_qpc =
        s_last_enforcement_marker_qpc.load(std::memory_order_relaxed);
    if (last_marker_qpc == 0) return false;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return qpc_to_us(now.QuadPart - last_marker_qpc) <
        MARKER_FLOW_TIMEOUT_US;
}

bool IsCorrelatorValid() {
    // Vulkan/DX11/OpenGL path doesn't use DXGI stats — report as valid
    ActiveAPI api = SwapMgr_GetActiveAPI();
    if (api == ActiveAPI::Vulkan || api == ActiveAPI::DX11 || api == ActiveAPI::OpenGL)
        return true;
    // DX12: check if DXGI stats source is responding
    return g_correlator.IsStatsAvailable();
}

bool IsNvAPIAvailable() {
#ifdef _WIN64
    HMODULE nvapi = GetModuleHandleW(L"nvapi64.dll");
#else
    HMODULE nvapi = GetModuleHandleW(L"nvapi.dll");
#endif
    return nvapi != nullptr;
}

bool IsSwapchainValid() {
    // Vulkan/DX11 path: use generic swapchain validity flag
    ActiveAPI api = SwapMgr_GetActiveAPI();
    if (api == ActiveAPI::Vulkan)
        return s_vk_swapchain_valid.load(std::memory_order_relaxed);

    // OpenGL and DX9: swapchain is valid as long as SwapMgr reports valid
    // (no DXGI swapchain pointer to check — these APIs don't use DXGI)
    if (api == ActiveAPI::OpenGL || api == ActiveAPI::DX9)
        return SwapMgr_IsValid();

    // DX12 and DX11 use the SwapMgr lifetime flag; raw COM pointers are owned
    // behind the correlator's synchronized snapshot API.
    return SwapMgr_IsValid() &&
           !s_swapchain_invalid.load(std::memory_order_relaxed) &&
           !s_device_lost.load(std::memory_order_relaxed) &&
           !s_occluded.load(std::memory_order_relaxed);
}

void SetVkSwapchainValid(bool v) {
    s_vk_swapchain_valid.store(v, std::memory_order_relaxed);
}

// ── NvAPI marker flow (game-originated markers only) ──
static std::atomic<int64_t> s_last_nvapi_marker_qpc{0};

void RecordEnforcementMarker() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    s_last_enforcement_marker_qpc.store(
        now.QuadPart,
        std::memory_order_relaxed);
}

void RecordNvAPIMarker() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    s_last_nvapi_marker_qpc.store(now.QuadPart, std::memory_order_relaxed);
}

bool AreNvAPIMarkersFlowing() {
    const int64_t last_marker_qpc =
        s_last_nvapi_marker_qpc.load(std::memory_order_relaxed);
    if (last_marker_qpc == 0) return false;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return qpc_to_us(now.QuadPart - last_marker_qpc) <
        MARKER_FLOW_TIMEOUT_US;
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
