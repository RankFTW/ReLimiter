#pragma once

#include <cstdint>

// Hotkeys are sampled off every game thread. One poller (the blackout service thread's loop,
// blackout.cpp) calls Hotkeys_Poll every kHotkeyPollPeriodMs and keeps the edge state of every
// slot (hotkey_edge.h); the OSD callback (osd.cpp DrawOSDImpl, which owns g_config under the
// config owner lock) publishes the parsed bindings, and takes the triggers to apply the toggles.
// Before this the OSD callback polled GetAsyncKeyState itself every frame on the presenting
// thread: ~9 key reads per bound hotkey per frame, ParseKeybind of every binding string per call,
// and a blocking NtUserGetAsyncKeyState wait on that thread ~40 times a second.
//
// Reaction: a press is seen by the next poll, at most kHotkeyPollPeriodMs (plus timer
// granularity, about 1 ms) after it starts, and applied by the OSD callback's next run (the next
// frame the overlay event fires, as before, whether or not the overlay is visible). A press
// shorter than the period can be missed; no human key press is (the shortest are 20 to 30 ms).
// Today's per-frame polling saw a press up to one frame time late, 16.7 ms at 60 fps.
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

// ── Config owner (DrawOSDImpl, under the config owner lock) ──

// The slot's binding, hotkey_edge::Pack of the parsed binding string (0: unbound). Call when the
// string changes. A changed binding must be seen released before it fires: its key is likely
// still held from binding it.
void Hotkeys_SetBinding(HotkeySlot slot, uint32_t packed_binding) noexcept;
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

// ── Poller thread ──

// One poll of every slot with GetAsyncKeyState (high bit: the key is down now).
void Hotkeys_Poll() noexcept;
// The same with the key reader of the caller (tests).
using HotkeyKeyReader = bool (*)(int vk) noexcept;
void Hotkeys_PollWith(HotkeyKeyReader read_key) noexcept;

// ── Who polls ──

// The service thread (blackout.cpp) calls this with true once it exists and is polling, and with
// false after it has been joined. While true the OSD callback never polls.
void Hotkeys_SetServiceThreadPolling(bool polling) noexcept;
bool Hotkeys_ServiceThreadPolling() noexcept;
// The OSD callback's poll, on the presenting thread, for when the service thread could not be
// created (the per-frame behaviour from before the service thread): polls only while no service thread polls, so
// the two never both poll. The same edge detection and hand-over as the service thread's.
void Hotkeys_PollFallback() noexcept;
void Hotkeys_PollFallbackWith(HotkeyKeyReader read_key) noexcept;
