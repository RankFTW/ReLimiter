#include "health.h"
#include "swapchain_manager.h"
#include "wake_guard.h"
#include <Windows.h>
#include <atomic>

// ── Marker flow ──
static std::atomic<int64_t> s_last_enforcement_marker_qpc{0};
static std::atomic<int64_t> s_last_marker_trigger_qpc{0};

// ── Swapchain validity ──
static std::atomic<bool> s_swapchain_invalid{false};
static std::atomic<bool> s_device_lost{false};
static std::atomic<bool> s_occluded{false};

// ── Vulkan swapchain validity ──
static std::atomic<bool> s_vk_swapchain_valid{false};

static bool IsRecent(const std::atomic<int64_t>& timestamp_qpc) {
    const int64_t last_qpc = timestamp_qpc.load(std::memory_order_relaxed);
    if (last_qpc == 0) return false;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return qpc_to_us(now.QuadPart - last_qpc) < MARKER_FLOW_TIMEOUT_US;
}

static void RecordNow(std::atomic<int64_t>* timestamp_qpc) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    timestamp_qpc->store(now.QuadPart, std::memory_order_relaxed);
}

bool AreMarkersFlowing() {
    return IsRecent(s_last_enforcement_marker_qpc);
}

bool AreMarkerTriggersFlowing() {
    return IsRecent(s_last_marker_trigger_qpc);
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
    RecordNow(&s_last_enforcement_marker_qpc);
}

void RecordMarkerTrigger() {
    RecordNow(&s_last_marker_trigger_qpc);
}

void RecordNvAPIMarker() {
    RecordNow(&s_last_nvapi_marker_qpc);
}

bool AreNvAPIMarkersFlowing() {
    return IsRecent(s_last_nvapi_marker_qpc);
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
