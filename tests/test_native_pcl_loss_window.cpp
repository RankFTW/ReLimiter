#include "native_pcl_loss_window.h"

#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
constexpr uint64_t kFrequency = 10'000'000;
constexpr uint64_t kQuietTicks =
    kFrequency * NATIVE_PCL_LOSS_WINDOW_QUIET_PERIOD_MILLISECONDS / 1000;
constexpr uint64_t kStart = 1'000'000'000;

using SingleCounterWindow = NativePclLossWindow<1>;
using TripleCounterWindow = NativePclLossWindow<3>;

void Check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

bool Matches(const NativePclLossWindowUpdate& update,
             bool began, bool ended, bool active) {
    return update.accepted && update.began == began &&
        update.ended == ended && update.active == active;
}

void TestQuietPeriod() {
    constexpr uint64_t maximum = (std::numeric_limits<uint64_t>::max)();
    Check(NATIVE_PCL_LOSS_WINDOW_QUIET_PERIOD_MILLISECONDS == 2000,
          "quiet period is two seconds");
    Check(SingleCounterWindow::QuietPeriodTicks(kFrequency) == 20'000'000,
          "10 MHz quiet period ticks");
    Check(SingleCounterWindow::QuietPeriodTicks(3'579'545) == 7'159'090,
          "ACPI PM timer quiet period ticks");
    Check(SingleCounterWindow::QuietPeriodTicks(1'001) == 2'002,
          "sub-millisecond remainder is kept");
    Check(SingleCounterWindow::QuietPeriodTicks(1) == 2, "one tick per second");
    Check(SingleCounterWindow::QuietPeriodTicks(maximum) == maximum,
          "huge frequency saturates instead of overflowing");
}

void TestOpening() {
    SingleCounterWindow window;
    Check(!window.IsActive() && window.WindowCount() == 0 &&
          window.WindowBeginQpc() == 0, "new tracker has no window");
    Check(Matches(window.Update({ 0 }, kStart, kFrequency), false, false, false),
          "zero loss does not open a window");
    Check(Matches(window.Update({ 0 }, kStart + kFrequency, kFrequency),
                  false, false, false), "unchanged counters keep it closed");
    Check(Matches(window.Update({ 3 }, kStart + 2 * kFrequency, kFrequency),
                  true, false, true) && window.IsActive(),
          "counter increase opens a window");
    Check(window.WindowBeginQpc() == kStart + 2 * kFrequency &&
          window.WindowCount() == 1, "window records its opening time");

    SingleCounterWindow first_query;
    Check(Matches(first_query.Update({ 5 }, kStart, kFrequency), true, false, true),
          "loss counted before the first query opens a window");
}

void TestExtensionByFurtherLoss() {
    SingleCounterWindow window;
    window.Update({ 1 }, kStart, kFrequency);
    Check(Matches(window.Update({ 2 }, kStart + kQuietTicks - 1, kFrequency),
                  false, false, true), "further loss extends the open window");
    Check(Matches(window.Advance(kStart + kQuietTicks, kFrequency),
                  false, false, true), "extension restarts the quiet period");
    Check(Matches(window.Advance(kStart + 2 * kQuietTicks - 2, kFrequency),
                  false, false, true), "open until the extended period elapses");
    Check(Matches(window.Advance(kStart + 2 * kQuietTicks - 1, kFrequency),
                  false, true, false),
          "closes one quiet period after the last increase");
    Check(window.WindowCount() == 1 && window.WindowBeginQpc() == kStart,
          "an extension is not a new window");
}

void TestClosingAfterQuietPeriod() {
    SingleCounterWindow window;
    window.Update({ 1 }, kStart, kFrequency);
    Check(Matches(window.Update({ 1 }, kStart + kQuietTicks - 1, kFrequency),
                  false, false, true), "open until the quiet period elapses");
    Check(Matches(window.Update({ 1 }, kStart + kQuietTicks, kFrequency),
                  false, true, false) && !window.IsActive(),
          "closes at exactly the quiet period");
    Check(Matches(window.Advance(kStart + 2 * kQuietTicks, kFrequency),
                  false, false, false), "a closed window stays closed");
    Check(Matches(window.Update({ 1 }, kStart + 3 * kQuietTicks, kFrequency),
                  false, false, false),
          "unchanged cumulative counters do not reopen");
}

void TestRebaselineOnCounterDecrease() {
    SingleCounterWindow window;
    window.Update({ 100 }, kStart, kFrequency);
    Check(Matches(window.Advance(kStart + kQuietTicks, kFrequency),
                  false, true, false), "first window closes");
    Check(Matches(window.Update({ 5 }, kStart + kQuietTicks + kFrequency, kFrequency),
                  false, false, false),
          "counter decrease re-baselines without opening a window");
    Check(Matches(window.Update({ 5 }, kStart + kQuietTicks + 2 * kFrequency,
                                kFrequency), false, false, false),
          "the new baseline is steady");
    Check(Matches(window.Update({ 6 }, kStart + kQuietTicks + 3 * kFrequency,
                                kFrequency), true, false, true) &&
          window.WindowCount() == 2,
          "increase over the new baseline opens a window");

    SingleCounterWindow open_window;
    open_window.Update({ 10 }, kStart, kFrequency);
    Check(Matches(open_window.Update({ 0 }, kStart + kQuietTicks / 2, kFrequency),
                  false, false, true), "decrease inside a window keeps it open");
    Check(Matches(open_window.Advance(kStart + kQuietTicks, kFrequency),
                  false, true, false), "decrease does not extend the window");
}

void TestMultipleCounters() {
    TripleCounterWindow window;
    Check(Matches(window.Update({ 0, 0, 0 }, kStart, kFrequency),
                  false, false, false), "no loss on any counter");
    Check(Matches(window.Update({ 0, 0, 1 }, kStart + 1, kFrequency),
                  true, false, true), "any single counter opens a window");
    Check(Matches(window.Update({ 4, 0, 1 }, kStart + kQuietTicks, kFrequency),
                  false, false, true), "another counter extends the window");
    Check(Matches(window.Advance(kStart + 2 * kQuietTicks, kFrequency),
                  false, true, false), "closes after the last increase of any counter");
    Check(Matches(window.Update({ 0, 0, 1 }, kStart + 3 * kQuietTicks, kFrequency),
                  false, false, false), "one counter decreasing opens nothing");
    Check(Matches(window.Update({ 0, 7, 0 }, kStart + 4 * kQuietTicks, kFrequency),
                  true, false, true),
          "increase in one counter opens while another re-baselines lower");
    Check(Matches(window.Advance(kStart + 5 * kQuietTicks, kFrequency),
                  false, true, false), "second window closes");
    Check(Matches(window.Update({ 0, 7, 1 }, kStart + 6 * kQuietTicks, kFrequency),
                  true, false, true) && window.WindowCount() == 3,
          "a re-baselined counter detects later loss");
}

void TestInvalidInput() {
    SingleCounterWindow window;
    auto update = window.Update({ 1 }, kStart, 0);
    Check(!update.accepted && !update.began && !update.active && !window.IsActive(),
          "zero frequency is rejected");
    update = window.Update({ 1 }, 0, kFrequency);
    Check(!update.accepted && !window.IsActive(), "zero time is rejected");
    Check(Matches(window.Update({ 1 }, kStart, kFrequency), true, false, true),
          "a rejected input leaves its increase pending");

    update = window.Update({ 2 }, kStart - 1, kFrequency);
    Check(!update.accepted && !update.began && update.active,
          "time going backwards is rejected and reports current state");
    Check(Matches(window.Advance(kStart + kQuietTicks, kFrequency),
                  false, true, false), "a rejected increase does not extend");
    Check(Matches(window.Update({ 2 }, kStart + kQuietTicks, kFrequency),
                  true, false, true) && window.WindowCount() == 2,
          "equal time is accepted and detects the pending increase");

    update = window.Advance(kStart + kQuietTicks - 1, kFrequency);
    Check(!update.accepted && update.active, "Advance rejects backwards time");
    update = window.Advance(kStart + 10 * kQuietTicks, 0);
    Check(!update.accepted && update.active && window.IsActive(),
          "zero frequency cannot close a window");
    Check(Matches(window.Advance(kStart + kQuietTicks, kFrequency),
                  false, false, true),
          "rejected far-future time does not move the clock");
    Check(Matches(window.Advance(kStart + 2 * kQuietTicks, kFrequency),
                  false, true, false), "valid time closes the window");
}

void TestTransitionReporting() {
    struct Step {
        uint64_t counter;
        uint64_t time;
        bool began;
        bool ended;
        bool active;
    };
    const Step steps[] = {
        { 0, kStart, false, false, false },
        { 1, kStart + kFrequency, true, false, true },
        { 2, kStart + 2 * kFrequency, false, false, true },     // extension
        { 2, kStart + 3 * kFrequency, false, false, true },
        { 2, kStart + 4 * kFrequency, false, true, false },     // 2 s quiet
        { 2, kStart + 5 * kFrequency, false, false, false },
        { 3, kStart + 6 * kFrequency, true, false, true },
        { 3, kStart + 8 * kFrequency - 1, false, false, true },
        { 9, kStart + 8 * kFrequency + 1, true, true, true },   // close + reopen
        { 0, kStart + 9 * kFrequency, false, false, true },     // decrease only
        { 0, kStart + 10 * kFrequency + 1, false, true, false },
    };
    SingleCounterWindow window;
    size_t index = 0;
    for (const Step& step : steps) {
        const auto update = window.Update({ step.counter }, step.time, kFrequency);
        Check(Matches(update, step.began, step.ended, step.active),
              "transition step " + std::to_string(index) + ": began=" +
              std::to_string(update.began) + " ended=" +
              std::to_string(update.ended) + " active=" +
              std::to_string(update.active));
        ++index;
    }
    Check(window.WindowCount() == 3 &&
          window.WindowBeginQpc() == kStart + 8 * kFrequency + 1,
          "a close-and-reopen update counts as a new window");

    SingleCounterWindow advance_only;
    advance_only.Update({ 1 }, kStart, kFrequency);
    const auto update = advance_only.Advance(kStart + kQuietTicks, kFrequency);
    Check(Matches(update, false, true, false), "Advance reports only ended");
}
} // namespace

int main() {
    try {
        TestQuietPeriod();
        TestOpening();
        TestExtensionByFurtherLoss();
        TestClosingAfterQuietPeriod();
        TestRebaselineOnCounterDecrease();
        TestMultipleCounters();
        TestInvalidInput();
        TestTransitionReporting();
        std::cout << "native PCL loss window tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
