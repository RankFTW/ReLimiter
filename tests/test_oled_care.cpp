// Direct tests of production oled_care_policy.h, not a copied worker model.
// Integration owner: register test_oled_care / oled_care with C++17 and src
// include directory in tests/CMakeLists.txt; no Windows or XInput libraries.
// These tests do not prove window creation, cursor balance, Win32 polling,
// focus/z-order, dynamic DLL resolution, or shutdown/join integration.
// Manual runtime checks (x64 + Win32): cover all/game-only, move the game and
// hotplug displays; toggle while capped; ordinary blackout + alt-tab; auto
// dismiss by keyboard, each mouse button/movement and all four controllers;
// reconnect controllers; disable idle in auto/manual sessions; force a failed
// cover then retry/cancel; unload/reinit active and retain a timed-out worker.
#include "oled_care_policy.h"

#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <limits>

namespace {

void Check(bool condition, const char* expression, int line) {
    if (condition) return;
    std::cerr << "FAILED line " << line << ": " << expression << '\n';
    std::exit(1);
}
#define CHECK(expression) Check((expression), #expression, __LINE__)

constexpr uint64_t MINUTE = 60000;
constexpr uint64_t LARGE_TICK = (uint64_t{1} << 32) + 123456;

void TestDefaultsAndClamping() {
    OLEDCarePolicy policy(0);
    CHECK(policy.AllMonitors());
    CHECK(policy.IdleMinutes() == 0);
    CHECK(!policy.WantsWindows());
    policy.Poll(LARGE_TICK, false);
    CHECK(!policy.WantsWindows());
    CHECK(OLEDCarePolicy::ClampIdleMinutes(std::numeric_limits<int>::min()) == 0);
    CHECK(OLEDCarePolicy::ClampIdleMinutes(std::numeric_limits<int>::max()) == 120);
    CHECK(!policy.Configure(true, -1, LARGE_TICK));
    CHECK(policy.Configure(false, 121, LARGE_TICK));
    CHECK(!policy.AllMonitors());
    CHECK(policy.IdleMinutes() == 120);
    policy.Poll(LARGE_TICK + 120 * MINUTE - 1, false);
    CHECK(!policy.WantsWindows());
    policy.Poll(LARGE_TICK + 120 * MINUTE, false);
    CHECK(policy.Session() == OLEDCareSession::Automatic);
}

void TestIdle64BitAndInputDismissal() {
    // Cross the DWORD boundary, then start above it; never truncate uptime.
    for (uint64_t start : { (uint64_t{1} << 32) - 1000, LARGE_TICK }) {
        OLEDCarePolicy policy(start);
        CHECK(policy.Configure(true, 1, start));
        policy.Poll(start + MINUTE - 1, false);
        CHECK(!policy.WantsWindows());
        policy.Poll(start + MINUTE, false);
        CHECK(policy.Session() == OLEDCareSession::Automatic);
        policy.Poll(start + MINUTE + 16, false);
        CHECK(policy.WantsWindows());
        policy.Poll(start + MINUTE + 32, true);
        CHECK(!policy.WantsWindows());
        policy.Poll(start + 2 * MINUTE + 31, false);
        CHECK(!policy.WantsWindows());
        policy.Poll(start + 2 * MINUTE + 32, false);
        CHECK(policy.Session() == OLEDCareSession::Automatic);
    }
    OLEDCarePolicy policy(0);
    policy.Configure(true, 1, 0);
    policy.Poll(MINUTE, true); // Activity wins over expiry on the same tick.
    CHECK(!policy.WantsWindows());
    policy.Poll(2 * MINUTE - 1, false);
    CHECK(!policy.WantsWindows());
    policy.Poll(2 * MINUTE, false);
    CHECK(policy.WantsWindows());
}

void TestManualSessions() {
    OLEDCarePolicy policy(0);
    policy.Configure(true, 1, 0);
    policy.ApplyCommand(OLEDCareCommand::Toggle, 10);
    CHECK(policy.Session() == OLEDCareSession::Manual);
    policy.Poll(LARGE_TICK, true);
    CHECK(policy.Session() == OLEDCareSession::Manual);
    policy.Poll(LARGE_TICK + MINUTE, false);
    CHECK(policy.Session() == OLEDCareSession::Manual);
    policy.ApplyCommand(OLEDCareCommand::Toggle, LARGE_TICK + MINUTE);
    CHECK(!policy.WantsWindows());
    policy.Poll(LARGE_TICK + 2 * MINUTE - 1, false);
    CHECK(!policy.WantsWindows());
    policy.Poll(LARGE_TICK + 2 * MINUTE, false);
    CHECK(policy.Session() == OLEDCareSession::Automatic);
    policy.ApplyCommand(OLEDCareCommand::Activate, LARGE_TICK + 2 * MINUTE);
    CHECK(policy.Session() == OLEDCareSession::Manual);
    policy.Poll(LARGE_TICK + 2 * MINUTE + 1, true);
    CHECK(policy.Session() == OLEDCareSession::Manual);
    policy.ApplyCommand(OLEDCareCommand::Activate, LARGE_TICK + 2 * MINUTE + 2);
    CHECK(policy.Session() == OLEDCareSession::Manual);
    policy.Configure(false, 0, LARGE_TICK + 2 * MINUTE + 3);
    CHECK(policy.Session() == OLEDCareSession::Manual);
    policy.ApplyCommand(OLEDCareCommand::Dismiss, LARGE_TICK + 2 * MINUTE + 4);
    CHECK(!policy.WantsWindows());
    policy.Poll(LARGE_TICK + 100 * MINUTE, false);
    CHECK(!policy.WantsWindows());
}

void TestReconfigurationAndDismissBaseline() {
    OLEDCarePolicy policy(0);
    policy.Configure(true, 1, LARGE_TICK); // Enabling starts a new baseline.
    policy.Poll(LARGE_TICK + MINUTE - 1, false);
    CHECK(!policy.WantsWindows());
    CHECK(!policy.Configure(true, 1, LARGE_TICK + MINUTE - 1));
    policy.Poll(LARGE_TICK + MINUTE, false);
    CHECK(policy.Session() == OLEDCareSession::Automatic);
    policy.Configure(true, 0, LARGE_TICK + MINUTE + 1);
    CHECK(!policy.WantsWindows());
    policy.Configure(true, 1, LARGE_TICK + MINUTE + 2);
    policy.Poll(LARGE_TICK + 2 * MINUTE, false);
    CHECK(!policy.WantsWindows());
    policy.Configure(false, 1, LARGE_TICK + 2 * MINUTE);
    policy.Poll(LARGE_TICK + 3 * MINUTE - 1, false);
    CHECK(!policy.WantsWindows());
    policy.Configure(false, 2, LARGE_TICK + 3 * MINUTE - 1);
    policy.Poll(LARGE_TICK + 5 * MINUTE - 2, false);
    CHECK(!policy.WantsWindows());
    policy.Poll(LARGE_TICK + 5 * MINUTE - 1, false);
    CHECK(policy.Session() == OLEDCareSession::Automatic);
    policy.ApplyCommand(OLEDCareCommand::Toggle, LARGE_TICK + 5 * MINUTE);
    CHECK(!policy.WantsWindows());
    policy.Poll(LARGE_TICK + 7 * MINUTE - 1, false);
    CHECK(!policy.WantsWindows());
    // Explicit dismissal of already off also starts a full fresh interval.
    policy.ApplyCommand(OLEDCareCommand::Dismiss, LARGE_TICK + 7 * MINUTE - 1);
    policy.Poll(LARGE_TICK + 9 * MINUTE - 2, false);
    CHECK(!policy.WantsWindows());
    policy.Poll(LARGE_TICK + 9 * MINUTE - 1, false);
    CHECK(policy.Session() == OLEDCareSession::Automatic);
}

void TestBackwardTickAndPendingWindowIntent() {
    OLEDCarePolicy policy(LARGE_TICK);
    policy.Configure(true, 1, LARGE_TICK);
    policy.Poll(10, false); // Defensive fake-clock rollback, not unsigned expiry.
    CHECK(!policy.WantsWindows());
    policy.Poll(MINUTE + 9, false);
    CHECK(!policy.WantsWindows());
    policy.Poll(MINUTE + 10, false);
    CHECK(policy.Session() == OLEDCareSession::Automatic);
    // Visibility is separate: window failure must not consume the request.
    // The worker can retry while WantsWindows() remains true. This checks
    // intent only, not the Win32 retry implementation or published active flag.
    policy.Poll(2 * MINUTE, false);
    CHECK(policy.WantsWindows());
    policy.Poll(2 * MINUTE + 1, true);
    CHECK(!policy.WantsWindows());
    policy.ApplyCommand(OLEDCareCommand::Activate, 2 * MINUTE + 2);
    policy.Poll(10 * MINUTE, true);
    CHECK(policy.WantsWindows());
    policy.ApplyCommand(OLEDCareCommand::Dismiss, 10 * MINUTE + 1);
    CHECK(!policy.WantsWindows());
}

void TestMailboxComposition() {
    constexpr OLEDCareCommand commands[] = {OLEDCareCommand::None, OLEDCareCommand::Toggle,
        OLEDCareCommand::Activate, OLEDCareCommand::Dismiss};
    for (auto session : {OLEDCareSession::Off, OLEDCareSession::Manual, OLEDCareSession::Automatic}) {
        // Exhaustively compare 4-command bursts against sequential application,
        // including two toggles of automatic -> off -> manual, and idle reset.
        for (unsigned int sequence = 0; sequence < 256; ++sequence) {
            OLEDCarePolicy sequential(0);
            sequential.Configure(true, 1, 0);
            if (session == OLEDCareSession::Manual)
                sequential.ApplyCommand(OLEDCareCommand::Activate, 0);
            if (session == OLEDCareSession::Automatic) sequential.Poll(MINUTE, false);
            OLEDCarePolicy combined = sequential;
            OLEDCareCommand pending = OLEDCareCommand::None;
            for (unsigned int step = 0; step < 4; ++step) {
                const auto command = commands[(sequence >> (step * 2)) & 3U];
                sequential.ApplyCommand(command, MINUTE + 10);
                pending = OLEDCare_ComposeCommand(pending, command);
            }
            combined.ApplyCommand(pending, MINUTE + 10);
            CHECK(combined.Session() == sequential.Session());
            combined.Poll(2 * MINUTE + 9, false);
            sequential.Poll(2 * MINUTE + 9, false);
            CHECK(combined.Session() == sequential.Session());
            combined.Poll(2 * MINUTE + 10, false);
            sequential.Poll(2 * MINUTE + 10, false);
            CHECK(combined.Session() == sequential.Session());
        }
    }
}

void TestKeyboardMouseEdgesAndCursor() {
    OLEDCareInputTracker tracker;
    OLEDCareInputSample sample;
    sample.pressed[65] = true;
    sample.cursor_valid = true;
    sample.cursor_x = -100;
    CHECK(!tracker.Observe(sample)); // Seed, not a fictitious first press/move.
    CHECK(!tracker.Observe(sample));
    sample.pressed[65] = false;
    CHECK(!tracker.Observe(sample)); // Release is not a pressed edge.
    for (std::size_t key = 1; key < 255; ++key) {
        sample.pressed[key] = true;
        CHECK(tracker.Observe(sample));
        CHECK(!tracker.Observe(sample)); // Held keys don't repeatedly dismiss.
        sample.pressed[key] = false;
        CHECK(!tracker.Observe(sample));
    }
    --sample.cursor_x;
    CHECK(tracker.Observe(sample));
    ++sample.cursor_y;
    CHECK(tracker.Observe(sample));
    CHECK(!tracker.Observe(sample));
    sample.cursor_valid = false;
    sample.cursor_x = 10000;
    CHECK(!tracker.Observe(sample));
    sample.cursor_valid = true;
    CHECK(!tracker.Observe(sample)); // Reacquisition seeds, not false motion.
    ++sample.cursor_x;
    CHECK(tracker.Observe(sample));
}

void TestAllControllerSlotsAndReconnect() {
    OLEDCareInputTracker tracker;
    OLEDCareInputSample sample;
    for (auto& controller : sample.controllers) controller = {true, 123};
    CHECK(!tracker.Observe(sample));
    for (std::size_t slot = 0; slot < sample.controllers.size(); ++slot) {
        auto& controller = sample.controllers[slot];
        ++controller.packet;
        CHECK(tracker.Observe(sample));
        CHECK(!tracker.Observe(sample));
        controller.connected = false;
        CHECK(!tracker.Observe(sample));
        CHECK(!tracker.Observe(sample));
        controller.connected = true;
        CHECK(tracker.Observe(sample)); // Reconnect with same packet.
        controller.packet = std::numeric_limits<uint32_t>::max();
        CHECK(tracker.Observe(sample));
        controller.packet = 0;
        CHECK(tracker.Observe(sample)); // Packet wrap is a change, not ordering.
    }
    sample.pressed[1] = true;
    ++sample.controllers[3].packet;
    CHECK(tracker.Observe(sample));
    CHECK(!tracker.Observe(sample)); // No short-circuit leaving stale slots.
}

void TestInputTrackerDrivesSessionPolicy() {
    OLEDCareInputTracker tracker;
    OLEDCareInputSample sample;
    OLEDCarePolicy policy(0);
    policy.Configure(true, 1, 0);
    policy.Poll(0, tracker.Observe(sample));
    policy.Poll(MINUTE, tracker.Observe(sample));
    CHECK(policy.Session() == OLEDCareSession::Automatic);
    sample.controllers[3] = {true, 0};
    policy.Poll(MINUTE + 16, tracker.Observe(sample));
    CHECK(!policy.WantsWindows());
    policy.ApplyCommand(OLEDCareCommand::Activate, MINUTE + 32);
    sample.pressed[1] = true;
    sample.controllers[3].packet = 1;
    policy.Poll(MINUTE + 32, tracker.Observe(sample));
    CHECK(policy.Session() == OLEDCareSession::Manual);
}

} // namespace

int main() {
    TestDefaultsAndClamping();
    TestIdle64BitAndInputDismissal();
    TestManualSessions();
    TestReconfigurationAndDismissBaseline();
    TestBackwardTickAndPendingWindowIntent();
    TestMailboxComposition();
    TestKeyboardMouseEdgesAndCursor();
    TestAllControllerSlotsAndReconnect();
    TestInputTrackerDrivesSessionPolicy();
    std::cout << "OLED Care policy tests passed\n";
    return 0;
}