#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

enum class OLEDCareSession { Off, Manual, Automatic };
enum class OLEDCareCommand { None, Toggle, Activate, Dismiss, ToggleTwice };

// Compose a bounded mailbox without losing two toggles before the next poll.
inline OLEDCareCommand OLEDCare_ComposeCommand(
    OLEDCareCommand pending, OLEDCareCommand next) noexcept {
    if (next == OLEDCareCommand::None) return pending;
    if (next != OLEDCareCommand::Toggle) return next;
    switch (pending) {
    case OLEDCareCommand::Activate: return OLEDCareCommand::Dismiss;
    case OLEDCareCommand::Dismiss: return OLEDCareCommand::Activate;
    // Auto -> off -> manual is not an identity operation. Even off -> off
    // must reset the idle baseline, unlike an empty mailbox.
    case OLEDCareCommand::Toggle: return OLEDCareCommand::ToggleTwice;
    case OLEDCareCommand::ToggleTwice: return OLEDCareCommand::Toggle;
    default: return OLEDCareCommand::Toggle;
    }
}

class OLEDCarePolicy {
public:
    explicit OLEDCarePolicy(uint64_t now_ms) noexcept : last_activity_ms_(now_ms) {}

    static constexpr int ClampIdleMinutes(int minutes) noexcept {
        return minutes < 0 ? 0 : (minutes > 120 ? 120 : minutes);
    }

    // Identical UI/config publications must not continually postpone idle.
    bool Configure(bool all_monitors, int idle_minutes, uint64_t now_ms) noexcept {
        idle_minutes = ClampIdleMinutes(idle_minutes);
        if (all_monitors_ == all_monitors && idle_minutes_ == idle_minutes)
            return false;
        all_monitors_ = all_monitors;
        idle_minutes_ = idle_minutes;
        last_activity_ms_ = now_ms;
        if (idle_minutes == 0 && session_ == OLEDCareSession::Automatic)
            session_ = OLEDCareSession::Off;
        return true;
    }

    void ApplyCommand(OLEDCareCommand command, uint64_t now_ms) noexcept {
        if (command == OLEDCareCommand::None) return;
        const bool activate = command == OLEDCareCommand::Activate ||
            (command == OLEDCareCommand::Toggle && session_ == OLEDCareSession::Off) ||
            (command == OLEDCareCommand::ToggleTwice && session_ != OLEDCareSession::Off);
        session_ = activate ? OLEDCareSession::Manual : OLEDCareSession::Off;
        // Includes dismissing an already hidden/failed window request.
        last_activity_ms_ = now_ms;
    }

    void Poll(uint64_t now_ms, bool input_activity) noexcept {
        if (now_ms < last_activity_ms_) last_activity_ms_ = now_ms;
        if (input_activity) {
            last_activity_ms_ = now_ms;
            if (session_ == OLEDCareSession::Automatic)
                session_ = OLEDCareSession::Off;
        }
        if (session_ == OLEDCareSession::Off && idle_minutes_ > 0 &&
            now_ms - last_activity_ms_ >= static_cast<uint64_t>(idle_minutes_) * 60000ULL) {
            session_ = OLEDCareSession::Automatic;
        }
    }

    OLEDCareSession Session() const noexcept { return session_; }
    bool WantsWindows() const noexcept { return session_ != OLEDCareSession::Off; }
    bool AllMonitors() const noexcept { return all_monitors_; }
    int IdleMinutes() const noexcept { return idle_minutes_; }

private:
    OLEDCareSession session_ = OLEDCareSession::Off;
    bool all_monitors_ = true;
    int idle_minutes_ = 0;
    uint64_t last_activity_ms_;
};

struct OLEDCareInputSample {
    struct Controller {
        bool connected = false;
        uint32_t packet = 0;
    };
    std::array<bool, 256> pressed{}; // Win32 virtual keys, including mouse buttons.
    bool cursor_valid = false;
    int32_t cursor_x = 0;
    int32_t cursor_y = 0;
    std::array<Controller, 4> controllers{};
};

class OLEDCareInputTracker {
public:
    bool Observe(const OLEDCareInputSample& sample) noexcept {
        bool activity = false;
        if (initialized_) {
            for (std::size_t i = 0; i < sample.pressed.size(); ++i)
                activity |= sample.pressed[i] && !previous_.pressed[i];
            activity |= sample.cursor_valid && previous_.cursor_valid &&
                (sample.cursor_x != previous_.cursor_x || sample.cursor_y != previous_.cursor_y);
            for (std::size_t i = 0; i < sample.controllers.size(); ++i) {
                const auto& current = sample.controllers[i];
                const auto& previous = previous_.controllers[i];
                // Seed initial packets; reconnect counts as activity even if
                // the driver reuses its old packet. Disconnect alone does not.
                activity |= current.connected &&
                    (!previous.connected || current.packet != previous.packet);
            }
        }
        previous_ = sample;
        initialized_ = true;
        return activity;
    }

private:
    OLEDCareInputSample previous_{};
    bool initialized_ = false;
};