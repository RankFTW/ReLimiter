// Production hotkey_edge.h and hotkey_poller.cpp tests (C++17, Windows): the edge detection of a
// hotkey (the previous per-frame PollHotkey of osd.cpp kept as the reference the new Step must
// match step for step), and the poller's hand-over: bindings, triggers, suppression, rearm, with a
// fake key reader instead of GetAsyncKeyState. They do not exercise the blackout thread's timer
// loop, the OSD callback that applies a trigger, or a real keyboard; those are game-side.
#include "hotkey_edge.h"
#include "hotkey_poller.h"

#include <atomic>
#include <chrono>
#include <Windows.h>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <thread>

namespace {

using hotkey_edge::Binding;
using hotkey_edge::Modifiers;
using hotkey_edge::State;

void Check(bool condition, const char* expression, int line) {
    if (condition) return;
    std::fprintf(stderr, "FAIL line %d: %s\n", line, expression);
    std::exit(1);
}
#define CHECK(expression) Check(!!(expression), #expression, __LINE__)

// The logic of osd.cpp's PollHotkey before the poller existed, with the key states passed in.
bool ReferencePoll(const Binding& kb, State* state, bool suppressed, bool key_down, const Modifiers& m) {
    if (kb.vk == 0) {
        *state = {};
        return false;
    }
    if (suppressed) {
        state->was_pressed = false;
        state->wait_for_release = true;
        return false;
    }
    if (state->wait_for_release) {
        state->was_pressed = false;
        if (!key_down) state->wait_for_release = false;
        return false;
    }
    const bool pressed = key_down && kb.ctrl == m.ctrl && kb.alt == m.alt && kb.shift == m.shift && !m.super;
    const bool triggered = pressed && !state->was_pressed;
    state->was_pressed = pressed;
    return triggered;
}

bool StepWith(const Binding& kb, State* state, bool suppressed, bool key_down, const Modifiers& m,
              int* key_reads = nullptr, int* modifier_reads = nullptr) {
    return hotkey_edge::Step(
        kb, state, suppressed,
        [&] { if (key_reads) ++*key_reads; return key_down; },
        [&] { if (modifier_reads) ++*modifier_reads; return m; });
}

void TestPackUnpack() {
    for (int vk = 0; vk < 256; ++vk) {
        for (int flags = 0; flags < 8; ++flags) {
            const Binding in{ vk, (flags & 1) != 0, (flags & 2) != 0, (flags & 4) != 0 };
            const Binding out = hotkey_edge::Unpack(hotkey_edge::Pack(in));
            CHECK(out.vk == in.vk && out.ctrl == in.ctrl && out.alt == in.alt && out.shift == in.shift);
        }
    }
    CHECK(hotkey_edge::Pack(Binding{}) == 0);
}

void TestDirected() {
    const Binding plain{ 0x21 };
    const Binding chord{ 0x4B, true, false, true };
    const Modifiers none, ctrl_shift{ true, false, true, false };
    State state;

    // A press fires once; holding does not repeat; a release and press fires again.
    CHECK(!StepWith(plain, &state, false, false, none));
    CHECK(StepWith(plain, &state, false, true, none));
    for (int i = 0; i < 10; ++i) CHECK(!StepWith(plain, &state, false, true, none));
    CHECK(!StepWith(plain, &state, false, false, none));
    CHECK(StepWith(plain, &state, false, true, none));

    // The chord is exact: extra or missing modifiers and either Windows key do not fire; the
    // modifiers arriving while the key is held do (as before).
    state = {};
    CHECK(!StepWith(chord, &state, false, true, none));
    CHECK(StepWith(chord, &state, false, true, ctrl_shift));
    CHECK(!StepWith(chord, &state, false, true, ctrl_shift));
    CHECK(!StepWith(chord, &state, false, true, Modifiers{ true, true, true, false }));
    CHECK(!StepWith(chord, &state, false, true, Modifiers{ true, false, true, true }));
    state = {};
    CHECK(!StepWith(plain, &state, false, true, Modifiers{ false, false, false, true }));

    // Suppression never fires and demands a release afterwards.
    state = {};
    CHECK(!StepWith(plain, &state, true, true, none));
    CHECK(!StepWith(plain, &state, false, true, none));
    CHECK(!StepWith(plain, &state, false, true, none));
    CHECK(!StepWith(plain, &state, false, false, none));
    CHECK(StepWith(plain, &state, false, true, none));

    // Rearm (a capture, a changed binding) is the same demand.
    state = {};
    hotkey_edge::Rearm(&state);
    CHECK(!StepWith(plain, &state, false, true, none));
    CHECK(!StepWith(plain, &state, false, false, none));
    CHECK(StepWith(plain, &state, false, true, none));

    // Unbound: state reset, nothing fires, no key is read.
    state = { true, true };
    int key_reads = 0, modifier_reads = 0;
    CHECK(!StepWith(Binding{}, &state, false, true, none, &key_reads, &modifier_reads));
    CHECK(!state.was_pressed && !state.wait_for_release && key_reads == 0 && modifier_reads == 0);

    // Reads are lazy: modifiers only with the main key down and no release owed; nothing while
    // suppressed.
    state = {};
    key_reads = modifier_reads = 0;
    StepWith(chord, &state, false, false, none, &key_reads, &modifier_reads);
    CHECK(key_reads == 1 && modifier_reads == 0);
    StepWith(chord, &state, false, true, none, &key_reads, &modifier_reads);
    CHECK(key_reads == 2 && modifier_reads == 1);
    key_reads = modifier_reads = 0;
    StepWith(chord, &state, true, true, none, &key_reads, &modifier_reads);
    CHECK(key_reads == 0 && modifier_reads == 0);
    key_reads = modifier_reads = 0;
    StepWith(chord, &state, false, true, none, &key_reads, &modifier_reads);  // release owed
    CHECK(key_reads == 1 && modifier_reads == 0);
}

// Step against the previous implementation, over a long random walk of keys, modifiers,
// suppression, rearms and bindings.
void TestDifferential() {
    std::mt19937 rng(12345);
    const Binding bindings[] = { {}, { 0x21 }, { 0x4B, true }, { 0x4B, false, true }, { 0x4B, true, true, true },
                                 { 0x70, false, false, true } };
    State actual, expected;
    Binding binding = bindings[1];
    bool key_down = false;
    Modifiers mods;
    unsigned triggers = 0;
    for (int step = 0; step < 400000; ++step) {
        const unsigned dice = rng() % 100;
        if (dice < 20) key_down = !key_down;
        if (dice >= 20 && dice < 30) mods.ctrl = !mods.ctrl;
        if (dice >= 30 && dice < 34) mods.alt = !mods.alt;
        if (dice >= 34 && dice < 40) mods.shift = !mods.shift;
        if (dice >= 40 && dice < 42) mods.super = !mods.super;
        if (dice == 42) binding = bindings[rng() % 6];
        const bool rearm = dice == 43;
        const bool suppressed = dice >= 44 && dice < 47;
        if (rearm) {
            hotkey_edge::Rearm(&actual);
            expected.was_pressed = false;
            expected.wait_for_release = true;
        }
        const bool want = ReferencePoll(binding, &expected, suppressed, key_down, mods);
        const bool got = StepWith(binding, &actual, suppressed, key_down, mods);
        CHECK(got == want);
        CHECK(actual.was_pressed == expected.was_pressed && actual.wait_for_release == expected.wait_for_release);
        triggers += want;
    }
    CHECK(triggers > 1000);  // the walk actually fires
}

// ── The poller ──

std::atomic<bool> g_keys[256];
std::atomic<unsigned> g_reads[256];

bool FakeKey(int vk) noexcept {
    g_reads[vk].fetch_add(1, std::memory_order_relaxed);
    return g_keys[vk].load(std::memory_order_acquire);
}

constexpr int kK = 0x4B, kL = 0x4C;
constexpr HotkeySlot kToggle = HotkeySlot::OsdToggle;
constexpr HotkeySlot kBlackout = HotkeySlot::Blackout;

void Poll() { Hotkeys_PollWith(&FakeKey); }
void Hold(int vk, bool down) { g_keys[vk].store(down, std::memory_order_release); }
uint32_t Bind(int vk, bool ctrl = false) { return hotkey_edge::Pack(Binding{ vk, ctrl }); }

void Reset() {
    for (int slot = 0; slot < static_cast<int>(HotkeySlot::Count); ++slot)
        Hotkeys_SetBinding(static_cast<HotkeySlot>(slot), 0);
    Hotkeys_SetSuppressed(false);
    for (auto& key : g_keys) key.store(false);
    Poll();  // takes the suppression latch, resets every slot
    Poll();
    for (int slot = 0; slot < static_cast<int>(HotkeySlot::Count); ++slot)
        while (Hotkeys_Take(static_cast<HotkeySlot>(slot))) {}
    for (auto& reads : g_reads) reads.store(0);
}

void TestPollerEdges() {
    Reset();
    // Unbound slots read no key at all.
    Poll();
    for (auto& reads : g_reads) CHECK(reads.load() == 0);

    Hotkeys_SetBinding(kToggle, Bind(kK));
    Poll();  // the new binding is armed: it must be seen released first
    CHECK(!Hotkeys_Take(kToggle));
    Hold(kK, true);
    Poll();  // the first poll after binding saw it up (above): this press is an edge
    CHECK(Hotkeys_Take(kToggle));
    CHECK(!Hotkeys_Take(kToggle));  // consumed
    for (int i = 0; i < 5; ++i) { Poll(); CHECK(!Hotkeys_Take(kToggle)); }  // held: no repeat
    Hold(kK, false);
    Poll();
    Hold(kK, true);
    Poll();
    CHECK(Hotkeys_Take(kToggle));
    Hold(kK, false);
    Poll();

    // A press and release between two polls is not seen: the documented bound of the period.
    Hold(kK, true);
    Hold(kK, false);
    Poll();
    CHECK(!Hotkeys_Take(kToggle));

    // Two presses before the consumer runs are one trigger (a latch, as one OSD frame saw one).
    Hold(kK, true); Poll(); Hold(kK, false); Poll(); Hold(kK, true); Poll(); Hold(kK, false); Poll();
    CHECK(Hotkeys_Take(kToggle));
    CHECK(!Hotkeys_Take(kToggle));
}

void TestPollerBindingChangeAndRearm() {
    Reset();
    Hotkeys_SetBinding(kToggle, Bind(kK));
    Poll();
    // The binding changes to a key that is held (just bound by pressing it): no fire until released.
    Hold(kL, true);
    Hotkeys_SetBinding(kToggle, Bind(kL));
    Poll();
    CHECK(!Hotkeys_Take(kToggle));
    Poll();
    CHECK(!Hotkeys_Take(kToggle));
    Hold(kL, false);
    Poll();
    Hold(kL, true);
    Poll();
    CHECK(Hotkeys_Take(kToggle));
    Hold(kL, false);
    Poll();

    // Rearm (a capture drawing for this slot) while the key is held.
    Hold(kL, true);
    Hotkeys_Rearm(kToggle);
    Poll();
    CHECK(!Hotkeys_Take(kToggle));
    Hold(kL, false);
    Poll();
    Hold(kL, true);
    Poll();
    CHECK(Hotkeys_Take(kToggle));
    Hold(kL, false);
    Poll();

    // Cleared: unbound again, the key is no longer read.
    Hotkeys_SetBinding(kToggle, 0);
    Poll();
    g_reads[kL].store(0);
    Hold(kL, true);
    Poll();
    CHECK(!Hotkeys_Take(kToggle) && g_reads[kL].load() == 0);
    Hold(kL, false);
}

void TestPollerSuppression() {
    Reset();
    Hotkeys_SetBinding(kToggle, Bind(kK));
    Hotkeys_SetBinding(kBlackout, Bind(kL));
    Poll();
    // A suppressed window SHORTER than a poll period (set and cleared between two polls) still
    // suppresses, and the keys held through it must be released before they fire.
    Hold(kK, true);
    Hold(kL, true);
    Hotkeys_SetSuppressed(true);
    Hotkeys_SetSuppressed(false);
    Poll();
    CHECK(!Hotkeys_Take(kToggle) && !Hotkeys_Take(kBlackout));
    Poll();
    CHECK(!Hotkeys_Take(kToggle) && !Hotkeys_Take(kBlackout));
    Hold(kK, false);
    Poll();
    Hold(kK, true);
    Poll();
    CHECK(Hotkeys_Take(kToggle) && !Hotkeys_Take(kBlackout));  // L is still held from the suppression
    Hold(kL, false);
    Poll();
    Hold(kL, true);
    Poll();
    CHECK(Hotkeys_Take(kBlackout));
    Hold(kK, false);
    Hold(kL, false);
    Poll();

    // Suppression held across polls: nothing fires; ending it with the key up leaves no debt.
    Hotkeys_SetSuppressed(true);
    Hold(kK, true);
    Poll();
    Poll();
    CHECK(!Hotkeys_Take(kToggle));
    Hold(kK, false);
    Poll();
    Hotkeys_SetSuppressed(false);
    Poll();
    Poll();
    Hold(kK, true);
    Poll();
    CHECK(Hotkeys_Take(kToggle));
    Hold(kK, false);
    Poll();
}

void TestPollerModifiers() {
    Reset();
    Hotkeys_SetBinding(kToggle, Bind(kK, true));
    Poll();
    for (auto& reads : g_reads) reads.store(0);
    Poll();  // the main key is up: no modifier is read
    CHECK(g_reads[VK_LCONTROL].load() == 0 && g_reads[VK_LSHIFT].load() == 0 && g_reads[kK].load() == 1);
    Hold(kK, true);
    Poll();
    CHECK(!Hotkeys_Take(kToggle));  // Ctrl missing
    Hold(VK_RCONTROL, true);
    Poll();
    CHECK(Hotkeys_Take(kToggle));   // either Control key
    Hold(VK_RCONTROL, false);
    Hold(kK, false);
    Poll();
    Hold(VK_LWIN, true);
    Hold(VK_LCONTROL, true);
    Hold(kK, true);
    Poll();
    CHECK(!Hotkeys_Take(kToggle));  // a Windows key vetoes
    Hold(VK_LWIN, false);
    Hold(VK_LCONTROL, false);
    Hold(kK, false);
    Poll();
}

// A poller thread polling flat out against a consumer that presses, waits for the trigger and
// releases: every press arrives exactly once, nothing arrives without one.
void TestPollerThreads() {
    Reset();
    Hotkeys_SetBinding(kToggle, Bind(kK));
    std::atomic<bool> stop{ false };
    std::thread poller([&stop] {
        while (!stop.load(std::memory_order_relaxed)) {
            Poll();
            std::this_thread::yield();
        }
    });
    unsigned fired = 0;
    // Never press before the poller has seen the key up under the new binding, nor before it has
    // seen a release: wait on its reads of the key (a read after the change is a poll after it).
    const auto wait_for_reads = [](unsigned target) {
        for (int wait = 0; wait < 4000 && g_reads[kK].load() < target; ++wait)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        CHECK(g_reads[kK].load() >= target);
    };
    wait_for_reads(2);
    for (int press = 0; press < 100; ++press) {
        Hold(kK, true);
        bool seen = false;
        for (int wait = 0; wait < 4000 && !seen; ++wait) {
            seen = Hotkeys_Take(kToggle);
            if (!seen) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK(seen);
        fired += seen;
        Hold(kK, false);
        wait_for_reads(g_reads[kK].load() + 2);  // two polls after the release: it is seen
        CHECK(!Hotkeys_Take(kToggle));
    }
    stop.store(true);
    poller.join();
    CHECK(fired == 100);
}

// ── A poll in flight across a transition ──

// A fake reader that stops in the middle of a poll: the first read of `g_pause_vk` announces it
// and waits until released. The poll thread has by then frozen its generation and suppression
// snapshot, and has not yet published.
std::atomic<int> g_pause_vk{ 0 };
std::atomic<bool> g_paused{ false };
std::atomic<bool> g_resume{ false };

bool PausableKey(int vk) noexcept {
    const bool down = g_keys[vk].load(std::memory_order_acquire);
    int expected = vk;
    if (g_pause_vk.compare_exchange_strong(expected, 0)) {
        g_paused.store(true, std::memory_order_release);
        while (!g_resume.load(std::memory_order_acquire)) std::this_thread::yield();
    }
    return down;
}

// Runs one poll on another thread, paused at the first read of `vk`; `during` runs on this thread
// while it is paused; the poll then finishes.
template <typename During>
void PollPausedAt(int vk, During&& during) {
    g_resume.store(false);
    g_paused.store(false);
    g_pause_vk.store(vk);
    std::thread poll([] { Hotkeys_PollWith(&PausableKey); });
    while (!g_paused.load(std::memory_order_acquire)) std::this_thread::yield();
    during();
    g_resume.store(true, std::memory_order_release);
    poll.join();
}

void TestPollInFlightAcrossCapture() {
    Reset();
    Hotkeys_SetBinding(kToggle, Bind(kK));
    Poll();  // armed and seen released

    // Control: a poll paused with no transition delivers its press.
    Hold(kK, true);
    PollPausedAt(kK, [] {});
    CHECK(Hotkeys_Take(kToggle));
    Hold(kK, false);
    Poll();

    // A capture begins and ends while the poll is paused after its snapshot and before its
    // publish; the key is pressed during the capture. The poll's trigger is not applied.
    Hold(kK, true);
    PollPausedAt(kK, [] {
        Hotkeys_SetSuppressed(true);
        CHECK(!Hotkeys_Take(kToggle));  // the UI's suppressed callback consumes nothing
        Hotkeys_SetSuppressed(false);
        CHECK(!Hotkeys_Take(kToggle));  // the capture's last callback: nothing published yet
    });
    CHECK(!Hotkeys_Take(kToggle));      // the next unsuppressed callback: the stale trigger is rejected
    CHECK(!Hotkeys_Take(kToggle));
    // The key is still down: the latch of the capture demands a release before the next fire.
    Poll();
    CHECK(!Hotkeys_Take(kToggle));
    Hold(kK, false);
    Poll();
    Hold(kK, true);
    Poll();
    CHECK(Hotkeys_Take(kToggle));
    Hold(kK, false);
    Poll();

    // The same for a capture that has only BEGUN when the poll publishes, and for a rearm and a
    // binding change in flight.
    Hold(kK, true);
    PollPausedAt(kK, [] { Hotkeys_SetSuppressed(true); });
    CHECK(!Hotkeys_Take(kToggle));
    Hotkeys_SetSuppressed(false);
    Hold(kK, false);
    Poll();
    Poll();
    Hold(kK, true);
    PollPausedAt(kK, [] { Hotkeys_Rearm(kToggle); });
    CHECK(!Hotkeys_Take(kToggle));
    Hold(kK, false);
    Poll();
    Hold(kK, true);
    PollPausedAt(kK, [] { Hotkeys_SetBinding(kToggle, Bind(kK, true)); });
    CHECK(!Hotkeys_Take(kToggle));
    Hold(kK, false);
    Hotkeys_SetBinding(kToggle, 0);
    Poll();
}

// ── Who polls ──

void TestFallbackSelection() {
    Reset();
    Hotkeys_SetBinding(kToggle, Bind(kK));
    Hotkeys_SetServiceThreadPolling(false);
    CHECK(!Hotkeys_ServiceThreadPolling());
    Hotkeys_PollFallbackWith(&FakeKey);  // armed, seen released
    CHECK(g_reads[kK].load() >= 1);
    // No service thread: the overlay callback's poll does the whole job.
    Hold(kK, true);
    Hotkeys_PollFallbackWith(&FakeKey);
    CHECK(Hotkeys_Take(kToggle));
    Hold(kK, false);
    Hotkeys_PollFallbackWith(&FakeKey);

    // A service thread polls: the fallback does not read a key, so it never polls twice.
    Hotkeys_SetServiceThreadPolling(true);
    CHECK(Hotkeys_ServiceThreadPolling());
    for (auto& reads : g_reads) reads.store(0);
    Hold(kK, true);
    for (int i = 0; i < 5; ++i) Hotkeys_PollFallbackWith(&FakeKey);
    for (auto& reads : g_reads) CHECK(reads.load() == 0);
    CHECK(!Hotkeys_Take(kToggle));
    Poll();  // the service thread's own poll
    CHECK(Hotkeys_Take(kToggle));
    Hold(kK, false);
    Poll();
    Hotkeys_SetServiceThreadPolling(false);
}

}  // namespace

int main() {
    TestPackUnpack();
    TestDirected();
    TestDifferential();
    TestPollerEdges();
    TestPollerBindingChangeAndRearm();
    TestPollerSuppression();
    TestPollerModifiers();
    TestPollerThreads();
    TestPollInFlightAcrossCapture();
    TestFallbackSelection();
    std::puts("hotkey_edge: ok");
    return 0;
}
