#pragma once

#include <cstdint>

// Pure edge detection of one hotkey, no Windows. The hotkey poller (hotkey_poller.cpp) feeds it
// sampled key states every 8 ms from its own service thread; the previous owner of this logic was
// osd.cpp's PollHotkey, run per frame on the presenting thread, and Step is that function with
// its key reads made lazy (a modifier is read only with the main key down, which `pressed`
// required anyway) and its state kept out of the UI.
namespace hotkey_edge {

struct Binding {
    int vk = 0;  // 0: unbound
    bool ctrl = false;
    bool alt = false;
    bool shift = false;
};

struct Modifiers {
    bool ctrl = false;
    bool alt = false;
    bool shift = false;
    bool super = false;  // either Windows key: any chord with it is not a binding
};

struct State {
    bool was_pressed = false;
    bool wait_for_release = false;
};

// A binding in one word, for the atomic that hands it from the configuration owner to the poller.
// vk is a virtual-key code 1..254.
constexpr uint32_t Pack(const Binding& binding) noexcept {
    return (static_cast<uint32_t>(binding.vk) & 0xFFu) | (binding.ctrl ? 0x100u : 0u) |
           (binding.alt ? 0x200u : 0u) | (binding.shift ? 0x400u : 0u);
}

constexpr Binding Unpack(uint32_t packed) noexcept {
    return Binding{ static_cast<int>(packed & 0xFFu), (packed & 0x100u) != 0, (packed & 0x200u) != 0,
                    (packed & 0x400u) != 0 };
}

// The key must be seen released before the binding can fire again: a capture in progress, a
// binding that just changed (its key is probably still held).
constexpr void Rearm(State* state) noexcept {
    state->was_pressed = false;
    state->wait_for_release = true;
}

// One poll of one hotkey. True on the release -> press edge of the exact chord (the main key down,
// Ctrl, Alt and Shift each exactly as bound, neither Windows key); held keys do not repeat.
// `suppressed` (a capture is drawing) never fires and demands a release before the next fire.
// `read_key()` samples the main key, `read_modifiers()` returns the modifiers; each is called at
// most once and only when its answer matters.
template <typename ReadKey, typename ReadModifiers>
inline bool Step(const Binding& binding, State* state, bool suppressed, ReadKey&& read_key,
                 ReadModifiers&& read_modifiers) noexcept {
    if (binding.vk == 0) {
        *state = {};
        return false;
    }
    if (suppressed) {
        Rearm(state);
        return false;
    }
    const bool key_down = read_key();
    if (state->wait_for_release) {
        state->was_pressed = false;
        if (!key_down) state->wait_for_release = false;
        return false;
    }
    bool pressed = false;
    if (key_down) {
        const Modifiers modifiers = read_modifiers();
        pressed = binding.ctrl == modifiers.ctrl && binding.alt == modifiers.alt &&
                  binding.shift == modifiers.shift && !modifiers.super;
    }
    const bool triggered = pressed && !state->was_pressed;
    state->was_pressed = pressed;
    return triggered;
}

}  // namespace hotkey_edge
