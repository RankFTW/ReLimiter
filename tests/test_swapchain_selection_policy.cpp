// Directly exercises the production header, not a copied manager model.
// Intended target: test_swapchain_selection_policy (C++17, src include path).
// Intended CTest name: swapchain_selection_policy. CMake registration is left
// to the integration owner. This does not validate Win32 window lookup, SRW
// admission, COM/device ownership, subsystem callbacks, or actual game pacing.
#include "swapchain_selection_policy.h"

#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <limits>

namespace {

constexpr SwapchainSelectionIdentity A{0x100, 1};
constexpr SwapchainSelectionIdentity B{0x200, 1}; // Same API (and may share a device).
constexpr SwapchainSelectionIdentity C{0x300, 2}; // Different API.
constexpr uintptr_t GAME_WINDOW = 10;
constexpr uintptr_t LAUNCHER_WINDOW = 20;
constexpr int64_t FREQUENCY = 1000;

void Check(bool condition, const char* expression) {
    if (condition) return;
    std::cerr << "FAILED: " << expression << '\n';
    std::exit(1);
}

#define CHECK(expression) Check((expression), #expression)

void Initialize(SwapchainSelectionPolicy* policy,
                const SwapchainSelectionIdentity& identity,
                uintptr_t window) {
    if (policy->CanInitialize(identity)) policy->Initialize(identity, window);
}

SwapchainPresentObservation Present(const SwapchainSelectionIdentity& identity,
                                    int64_t now_qpc,
                                    uintptr_t window = GAME_WINDOW,
                                    uintptr_t foreground = GAME_WINDOW) {
    return {identity, window, foreground, now_qpc, FREQUENCY};
}

void Accept(SwapchainSelectionPolicy* policy,
            const SwapchainPresentObservation& observation,
            SwapchainPresentSelection expected) {
    CHECK(policy->Evaluate(observation) == expected);
    policy->RecordPresent(observation);
    CHECK(policy->Owns(observation.identity));
}

void TestActivePrimarySurvivesIdleAuxiliaryInit() {
    SwapchainSelectionPolicy policy;
    Initialize(&policy, A, GAME_WINDOW);
    Accept(&policy, Present(A, 100), SwapchainPresentSelection::Keep);
    CHECK(!policy.CanInitialize(B));
    Initialize(&policy, B, GAME_WINDOW);
    CHECK(policy.Owns(A));
    Accept(&policy, Present(A, 116), SwapchainPresentSelection::Keep);
    CHECK(!policy.CanInitialize(C)); // Nor may an API change during init steal it.
}

void TestFirstActualPresenterOverridesInitOrder() {
    SwapchainSelectionPolicy policy;
    Initialize(&policy, A, LAUNCHER_WINDOW);
    Initialize(&policy, B, GAME_WINDOW);
    CHECK(policy.Owns(A)); // B's init alone has no selection authority.
    Accept(&policy, Present(B, 100, GAME_WINDOW, 0), SwapchainPresentSelection::Activate);
    CHECK(policy.Evaluate(Present(A, 116, LAUNCHER_WINDOW)) == SwapchainPresentSelection::Reject);
    CHECK(!policy.Destroy(A));
    Accept(&policy, Present(B, 132), SwapchainPresentSelection::Keep);
}

void TestSameDeviceAuxiliaryDestroyAndResizeAreIgnored() {
    SwapchainSelectionPolicy policy;
    Initialize(&policy, A, GAME_WINDOW);
    Accept(&policy, Present(A, 100), SwapchainPresentSelection::Keep);
    // Device equality is deliberately not an input to the production policy.
    // Both full destroy and resize use this exact same identity guard.
    for (int event = 0; event != 2; ++event) {
        CHECK(!policy.Destroy(B));
        CHECK(!policy.Destroy({A.native, C.api})); // Same numeric handle, wrong API.
        CHECK(!policy.Destroy({}));
        CHECK(policy.Owns(A));
    }
    Accept(&policy, Present(A, 116), SwapchainPresentSelection::Keep);
}

void TestRegularAuxiliaryPresentsNeverFlap() {
    SwapchainSelectionPolicy policy;
    Initialize(&policy, A, GAME_WINDOW);
    Accept(&policy, Present(A, 100), SwapchainPresentSelection::Keep);
    for (int64_t now = 116; now < 10000; now += 16) {
        Initialize(&policy, B, GAME_WINDOW);
        CHECK(policy.Evaluate(Present(B, now)) == SwapchainPresentSelection::Reject);
        CHECK(policy.Evaluate(Present(C, now, LAUNCHER_WINDOW)) == SwapchainPresentSelection::Reject);
        Accept(&policy, Present(A, now + 1), SwapchainPresentSelection::Keep);
    }
}

void TestActiveDestroyResizeRecreateAndRepeatedLateLoad() {
    SwapchainSelectionPolicy policy;
    // No init callback: this is not limited to one attempt per process.
    for (int lifetime = 0; lifetime != 4; ++lifetime) {
        Accept(&policy, Present(A, 100 + lifetime * 100), SwapchainPresentSelection::Activate);
        CHECK(policy.Destroy(A));
        CHECK(!policy.Destroy(A));
        CHECK(policy.CanInitialize(B));
        Accept(&policy, Present(B, 116 + lifetime * 100), SwapchainPresentSelection::Activate);
        CHECK(policy.Destroy(B));
    }
    Initialize(&policy, A, GAME_WINDOW);
    Accept(&policy, Present(A, 1000), SwapchainPresentSelection::Keep);
    CHECK(policy.Destroy(A)); // Resize invalidates the selection too.
    Initialize(&policy, A, GAME_WINDOW); // Native pointer reuse on reinit.
    Accept(&policy, Present(A, 1016), SwapchainPresentSelection::Keep);
    CHECK(policy.Destroy(A));
    Initialize(&policy, C, GAME_WINDOW);
    Accept(&policy, Present(C, 1032), SwapchainPresentSelection::Keep);
}

void TestForegroundLauncherTransitionsIncludingSameApi() {
    for (const auto game : {B, C}) {
        SwapchainSelectionPolicy policy;
        Initialize(&policy, A, LAUNCHER_WINDOW);
        Accept(&policy, Present(A, 100, LAUNCHER_WINDOW, LAUNCHER_WINDOW),
               SwapchainPresentSelection::Keep);
        Initialize(&policy, game, GAME_WINDOW);
        // The launcher can still be rendering: a distinct foreground window
        // is sufficient and does not depend on a DX12-over-DX11 ranking.
        Accept(&policy, Present(game, 116), SwapchainPresentSelection::Activate);
        for (int64_t now = 132; now < 5000; now += 16) {
            CHECK(policy.Evaluate(Present(A, now, LAUNCHER_WINDOW)) == SwapchainPresentSelection::Reject);
            Accept(&policy, Present(game, now + 1), SwapchainPresentSelection::Keep);
        }
    }

    SwapchainSelectionPolicy reverse;
    Initialize(&reverse, C, LAUNCHER_WINDOW);
    Accept(&reverse, Present(C, 100, LAUNCHER_WINDOW, LAUNCHER_WINDOW),
           SwapchainPresentSelection::Keep);
    Accept(&reverse, Present(B, 116), SwapchainPresentSelection::Activate);
    CHECK(reverse.Evaluate(Present(C, 132, LAUNCHER_WINDOW)) == SwapchainPresentSelection::Reject);
}

void TestPrimaryFastPathPreservesWindowAndClock() {
    SwapchainSelectionPolicy policy;
    Initialize(&policy, A, GAME_WINDOW);
    // The manager omits window/focus lookups for a matching primary.
    Accept(&policy, Present(A, 100, 0, 0), SwapchainPresentSelection::Keep);
    Accept(&policy, Present(A, 90, 0, 0), SwapchainPresentSelection::Keep);
    CHECK(policy.Evaluate(Present(B, 2099)) == SwapchainPresentSelection::Reject);
    CHECK(policy.Evaluate(Present(B, 2100)) == SwapchainPresentSelection::Activate);
    // Its initialized HWND must still establish a distinct-window handoff.
    CHECK(policy.Evaluate(Present(B, 116, LAUNCHER_WINDOW, LAUNCHER_WINDOW)) ==
          SwapchainPresentSelection::Activate);
}

void TestSameWindowStaleLauncherTransition() {
    SwapchainSelectionPolicy policy;
    Initialize(&policy, A, GAME_WINDOW);
    Accept(&policy, Present(A, 100), SwapchainPresentSelection::Keep);
    // Rejected auxiliary Presents must not extend the primary's timeout.
    for (int64_t now = 101; now < 2100; ++now) {
        CHECK(policy.Evaluate(Present(B, now)) == SwapchainPresentSelection::Reject);
    }
    Accept(&policy, Present(B, 2100), SwapchainPresentSelection::Activate);
    CHECK(policy.Evaluate(Present(A, 2101)) == SwapchainPresentSelection::Reject);
    Accept(&policy, Present(B, 2116), SwapchainPresentSelection::Keep);
    CHECK(policy.Evaluate(Present(A, 4100)) == SwapchainPresentSelection::Reject);
}

void TestConservativeWindowAndClockHandling() {
    SwapchainSelectionPolicy policy;
    CHECK(policy.Evaluate(Present({}, 100)) == SwapchainPresentSelection::Reject);
    CHECK(!policy.CanInitialize({0, 1}));
    CHECK(!policy.CanInitialize({A.native, 0}));
    Initialize(&policy, A, GAME_WINDOW);
    Accept(&policy, Present(A, 100), SwapchainPresentSelection::Keep);
    CHECK(policy.Evaluate(Present(B, 5000, 0, 0)) == SwapchainPresentSelection::Reject);
    CHECK(policy.Evaluate(Present(B, 5000, GAME_WINDOW, LAUNCHER_WINDOW)) == SwapchainPresentSelection::Reject);
    CHECK(policy.Evaluate(Present(B, 99)) == SwapchainPresentSelection::Reject);
    CHECK(policy.Evaluate(Present(B, -1)) == SwapchainPresentSelection::Reject);
    auto invalid_frequency = Present(B, 5000);
    invalid_frequency.qpc_frequency = 0;
    CHECK(policy.Evaluate(invalid_frequency) == SwapchainPresentSelection::Reject);
    invalid_frequency.qpc_frequency = -1;
    CHECK(policy.Evaluate(invalid_frequency) == SwapchainPresentSelection::Reject);
    invalid_frequency.qpc_frequency = (std::numeric_limits<int64_t>::max)();
    CHECK(policy.Evaluate(invalid_frequency) == SwapchainPresentSelection::Reject);

    policy.Reset();
    Initialize(&policy, A, 0); // Unknown old HWND cannot prove a focus transfer.
    Accept(&policy, Present(A, 100, 0, 0), SwapchainPresentSelection::Keep);
    CHECK(policy.Evaluate(Present(B, 116)) == SwapchainPresentSelection::Reject);
    Accept(&policy, Present(B, 2100), SwapchainPresentSelection::Activate);
}

} // namespace

int main() {
    TestActivePrimarySurvivesIdleAuxiliaryInit();
    TestFirstActualPresenterOverridesInitOrder();
    TestSameDeviceAuxiliaryDestroyAndResizeAreIgnored();
    TestRegularAuxiliaryPresentsNeverFlap();
    TestActiveDestroyResizeRecreateAndRepeatedLateLoad();
    TestForegroundLauncherTransitionsIncludingSameApi();
    TestPrimaryFastPathPreservesWindowAndClock();
    TestSameWindowStaleLauncherTransition();
    TestConservativeWindowAndClockHandling();
    std::cout << "Swapchain selection policy tests passed\n";
    return 0;
}