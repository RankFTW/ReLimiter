#pragma once

#include <cstdint>

// Hotkeys are sampled off every game thread. One poller (the blackout service thread's loop,
// blackout.cpp) calls Hotkeys_Poll every kHotkeyPollPeriodMs and keeps the edge state of every
// slot (hotkey_edge.h). The config owner publishes changed bindings, and takes
// the triggers after ReShade presents, independently of the ImGui frame.
// A press is seen within one poll period plus timer granularity and applied on
// the next present. Two edges before one present coalesce.
constexpr unsigned kHotkeyPollPeriodMs = 8;

enum class HotkeySlot : uint8_t {
    OsdToggle,
    PresetPrev,
    PresetNext,
    Blackout,
    PositionCycle,
    OledCare,
    Count
};

// ── Config owner ──

// The slot's binding, hotkey_edge::Pack of the parsed binding string (0: unbound). Call when the
// string changes. A changed binding must be seen released before it fires: its key is likely
// still held from binding it.
void Hotkeys_SetBinding(HotkeySlot slot, uint32_t packed_binding) noexcept;
bool Hotkeys_HasBindings() noexcept;
// The slot's key must be seen released before the slot fires again (a capture started or is in
// progress for it).
void Hotkeys_Rearm(HotkeySlot slot) noexcept;
// A key capture is drawing: no slot fires, and every slot waits for a release afterwards. A call
// with true is remembered until the poller has seen it, so a suppressed window shorter than one
// poll period still suppresses.
void Hotkeys_SetSuppressed(bool suppressed) noexcept;
// True once per trigger since the last call; consumed. A trigger whose poll started before the
// latest transition (a capture beginning or ending, a rearm, a binding change) is consumed and
// rejected: it was sampled under the old suppression state.
bool Hotkeys_Take(HotkeySlot slot) noexcept;

// One cheap check before taking the config owner lock. BeginDrain clears the
// notification before consuming slots: an edge arriving later re-notifies.
bool Hotkeys_HasPending() noexcept;
void Hotkeys_BeginDrain() noexcept;

// ── Poller thread ──

// One poll of every slot with GetAsyncKeyState (high bit: the key is down now).
void Hotkeys_Poll() noexcept;
// The same with the key reader of the caller (tests).
using HotkeyKeyReader = bool (*)(int vk) noexcept;
void Hotkeys_PollWith(HotkeyKeyReader read_key) noexcept;

// ── Who polls ──

// The service thread (blackout.cpp) calls this with true once it exists and is polling, and with
// false after it has been joined. While true the present callback never polls.
void Hotkeys_SetServiceThreadPolling(bool polling) noexcept;
bool Hotkeys_ServiceThreadPolling() noexcept;
// The present callback's poll, on the presenting thread, for when the service thread could not be
// created (the per-frame behaviour from before the service thread): polls only while no service thread polls, so
// the two never both poll. The same edge detection and hand-over as the service thread's.
void Hotkeys_PollFallback() noexcept;
void Hotkeys_PollFallbackWith(HotkeyKeyReader read_key) noexcept;
