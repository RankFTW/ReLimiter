#include "hotkey_poller.h"

#include "hotkey_edge.h"

#include <Windows.h>

#include <array>
#include <atomic>

namespace {

constexpr size_t kSlotCount = static_cast<size_t>(HotkeySlot::Count);

struct Slot {
    // Handed over by the config owner.
    std::atomic<uint32_t> binding{0};
    std::atomic<bool> rearm{false};
    // Handed back to the config owner: 0 none, else (the transition generation the sample was
    // taken under) + 1.
    std::atomic<uint64_t> trigger{0};
    // The poller thread's alone.
    hotkey_edge::State state;
    uint32_t seen_binding = 0;
};

std::array<Slot, kSlotCount> s_slots;
std::atomic<bool> s_suppressed{false};
std::atomic<bool> s_suppress_latch{false};
// Advanced by every transition of what a sample may not straddle: a capture beginning or ending
// (Hotkeys_SetSuppressed changing value), a slot rearm, a binding change. A poll freezes it before
// it reads the suppression flag or a key; a trigger it publishes carries it and Hotkeys_Take
// rejects a trigger whose generation is no longer the current one, so a poll in flight across a
// transition (a capture that begins and ends while it is paused) publishes nothing that is applied.
std::atomic<uint64_t> s_generation{0};
// A service thread polls (blackout.cpp); false: the OSD callback polls (Hotkeys_PollFallback).
std::atomic<bool> s_service_polling{false};

bool AsyncKeyDown(int vk) noexcept {
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

Slot& SlotOf(HotkeySlot slot) noexcept {
    return s_slots[static_cast<size_t>(slot)];
}

}  // namespace

void Hotkeys_SetBinding(HotkeySlot slot, uint32_t packed_binding) noexcept {
    SlotOf(slot).binding.store(packed_binding, std::memory_order_release);
    s_generation.fetch_add(1, std::memory_order_acq_rel);
}

void Hotkeys_Rearm(HotkeySlot slot) noexcept {
    SlotOf(slot).rearm.store(true, std::memory_order_release);
    s_generation.fetch_add(1, std::memory_order_acq_rel);
}

void Hotkeys_SetSuppressed(bool suppressed) noexcept {
    // The latch first: a poll that reads the flag false after this call has already seen the latch.
    if (suppressed) s_suppress_latch.store(true, std::memory_order_release);
    // The flag before the generation: a poll that froze the new generation sees the new flag.
    if (s_suppressed.exchange(suppressed, std::memory_order_acq_rel) != suppressed)
        s_generation.fetch_add(1, std::memory_order_acq_rel);
}

bool Hotkeys_Take(HotkeySlot slot) noexcept {
    std::atomic<uint64_t>& trigger = SlotOf(slot).trigger;
    if (trigger.load(std::memory_order_relaxed) == 0) return false;
    const uint64_t sampled = trigger.exchange(0, std::memory_order_acq_rel);
    return sampled != 0 && sampled - 1 == s_generation.load(std::memory_order_acquire);
}

void Hotkeys_SetServiceThreadPolling(bool polling) noexcept {
    s_service_polling.store(polling, std::memory_order_release);
}

bool Hotkeys_ServiceThreadPolling() noexcept {
    return s_service_polling.load(std::memory_order_acquire);
}

void Hotkeys_PollWith(HotkeyKeyReader read_key) noexcept {
    // Frozen first: everything below is sampled under this generation.
    const uint64_t generation = s_generation.load(std::memory_order_acquire);
    bool suppressed = s_suppressed.load(std::memory_order_acquire);
    if (s_suppress_latch.load(std::memory_order_relaxed) &&
        s_suppress_latch.exchange(false, std::memory_order_acq_rel))
        suppressed = true;
    const auto read_modifiers = [read_key]() noexcept {
        hotkey_edge::Modifiers modifiers;
        modifiers.ctrl = read_key(VK_LCONTROL) || read_key(VK_RCONTROL);
        modifiers.alt = read_key(VK_LMENU) || read_key(VK_RMENU);
        modifiers.shift = read_key(VK_LSHIFT) || read_key(VK_RSHIFT);
        modifiers.super = read_key(VK_LWIN) || read_key(VK_RWIN);
        return modifiers;
    };
    for (Slot& slot : s_slots) {
        const uint32_t packed = slot.binding.load(std::memory_order_acquire);
        if (packed != slot.seen_binding) {
            slot.seen_binding = packed;
            hotkey_edge::Rearm(&slot.state);
        }
        if (slot.rearm.load(std::memory_order_relaxed) &&
            slot.rearm.exchange(false, std::memory_order_acq_rel))
            hotkey_edge::Rearm(&slot.state);
        const hotkey_edge::Binding binding = hotkey_edge::Unpack(packed);
        const bool triggered = hotkey_edge::Step(
            binding, &slot.state, suppressed,
            [read_key, vk = binding.vk]() noexcept { return read_key(vk); }, read_modifiers);
        if (triggered) slot.trigger.store(generation + 1, std::memory_order_release);
    }
}

void Hotkeys_Poll() noexcept {
    Hotkeys_PollWith(&AsyncKeyDown);
}

void Hotkeys_PollFallbackWith(HotkeyKeyReader read_key) noexcept {
    if (!s_service_polling.load(std::memory_order_acquire)) Hotkeys_PollWith(read_key);
}

void Hotkeys_PollFallback() noexcept {
    Hotkeys_PollFallbackWith(&AsyncKeyDown);
}
