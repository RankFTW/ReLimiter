// Direct production policy and capture-independent registry tests. These do
// not prove a game's marker/Present causal mapping or displayed pacing.
#include "native_present_gate.h"
#include <cstring>
#include "frame_timeline.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>

#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #value); std::abort(); } } while (false)

namespace {
NativeGateContext Context(uint64_t frame) {
    return {0x1000, 10, 2, 3, static_cast<int64_t>(frame * 100'000), 10'000'000, true, true};
}
PacingPlanKey Key(uint64_t frame) { return {PacingIdentityDomain::NvApiFrame, 2, 3, frame}; }
NativeGateTrace Frame(NativePresentGateRoute* route, uint64_t frame, NativeGateMode mode = NativeGateMode::Native) {
    auto context = Context(frame);
    const bool native = route->Marker(context, Key(frame), mode, 1);
    route->MarkerReleased(context.now_qpc + (native ? 10 : 50'000));
    route->BeginScope(context.native_swapchain, context.swapchain_generation);
    context.now_qpc += 1000;
    auto trace = route->BeginCall(context, 0, 1);
    CHECK((trace.status == NativeGateStatus::Native) == native);
    route->EndCall(true, false);
    route->EndScope(context.native_swapchain, context.swapchain_generation);
    return trace;
}
void Warm(NativePresentGateRoute* route) {
    for (uint64_t frame = 1; frame <= 32; ++frame) {
        CHECK(Frame(route, frame).status == NativeGateStatus::Warming);
        CHECK(route->QualifiedFrames() == frame);
    }
}
void TestQualificationAndShadow() {
    NativePresentGateRoute route;
    Warm(&route);
    const auto trace = Frame(&route, 33);
    // The route carries the exact key to the native Present; the target is
    // left to the H2/H3 executors (plan and grid), never the marker.
    CHECK(trace.source_frame_id == 33 && trace.producer_epoch == 2 && trace.controller_generation == 3);
    CHECK(trace.marker_release_qpc == trace.marker_arrival_qpc + 10);
    CHECK(trace.native_arrival_qpc == trace.marker_arrival_qpc + 1000);
    CHECK(trace.target_qpc == 0 && trace.plan_status == 0);
    CHECK(trace.swapchain_generation == 10 && trace.settings_generation == 1);
    CHECK(Frame(&route, 35).status == NativeGateStatus::Warming); // genuine missing ID
    NativePresentGateRoute shadow;
    for (uint64_t frame = 1; frame <= 40; ++frame)
        CHECK(Frame(&shadow, frame, NativeGateMode::Shadow).status == NativeGateStatus::Shadow);
}
void TestRejectionMatrix() {
    for (int scenario = 0; scenario < 15; ++scenario) {
        NativePresentGateRoute route;
        Warm(&route);
        auto context = Context(33);
        CHECK(route.Marker(context, Key(33), NativeGateMode::Native, 1));
        route.MarkerReleased(context.now_qpc);
        if (scenario != 0) route.BeginScope(context.native_swapchain, context.swapchain_generation);
        if (scenario == 1) ++context.native_swapchain;
        if (scenario == 2) ++context.swapchain_generation;
        if (scenario == 3) ++context.producer_epoch;
        if (scenario == 4) ++context.controller_generation;
        if (scenario == 5) context.supported = false;
        if (scenario == 6) context.no_frame_generation = false;
        if (scenario == 7) context.now_qpc -= 1;
        if (scenario == 8) context.now_qpc += 5'000'001;
        if (scenario == 9) context.qpc_frequency = 0;
        if (scenario == 13) route.BeginScope(context.native_swapchain, context.swapchain_generation);
        if (scenario == 14) route.BeginScope(0x2000, context.swapchain_generation);
        const uint32_t flags = scenario == 10 ? 1 : (scenario == 11 ? 8 : 0);
        const auto trace = route.BeginCall(context, flags, scenario == 12 ? 2 : 1);
        CHECK(trace.status != NativeGateStatus::Native);
        route.EndCall(true, false);
        route.EndScope(context.native_swapchain, context.swapchain_generation);
        CHECK(route.QualifiedFrames() == 0);
        CHECK(Frame(&route, 34).status == NativeGateStatus::Warming);
    }
}
void TestExactlyOnceAndFailures() {
    for (int scenario = 0; scenario < 5; ++scenario) {
        NativePresentGateRoute route;
        Warm(&route);
        auto context = Context(33);
        CHECK(route.Marker(context, Key(33), NativeGateMode::Native, 1));
        route.BeginScope(context.native_swapchain, context.swapchain_generation);
        CHECK(route.BeginCall(context, 0, 1).status == NativeGateStatus::Native);
        if (scenario == 0) {
            CHECK(route.BeginCall(context, 0, 1).status == NativeGateStatus::Nested);
            route.EndCall(true, false);
        }
        route.EndCall(scenario != 1, scenario == 2);
        if (scenario == 3) {
            CHECK(route.BeginCall(context, 0, 1).status == NativeGateStatus::MultipleCalls);
            route.EndCall(true, false);
        }
        if (scenario != 4) route.EndScope(context.native_swapchain, context.swapchain_generation);
        // A missing finish callback also invalidates on the next marker.
        CHECK(Frame(&route, 34).status == NativeGateStatus::Warming);
    }
}
void TestMarkerEligibilityAndFg() {
    for (int scenario = 0; scenario < 8; ++scenario) {
        NativePresentGateRoute route;
        Warm(&route);
        auto context = Context(33);
        auto key = Key(33);
        if (scenario == 0) context.no_frame_generation = false;
        if (scenario == 1) context.supported = false;
        if (scenario == 2) key.identity_domain = PacingIdentityDomain::PclFrame;
        if (scenario == 3) context.swapchain_generation = 11;
        if (scenario == 4) ++key.controller_generation;
        if (scenario == 5) key.frame_id = 0;
        if (scenario == 6) key.producer_epoch = 0;
        CHECK(!route.Marker(context, key, NativeGateMode::Native, scenario == 7 ? 2 : 1));
    }
    NativePresentGateRoute fg;
    for (uint64_t frame = 1; frame < 100; ++frame) {
        auto context = Context(frame);
        context.no_frame_generation = false;
        CHECK(!fg.Marker(context, Key(frame), NativeGateMode::Native, 1));
        fg.BeginScope(context.native_swapchain, context.swapchain_generation);
        CHECK(fg.BeginCall(context, 0, 1).status != NativeGateStatus::Native);
        fg.EndCall(true, false);
        fg.EndScope(context.native_swapchain, context.swapchain_generation);
        CHECK(fg.QualifiedFrames() == 0);
    }
}
// The thread-local production route behind NativeGate_OnMarker/NativeGateCall:
// every marker feeds it, and it qualifies after kQualificationFrames complete
// mappings in a selecting mode. Shadow never qualifies for selection.
void RouteFrame(uint64_t frame, bool expect_native) {
    auto context = Context(frame);
    CHECK(NativeGate_OnMarker(context, Key(frame)) == expect_native);
    NativeGate_OnMarkerReleased(context.now_qpc + 10);
    NativeGate_BeginScope(context.native_swapchain, context.swapchain_generation);
    context.now_qpc += 1000;
    {
        NativeGateCall call(context, 0);
        CHECK(call.trace.source_frame_id == frame);
        CHECK((call.trace.status == NativeGateStatus::Native) == expect_native);
        call.Complete(true, false);
    }
    NativeGate_EndScope(context.native_swapchain, context.swapchain_generation);
}
void TestRouteQualificationPublication() {
    std::thread route_thread([] {
        NativeGate_SetMode(NativeGateMode::Marker);
        CHECK(!NativeGate_IsRouteQualified());
        for (uint64_t frame = 1; frame <= NativePresentGateRoute::kQualificationFrames; ++frame) {
            RouteFrame(frame, false);
            CHECK(NativeGate_GetQualifiedFrames() == frame);
        }
        // Marker (the default) runs the route with native selection.
        CHECK(NativeGate_IsRouteQualified());
        RouteFrame(NativePresentGateRoute::kQualificationFrames + 1, true);
        // A genuine gap resets qualification and the published readout.
        const uint64_t epoch = NativeGate_PredictionEpoch();
        RouteFrame(NativePresentGateRoute::kQualificationFrames + 3, false);
        CHECK(!NativeGate_IsRouteQualified());
        CHECK(NativeGate_PredictionEpoch() > epoch);
        // Shadow observes only and never qualifies for selection.
        NativeGate_SetMode(NativeGateMode::Shadow);
        CHECK(!NativeGate_IsRouteQualified());
        NativeGate_SetMode(NativeGateMode::Marker);
    });
    route_thread.join();
}
void TestCaptureAndThreadIsolation() {
    CHECK(!FrameTimeline_IsEnabled());
    const uint64_t generation = FrameTimeline_RegisterPresentSwapchain(0x1000, PresentTimelineApi::D3D12);
    CHECK(generation != 0 && FrameTimeline_GetPresentSwapchainGeneration(0x1000) == generation);
    PresentTimelineCall call{};
    CHECK(!FrameTimeline_BeginPresentCall(0x1000, PresentTimelineEntryPoint::DxgiPresent, &call));
    NativePresentGateRoute route;
    Warm(&route);
    CHECK(Frame(&route, 33).status == NativeGateStatus::Native); // no CSV dependency
    FrameTimeline_UnregisterPresentSwapchain(0x1000);
    CHECK(FrameTimeline_GetPresentSwapchainGeneration(0x1000) == 0);
    CHECK(FrameTimeline_RegisterPresentSwapchain(0x1000, PresentTimelineApi::D3D12) != generation);
    NativeGate_SetMode(NativeGateMode::Native);
    NativeGate_PublishNoFrameGeneration(true, 3);
    CHECK(NativeGate_HasNoFrameGeneration(3) && !NativeGate_HasNoFrameGeneration(4));
    const auto context = Context(1);
    CHECK(!NativeGate_OnMarker(context, Key(1)));
    // A Present on another thread takes the newest marker (hand-off),
    // observed only (Shadow) until telemetry verifies the mapping...
    std::thread other([&] {
        NativeGate_BeginScope(context.native_swapchain, context.swapchain_generation);
        {
            NativeGateCall native(context, 0);
            CHECK(native.trace.source_frame_id == 1 && native.trace.status == NativeGateStatus::Shadow);
            CHECK(native.trace.cross_thread);
            native.Complete(true, false);
        }
        NativeGate_EndScope(context.native_swapchain, context.swapchain_generation);
        // ...exactly once: a second Present for the same marker has no frame.
        NativeGate_BeginScope(context.native_swapchain, context.swapchain_generation);
        {
            NativeGateCall again(context, 0);
            CHECK(again.trace.source_frame_id == 0 && again.trace.status == NativeGateStatus::Unmapped);
            again.Complete(true, false);
        }
        NativeGate_EndScope(context.native_swapchain, context.swapchain_generation);
    });
    other.join();
    NativeGate_SetMode(NativeGateMode::Marker);
    CHECK(!NativeGate_HasNoFrameGeneration(3));
}
// Cross-thread hand-off (Gears of War: E-Day): PRESENT_START on one thread, Present on another,
// strictly alternating. The Present thread's route qualifies after 32
// hand-offs and then selects native carriage; a Present without a new marker
// or a skipped frame resets qualification.
void TestCrossThreadHandoff() {
    NativeGate_SetMode(NativeGateMode::Marker);
    std::atomic<uint64_t> request{0};   // frame the Present thread should present; 0 = none
    std::atomic<uint64_t> done{0};      // newest frame the Present thread finished
    std::atomic<int> status{-1};
    std::atomic<uint64_t> mapped{0};
    std::atomic<bool> stop{false};
    std::thread present([&] {
        uint64_t last = 0;
        while (!stop.load()) {
            const uint64_t frame = request.load();
            if (frame == last) { std::this_thread::yield(); continue; }
            last = frame;
            const uint64_t real = frame & 0xFFFF'FFFFu;
            auto context = Context(real);
            context.now_qpc += 1000;
            NativeGate_BeginScope(context.native_swapchain, context.swapchain_generation);
            {
                NativeGateCall call(context, 0);
                mapped.store(call.trace.source_frame_id);
                status.store(static_cast<int>(call.trace.status));
                call.Complete(true, false);
            }
            NativeGate_EndScope(context.native_swapchain, context.swapchain_generation);
            done.store(frame);
        }
    });
    uint64_t serial = 0;
    // marker: publish frame `marker` (0: none); then the Present thread presents for `present_frame`.
    const auto step = [&](uint64_t marker, uint64_t present_frame) {
        if (marker != 0) {
            const auto context = Context(marker);
            NativeGate_OnMarker(context, Key(marker));
            NativeGate_OnMarkerReleased(context.now_qpc + 10);
        }
        const uint64_t token = (++serial << 32) | present_frame;
        request.store(token);
        while (done.load() != token) std::this_thread::yield();
    };
    // Unverified: taken frames are observed only and never qualify.
    for (uint64_t frame = 101; frame < 141; ++frame) {
        step(frame, frame);
        CHECK(mapped.load() == frame && status.load() == static_cast<int>(NativeGateStatus::Shadow));
    }
    CHECK(!NativeGate_IsRouteQualified() && !NativeGate_CrossThreadVerified());
    // 63 agreeing ETW joins are not enough; the 64th verifies.
    NativeGate_ReportCrossThreadEvidence(63, 0);
    CHECK(!NativeGate_CrossThreadVerified());
    NativeGate_ReportCrossThreadEvidence(1, 0);
    CHECK(NativeGate_CrossThreadVerified());
    // The consecutive frames observed so far carry over: native at once.
    step(141, 141);
    CHECK(mapped.load() == 141 && status.load() == static_cast<int>(NativeGateStatus::Native));
    CHECK(NativeGate_IsRouteQualified());
    // A Present without a new marker: no frame, and qualification resets.
    step(0, 141);
    CHECK(mapped.load() == 0 && status.load() == static_cast<int>(NativeGateStatus::Unmapped));
    CHECK(!NativeGate_IsRouteQualified() && NativeGate_GetQualifiedFrames() == 0);
    // Rebuild, then skip a frame: the gap restarts warming.
    for (uint64_t frame = 142; frame < 142 + NativePresentGateRoute::kQualificationFrames; ++frame) step(frame, frame);
    CHECK(NativeGate_IsRouteQualified());
    step(174, 174);
    CHECK(mapped.load() == 174 && status.load() == static_cast<int>(NativeGateStatus::Native));
    step(176, 176);
    CHECK(mapped.load() == 176 && status.load() == static_cast<int>(NativeGateStatus::Warming));
    CHECK(!NativeGate_IsRouteQualified());
    // One disagreement (a Present running a frame behind its markers) revokes
    // the verification: taken frames are observed only again.
    for (uint64_t frame = 177; frame < 177 + NativePresentGateRoute::kQualificationFrames; ++frame) step(frame, frame);
    CHECK(NativeGate_IsRouteQualified());
    const uint64_t epoch = NativeGate_PredictionEpoch();
    NativeGate_ReportCrossThreadEvidence(10, 1);
    CHECK(!NativeGate_CrossThreadVerified() && NativeGate_PredictionEpoch() > epoch);
    step(209, 209);
    CHECK(status.load() == static_cast<int>(NativeGateStatus::Shadow));
    CHECK(!NativeGate_IsRouteQualified());
    stop.store(true);
    present.join();
}
// A thread that presents its own markers offers nothing to
// other Present threads (DLSS-G's present thread, a loading thread), and a
// frame with frame generation is never offered.
void TestSameThreadOwnerKeepsMarker() {
    NativeGate_SetMode(NativeGateMode::Marker);
    std::thread owner([] {
        for (uint64_t frame = 301; frame <= 305; ++frame) RouteFrame(frame, false);
        auto context = Context(306);
        NativeGate_OnMarker(context, Key(306));
        std::thread thief([context] {
            NativeGate_BeginScope(context.native_swapchain, context.swapchain_generation);
            {
                NativeGateCall call(context, 0);
                CHECK(call.trace.source_frame_id == 0 && !call.trace.cross_thread);
                call.Complete(true, false);
            }
            NativeGate_EndScope(context.native_swapchain, context.swapchain_generation);
        });
        thief.join();
        // The owner still presents its own frame.
        NativeGate_BeginScope(context.native_swapchain, context.swapchain_generation);
        {
            NativeGateCall call(context, 0);
            CHECK(call.trace.source_frame_id == 306 && !call.trace.cross_thread);
            call.Complete(true, false);
        }
        NativeGate_EndScope(context.native_swapchain, context.swapchain_generation);
    });
    owner.join();
    // Frame generation: never offered.
    std::thread marker([] {
        auto context = Context(401);
        context.no_frame_generation = false;
        NativeGate_OnMarker(context, Key(401));
        std::thread present([context] {
            NativeGate_BeginScope(context.native_swapchain, context.swapchain_generation);
            {
                NativeGateCall call(context, 0);
                CHECK(call.trace.source_frame_id == 0);
                call.Complete(true, false);
            }
            NativeGate_EndScope(context.native_swapchain, context.swapchain_generation);
        });
        present.join();
        // A withdrawn marker is not taken either.
        auto plain = Context(402);
        NativeGate_OnMarker(plain, Key(402));
        NativeGate_WithdrawMarker();
        std::thread late([plain] {
            NativeGate_BeginScope(plain.native_swapchain, plain.swapchain_generation);
            {
                NativeGateCall call(plain, 0);
                CHECK(call.trace.source_frame_id == 0);
                call.Complete(true, false);
            }
            NativeGate_EndScope(plain.native_swapchain, plain.swapchain_generation);
        });
        late.join();
    });
    marker.join();
}
void TestFgObservationDoesNotAuthorizeEnforcement() {
    NativePresentGateRoute route;
    for(uint64_t frame=1;frame<=40;++frame) {
        auto context=Context(frame);
        context.no_frame_generation=false;context.observe_frame_generation=true;
        CHECK(!route.Marker(context,Key(frame),NativeGateMode::Shadow,1));
        route.BeginScope(context.native_swapchain,context.swapchain_generation);
        auto trace=route.BeginCall(context,0,1);
        CHECK(trace.status==NativeGateStatus::FrameGenerationShadow);
        route.EndCall(true,false,&trace);
        NativeGateTrace completed;
        CHECK(route.EndScope(context.native_swapchain,context.swapchain_generation,&completed));
        CHECK(completed.status==NativeGateStatus::FrameGenerationShadow);
    }
    CHECK(route.QualifiedFrames()==32);
    // Forty diagnostic mappings must never qualify the first no-FG frame.
    CHECK(Frame(&route,41).status==NativeGateStatus::Warming);
    auto context=Context(42);
    context.no_frame_generation=false;context.observe_frame_generation=true;
    CHECK(!route.Marker(context,Key(42),NativeGateMode::Native,1));
    CHECK(route.Status()==NativeGateStatus::FrameGeneration);
    CHECK(!route.Marker(context,Key(43),NativeGateMode::Shadow,1));
    route.BeginScope(context.native_swapchain,context.swapchain_generation);
    context.observe_frame_generation=false;
    CHECK(route.BeginCall(context,0,1).status==NativeGateStatus::Stale);
    route.EndCall(true,false);
    CHECK(!route.EndScope(context.native_swapchain,context.swapchain_generation));
    CHECK(route.QualifiedFrames()==0);
}
// Native route diagnostics: a scope that ends without a Present call counts, a scope with one call does
// not, and the rejected-Present counters are separate per entry point and monotonic.
void TestRouteDiagnosticCounters() {
    NativePresentGateRoute route;
    const uint64_t before = NativeGate_ScopesWithoutCalls();
    Frame(&route, 1);
    CHECK(NativeGate_ScopesWithoutCalls() == before);
    const auto context = Context(2);
    route.Marker(context, Key(2), NativeGateMode::Native, 1);   // not yet qualified: warming
    route.MarkerReleased(context.now_qpc);
    route.BeginScope(context.native_swapchain, context.swapchain_generation);
    CHECK(!route.EndScope(context.native_swapchain, context.swapchain_generation));
    CHECK(NativeGate_ScopesWithoutCalls() == before + 1);
    CHECK(route.QualifiedFrames() == 0);
    const uint64_t present = NativeGate_PresentsRejected(false), present1 = NativeGate_PresentsRejected(true);
    NativeGate_NotePresentRejected(false);
    NativeGate_NotePresentRejected(false);
    NativeGate_NotePresentRejected(true);
    CHECK(NativeGate_PresentsRejected(false) == present + 2 && NativeGate_PresentsRejected(true) == present1 + 1);
}
// Where the route counters put each gate step: a BeginCall before the scope opened, without a marker, after
// a scope native mismatch, the Present flags histogram, and a mismatch pair that logs once per change.
void TestRouteDiagnosticOutcomes() {
    using C = NativeGateCallOutcome;
    using S = NativeGateScopeEvent;
    auto at = [](const NativeGateDiagnostics& d, C c) { return d.call[static_cast<size_t>(c)]; };
    auto sc = [](const NativeGateDiagnostics& d, S s) { return d.scope[static_cast<size_t>(s)]; };
    NativePresentGateRoute route;
    auto begin = NativeGate_Diagnostics();
    // No marker pending: BeginScope notes it, BeginCall is unmapped for lack of a pending frame.
    route.BeginScope(0x1000, 10);
    route.BeginCall(Context(1), 0, 1);
    route.EndCall(true, false);
    auto d = NativeGate_Diagnostics();
    CHECK(sc(d, S::NoPending) == sc(begin, S::NoPending) + 1);
    CHECK(at(d, C::UnmappedNoPending) == at(begin, C::UnmappedNoPending) + 1);
    // The hooked Present runs outside the scope (before BeginScope): no scope yet.
    auto context = Context(1);
    route.Marker(context, Key(1), NativeGateMode::Native, 1);
    route.MarkerReleased(context.now_qpc);
    begin = NativeGate_Diagnostics();
    route.BeginCall(context, 0, 1);
    route.EndCall(true, false);
    route.BeginScope(context.native_swapchain, context.swapchain_generation);
    CHECK(!route.EndScope(context.native_swapchain, context.swapchain_generation));
    d = NativeGate_Diagnostics();
    CHECK(at(d, C::UnmappedNoScope) == at(begin, C::UnmappedNoScope) + 1);
    CHECK(d.scopes_without_call == begin.scopes_without_call + 1);
    // A scope opened with another native handle never opens (BeginCall sees no scope), the mismatch is
    // counted at BeginScope and EndScope. The same pair again does not advance the mismatch sequence; a
    // different pair does.
    for (int pass = 0; pass < 3; ++pass) {
        const uint64_t other = pass == 2 ? 0x3333 : 0x2222;
        context = Context(2 + pass);
        route.Marker(context, Key(2 + pass), NativeGateMode::Native, 1);
        route.MarkerReleased(context.now_qpc);
        begin = NativeGate_Diagnostics();
        route.BeginScope(other, context.swapchain_generation);
        route.BeginCall(context, 0, 1);
        route.EndCall(true, false);
        route.EndScope(other, context.swapchain_generation);
        d = NativeGate_Diagnostics();
        CHECK(sc(d, S::NativeMismatch) == sc(begin, S::NativeMismatch) + 1);
        CHECK(at(d, C::UnmappedNoScope) == at(begin, C::UnmappedNoScope) + 1);
        CHECK(sc(d, S::EndNativeMismatch) == sc(begin, S::EndNativeMismatch) + 1);
        CHECK(d.mismatch_scope_native == other && d.mismatch_marker_native == 0x1000);
        CHECK((d.mismatch_sequence != begin.mismatch_sequence) == (pass != 1));
    }
    // Flags histogram: zero, TEST, DO_NOT_WAIT, other; TEST and DO_NOT_WAIT are special Presents.
    const uint32_t flags[] = {0, 0x1, 0x8, 0x200};
    for (uint32_t value : flags) {
        context = Context(10);
        route.Marker(context, Key(10), NativeGateMode::Native, 1);
        route.MarkerReleased(context.now_qpc);
        route.BeginScope(context.native_swapchain, context.swapchain_generation);
        begin = NativeGate_Diagnostics();
        route.BeginCall(context, value, 1);
        route.EndCall(true, false);
        d = NativeGate_Diagnostics();
        const uint64_t zero = d.flags_zero - begin.flags_zero, test = d.flags_test - begin.flags_test,
            dnw = d.flags_do_not_wait - begin.flags_do_not_wait, other = d.flags_other - begin.flags_other;
        CHECK(zero + test + dnw + other == 1);
        CHECK((value == 0 && zero == 1) || (value == 0x1 && test == 1) || (value == 0x8 && dnw == 1) ||
              (value == 0x200 && other == 1));
        CHECK((at(d, C::SpecialPresent) == at(begin, C::SpecialPresent) + 1) == ((value & 0x9u) != 0));
        route.EndScope(context.native_swapchain, context.swapchain_generation);
    }
    NativeGate_NoteEntry(NativeGateEntry::TypedPresent);
    d = NativeGate_Diagnostics();
    CHECK(d.entries[static_cast<size_t>(NativeGateEntry::TypedPresent)] != 0 &&
          d.entry_thread[static_cast<size_t>(NativeGateEntry::TypedPresent)] != 0);
    NativeGate_NoteEntry(NativeGateEntry::VtablePresent);
    NativeGate_NoteEntry(NativeGateEntry::VtablePresent1);
    NativeGate_NoteEntry(NativeGateEntry::InnerPassThrough);
    d = NativeGate_Diagnostics();
    CHECK(d.entries[static_cast<size_t>(NativeGateEntry::VtablePresent)] != 0 &&
          d.entries[static_cast<size_t>(NativeGateEntry::VtablePresent1)] != 0 &&
          d.entries[static_cast<size_t>(NativeGateEntry::InnerPassThrough)] != 0);
    char text[1400];
    CHECK(NativeGate_DiagnosticsText(text, sizeof(text)) > 0 && std::strstr(text, "begin_call mapped=") != nullptr);
    CHECK(std::strstr(text, "vtable_present=") != nullptr && std::strstr(text, "vtable_present1=") != nullptr &&
          std::strstr(text, "inner_pass_through=") != nullptr);
}
}
int main() {
    TestRouteDiagnosticCounters();
    TestRouteDiagnosticOutcomes();
    // Mode changes bump the settings generation and invalidate the
    // prediction epoch exactly once per change.
    NativeGate_SetMode(NativeGateMode::Native);
    auto epoch = NativeGate_PredictionEpoch();
    NativeGate_SetMode(NativeGateMode::Native);
    CHECK(NativeGate_PredictionEpoch() == epoch);
    NativeGate_SetMode(NativeGateMode::Shadow);
    CHECK(NativeGate_GetMode() == NativeGateMode::Shadow);
    CHECK(NativeGate_PredictionEpoch() > epoch);
    NativeGate_SetMode(NativeGateMode::Marker);
    CHECK(NativeGate_GetMode() == NativeGateMode::Marker);
    // The scheduler's FG observation selects the route mode: Shadow while FG
    // presents (FG observers and Streamline admission run only there), Native
    // otherwise. A change invalidates the prediction epoch once; repeating
    // the same observation changes nothing and keeps the no-FG token.
    epoch = NativeGate_PredictionEpoch();
    NativeGate_ObserveFrameGeneration(2, 9);
    CHECK(NativeGate_GetMode() == NativeGateMode::Shadow);
    CHECK(!NativeGate_HasNoFrameGeneration(9));
    CHECK(!NativeGate_IsRouteQualified());
    CHECK(NativeGate_PredictionEpoch() == epoch + 1);
    NativeGate_ObserveFrameGeneration(2, 9);
    CHECK(NativeGate_PredictionEpoch() == epoch + 1);
    NativeGate_ObserveFrameGeneration(1, 9);
    CHECK(NativeGate_GetMode() == NativeGateMode::Native);
    CHECK(NativeGate_HasNoFrameGeneration(9));
    CHECK(NativeGate_PredictionEpoch() == epoch + 2);
    NativeGate_ObserveFrameGeneration(1, 9);
    CHECK(NativeGate_HasNoFrameGeneration(9));
    CHECK(NativeGate_PredictionEpoch() == epoch + 2);
    NativeGate_SetMode(NativeGateMode::Marker);
    TestQualificationAndShadow();
    TestRejectionMatrix();
    TestExactlyOnceAndFailures();
    TestMarkerEligibilityAndFg();
    TestRouteQualificationPublication();
    TestCaptureAndThreadIsolation();
    TestFgObservationDoesNotAuthorizeEnforcement();
    TestCrossThreadHandoff();
    TestSameThreadOwnerKeepsMarker();
    std::puts("Native Present gate production routing tests passed (no live pacing validation).");
}
