#include "nvapi_marker_policy.h"
#include "nvapi_hooks.h"

#include <cstdlib>
#include <cstdint>
#include <iostream>

namespace {

constexpr uint32_t SIMULATION_START = 0;
constexpr uint32_t RENDER_SUBMIT_START = 2;

void Check(bool condition, const char* expression)
{
    if (condition) return;
    std::cerr << "FAILED: " << expression << '\n';
    std::exit(1);
}

#define CHECK(expression) Check((expression), #expression)

void WarmDirectStream(NvApiMarkerPolicy& policy)
{
    for (uint64_t frame_id = 1;
         frame_id <= NvApiMarkerPolicy::DIRECT_MARKER_WARMUP;
         ++frame_id) {
        CHECK(policy.ShouldProcess(
            false,
            RENDER_SUBMIT_START,
            frame_id));
    }
}

void TestRtssCannotCreateMarkerPath()
{
    NvApiMarkerPolicy policy;
    for (uint64_t frame_id = 1; frame_id <= 64; ++frame_id) {
        CHECK(!policy.ShouldProcess(true, SIMULATION_START, frame_id));
    }
    CHECK(policy.DirectMarkerCount() == 0);
    CHECK(policy.RtssSimulationStartCount() == 64);
    CHECK(policy.AdmittedRtssSimulationStartCount() == 0);
}

void TestRtssCompletesPartialDirectStream()
{
    NvApiMarkerPolicy policy;
    WarmDirectStream(policy);

    CHECK(policy.ShouldProcess(true, SIMULATION_START, 100));
    CHECK(policy.AdmittedRtssSimulationStartCount() == 1);
}

void TestDirectSimulationStartAlwaysWins()
{
    NvApiMarkerPolicy policy;
    CHECK(policy.ShouldProcess(false, SIMULATION_START, 1));
    WarmDirectStream(policy);

    CHECK(!policy.ShouldProcess(true, SIMULATION_START, 100));
    CHECK(policy.AdmittedRtssSimulationStartCount() == 0);
}

void TestLateDirectDuplicateDoesNotEnforceTwice()
{
    NvApiMarkerPolicy policy;
    WarmDirectStream(policy);

    CHECK(policy.ShouldProcess(true, SIMULATION_START, 100));
    CHECK(!policy.ShouldProcess(false, SIMULATION_START, 100));
    CHECK(!policy.ShouldProcess(true, SIMULATION_START, 101));
}

void TestZeroIdDirectSimulationStartDoesNotBlockSupplement()
{
    NvApiMarkerPolicy policy;
    CHECK(policy.ShouldProcess(false, SIMULATION_START, 0));
    WarmDirectStream(policy);

    CHECK(policy.ShouldProcess(true, SIMULATION_START, 100));
}

void TestProducerResetClearsAdmissionHistory()
{
    NvApiMarkerPolicy policy;
    WarmDirectStream(policy);
    CHECK(policy.ShouldProcess(true, SIMULATION_START, 100));
    CHECK(policy.DirectMarkerCount() == NvApiMarkerPolicy::DIRECT_MARKER_WARMUP);

    policy.Reset();
    CHECK(policy.DirectMarkerCount() == 0);
    CHECK(policy.RtssSimulationStartCount() == 0);
    CHECK(policy.AdmittedRtssSimulationStartCount() == 0);
    CHECK(!policy.ShouldProcess(true, SIMULATION_START, 1));
}

void TestInvalidRtssMarkersStayFiltered()
{
    NvApiMarkerPolicy policy;
    WarmDirectStream(policy);

    CHECK(!policy.ShouldProcess(true, RENDER_SUBMIT_START, 100));
    CHECK(!policy.ShouldProcess(true, SIMULATION_START, 0));
    CHECK(policy.RtssSimulationStartCount() == 0);
}

// These tests directly exercise the production publication predicate, not COM
// or the hook implementation. Addresses are opaque identity sentinels and are
// never dereferenced. Each reassignment represents a deterministic interleaving.
void TestFeedbackPublicationRequiresExactIdentity()
{
    int device_a = 0;
    int device_b = 0;
    const NvAPIFeedbackDevice a{
        reinterpret_cast<IUnknown*>(&device_a), 7, 12 };
    CHECK(NvAPI_IsSameFeedbackPublication(a, a));
    CHECK(!NvAPI_IsSameFeedbackPublication({}, {}));
    CHECK(!NvAPI_IsSameFeedbackPublication(a, {}));
    CHECK(!NvAPI_IsSameFeedbackPublication({}, a));
    CHECK(!NvAPI_IsSameFeedbackPublication(a, {
        reinterpret_cast<IUnknown*>(&device_b), 7, 12 }));
    CHECK(!NvAPI_IsSameFeedbackPublication(a, { a.device, 8, 12 }));
    CHECK(!NvAPI_IsSameFeedbackPublication(a, { a.device, 7, 14 }));
    const NvAPIFeedbackDevice invalid{ a.device, 7, 13 };
    CHECK(!NvAPI_IsSameFeedbackPublication(invalid, invalid));
}

void TestDestroyTokenCannotClearReplacementOrReusedPointer()
{
    int device_a = 0;
    int device_b = 0;
    NvAPIFeedbackDevice current{
        reinterpret_cast<IUnknown*>(&device_a), 1, 2 };
    const NvAPIFeedbackDevice destroy_token = current;
    current = { reinterpret_cast<IUnknown*>(&device_b), 2, 4 };
    CHECK(!NvAPI_IsSameFeedbackPublication(destroy_token, current));

    // Even an identical pointer and a wrapped/repeated epoch cannot turn an
    // old destroy token into permission to clear a newer publication.
    current = { destroy_token.device, destroy_token.producer_epoch, 6 };
    CHECK(!NvAPI_IsSameFeedbackPublication(destroy_token, current));
    CHECK(NvAPI_IsSameFeedbackPublication(current, current));
}

void TestWaitingSleepParamsCannotOverwriteReplacement()
{
    int device_a = 0;
    int device_b = 0;
    NvAPIFeedbackDevice current{
        reinterpret_cast<IUnknown*>(&device_a), 1, 2 };
    const NvAPIFeedbackDevice waiting_a = current;

    // B publishes and saves params before A acquires GameParamsGuard. The
    // model below uses the actual predicate evaluated by the in-lock check;
    // this is not a test of the production lock or its memory ordering.
    current = { reinterpret_cast<IUnknown*>(&device_b), 2, 4 };
    NvAPIFeedbackDevice restore_identity = current;
    uint32_t restore_interval = 9000;
    if (NvAPI_IsSameFeedbackPublication(waiting_a, current)) {
        restore_identity = waiting_a;
        restore_interval = 16000;
    }
    CHECK(NvAPI_IsSameFeedbackPublication(restore_identity, current));
    CHECK(restore_interval == 9000);
}

void TestForwardingCannotRelabelOldMarkerOrWarmNewPolicy()
{
    int device_a = 0;
    int device_b = 0;
    NvAPIFeedbackDevice current{
        reinterpret_cast<IUnknown*>(&device_a), 11, 22 };
    const NvAPIFeedbackDevice marker_token = current;
    NvApiMarkerPolicy policy;

    // Replacement during the native call: post-forward validation must reject
    // A before policy counters, marker health, predictor, or scheduling work.
    current = { reinterpret_cast<IUnknown*>(&device_b), 12, 24 };
    if (NvAPI_IsSameFeedbackPublication(marker_token, current)) {
        policy.ShouldProcess(false, RENDER_SUBMIT_START, 100);
    }
    CHECK(policy.DirectMarkerCount() == 0);
    CHECK(marker_token.producer_epoch == 11);
    CHECK(marker_token.producer_epoch != current.producer_epoch);

    // Clear-and-republish of the very same device is still a different source.
    current = { marker_token.device, 13, 26 };
    CHECK(!NvAPI_IsSameFeedbackPublication(marker_token, current));
    CHECK(NvAPI_IsSameFeedbackPublication(current, current));
}

} // namespace

int main()
{
    TestRtssCannotCreateMarkerPath();
    TestRtssCompletesPartialDirectStream();
    TestDirectSimulationStartAlwaysWins();
    TestLateDirectDuplicateDoesNotEnforceTwice();
    TestZeroIdDirectSimulationStartDoesNotBlockSupplement();
    TestProducerResetClearsAdmissionHistory();
    TestInvalidRtssMarkersStayFiltered();
    TestFeedbackPublicationRequiresExactIdentity();
    TestDestroyTokenCannotClearReplacementOrReusedPointer();
    TestWaitingSleepParamsCannotOverwriteReplacement();
    TestForwardingCannotRelabelOldMarkerOrWarmNewPolicy();
    std::cout << "NvAPI marker policy tests passed\n";
    return 0;
}