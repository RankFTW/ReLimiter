// The enforcement dispatcher's route selection (src/enforcement_dispatcher.cpp) with the marker state scripted by
// stubs: marker routes (NvAPI latency markers, Streamline PCL markers) are a 64-bit route. In a 32-bit process the
// dispatcher selects the present route whatever the marker state says, and its present handler runs the present
// enforcement; in a 64-bit process the same marker state selects the marker routes (sticky for NvAPI, with the
// present fallback while the markers stop). The hooks that produce the markers and the scheduler behind the present
// handler need live validation.

#include "enforcement_dispatcher.h"
#include "flush.h"
#include "health.h"
#include "logger.h"
#include "pcl_hooks.h"
#include "streamline_hooks.h"
#include "vk_enforce.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>

namespace
{
    int s_failures = 0;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,        \
                         #condition);                                           \
            ++s_failures;                                                       \
        }                                                                       \
    } while (false)

    // The scripted marker state and the calls the dispatcher makes.
    bool s_nvapi_flowing = false;
    bool s_marker_triggers_flowing = false;
    bool s_pcl_flowing = false;
    int s_present_calls = 0;
    int s_forced_present_calls = 0;
    int s_flushes = 0;

    constexpr bool kIs64 = sizeof(void*) == 8;

    void ResetState()
    {
        EnfDisp_OnSwapchainDestroy(true);
        s_nvapi_flowing = s_marker_triggers_flowing = s_pcl_flowing = false;
        s_present_calls = s_forced_present_calls = s_flushes = 0;
    }

    EnforcementPath Select(ActiveAPI api)
    {
        EnfDisp_OnSwapchainInit(api);
        return EnfDisp_GetActivePath();
    }

    void TestNoMarkersIsThePresentRoute()
    {
        ResetState();
        CHECK(Select(ActiveAPI::DX12) == EnforcementPath::PresentBased);
        CHECK(Select(ActiveAPI::DX11) == EnforcementPath::PresentBased);
        CHECK(Select(ActiveAPI::Vulkan) == EnforcementPath::PresentBased);
        CHECK(Select(ActiveAPI::DX9) == EnforcementPath::PresentBased);
        CHECK(Select(ActiveAPI::OpenGL) == EnforcementPath::PresentBased);
        CHECK(Select(ActiveAPI::None) == EnforcementPath::None);
    }

    // The same marker state: the marker routes on 64-bit, the present route on 32-bit.
    void TestMarkersSelectTheMarkerRouteOnlyOn64Bit()
    {
        ResetState();
        s_nvapi_flowing = s_marker_triggers_flowing = true;
        const EnforcementPath nvapi_route = kIs64 ? EnforcementPath::NvAPIMarkers : EnforcementPath::PresentBased;
        CHECK(Select(ActiveAPI::DX12) == nvapi_route);
        ResetState();
        s_nvapi_flowing = s_marker_triggers_flowing = true;
        CHECK(Select(ActiveAPI::DX11) == nvapi_route);
        // DirectX markers do not make a Vulkan swapchain a marker route, nor the reverse.
        ResetState();
        s_nvapi_flowing = true;
        CHECK(Select(ActiveAPI::Vulkan) == EnforcementPath::PresentBased);
        ResetState();
        s_pcl_flowing = true;
        CHECK(Select(ActiveAPI::Vulkan) == (kIs64 ? EnforcementPath::PCLMarkers : EnforcementPath::PresentBased));
        CHECK(Select(ActiveAPI::DX12) == EnforcementPath::PresentBased);
        CHECK(Select(ActiveAPI::DX9) == EnforcementPath::PresentBased);
        CHECK(Select(ActiveAPI::OpenGL) == EnforcementPath::PresentBased);
    }

    // The NvAPI marker route is sticky on 64-bit once markers have flowed; a 32-bit process never takes it, and its
    // present handler enforces on every present however the marker state moves.
    void TestPresentHandlerFollowsTheRoute()
    {
        ResetState();
        s_nvapi_flowing = s_marker_triggers_flowing = true;
        Select(ActiveAPI::DX12);
        EnfDisp_OnPresent(1000);
        // 64-bit: the marker route enforces at the markers, nothing at the present. 32-bit: the present enforces.
        CHECK(s_present_calls == (kIs64 ? 0 : 1));
        s_nvapi_flowing = false; // the markers stop
        EnfDisp_OnPresent(2000);
        CHECK(EnfDisp_GetActivePath() == (kIs64 ? EnforcementPath::NvAPIMarkers : EnforcementPath::PresentBased));
        // 64-bit: the marker route falls back to the present enforcement, forced; 32-bit: the present route's own.
        CHECK(s_present_calls == (kIs64 ? 1 : 2));
        CHECK(s_forced_present_calls == (kIs64 ? 1 : 0));
    }

    void TestVulkanMarkerRouteFallsBackWhileTheMarkersStop()
    {
        ResetState();
        s_pcl_flowing = true;
        Select(ActiveAPI::Vulkan);
        EnfDisp_OnPresent(1000);
        CHECK(s_present_calls == (kIs64 ? 0 : 1));
        s_pcl_flowing = false;
        EnfDisp_OnPresent(2000);
        CHECK(EnfDisp_GetActivePath() == EnforcementPath::PresentBased);
        CHECK(s_present_calls == (kIs64 ? 1 : 2));
        CHECK(s_forced_present_calls == 0); // the route re-evaluated to the present route: its own enforcement
    }
}

// The dispatcher's external state, scripted.
bool AreNvAPIMarkersFlowing() { return s_nvapi_flowing; }
bool AreMarkerTriggersFlowing() { return s_marker_triggers_flowing; }
#ifdef _WIN64
bool PCL_MarkersFlowing() { return s_pcl_flowing; }
bool IsDmfgActive() { return false; }
#endif
void VkEnforce_OnPresent(int64_t, bool force_marker_fallback)
{
    ++s_present_calls;
    if (force_marker_fallback) ++s_forced_present_calls;
}
void Flush(uint32_t) { ++s_flushes; }
void Log_Write(LogLevel, const char*, ...) {}

int main()
{
    TestNoMarkersIsThePresentRoute();
    TestMarkersSelectTheMarkerRouteOnlyOn64Bit();
    TestPresentHandlerFollowsTheRoute();
    TestVulkanMarkerRouteFallsBackWhileTheMarkersStop();
    if (s_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", s_failures);
        return 1;
    }
    std::puts("enforcement route: all tests passed");
    return 0;
}
