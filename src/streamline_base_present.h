#pragma once
#include <cstdint>

// One output binding below ReShade. The base is the DXGI receiver; the alias is the native
// swapchain ReShade reports. These are identities, not retained COM references.
struct StreamlineBaseBinding {
    uint64_t base = 0, alias = 0, proxy = 0, queue = 0, device = 0, revision = 0;
};
#ifdef _WIN64
#include <atomic>
extern std::atomic<uint64_t> g_streamline_base_publication;
StreamlineBaseBinding StreamlineBase_Read(uint64_t publication) noexcept;
inline StreamlineBaseBinding StreamlineBase_Current() noexcept {
    // Usual order / no Streamline: one relaxed load and a compare, no other state read.
    const auto publication = g_streamline_base_publication.load(std::memory_order_relaxed);
    return publication ? StreamlineBase_Read(publication) : StreamlineBaseBinding{};
}
inline uint64_t StreamlineBase_AliasOf(uint64_t object, const StreamlineBaseBinding& b = StreamlineBase_Current()) noexcept {
    return object == b.base ? b.alias : 0;
}
inline bool StreamlineBase_Carries(uint64_t native, const StreamlineBaseBinding& b = StreamlineBase_Current()) noexcept {
    return native && native == b.alias;
}
// Only the vtable lock's owner publishes (at the discovering event's finish) or retires.
void StreamlineBase_PublishLocked(const StreamlineBaseBinding& binding) noexcept;
// A bounded per-thread stack remembers each event's decision and pending ticket until its matching finish.
bool StreamlineBase_NoteGamePresent(uint64_t native, uint64_t device, uint64_t game_queue);
bool StreamlineBase_EventCarried(uint64_t native) noexcept;
bool StreamlineBase_NoteGamePresentDone(uint64_t native) noexcept;
void StreamlineBase_NoteProxyPresent(uint64_t proxy) noexcept;
void StreamlineBase_OnDestroyed(uint64_t native) noexcept;
#else
inline bool StreamlineBase_NoteGamePresent(uint64_t, uint64_t, uint64_t) noexcept { return false; }
inline bool StreamlineBase_EventCarried(uint64_t) noexcept { return false; }
inline bool StreamlineBase_NoteGamePresentDone(uint64_t) noexcept { return false; }
inline void StreamlineBase_OnDestroyed(uint64_t) noexcept {}
inline void StreamlineBase_NoteProxyPresent(uint64_t) noexcept {}
inline uint64_t StreamlineBase_AliasOf(uint64_t) noexcept { return 0; }
inline bool StreamlineBase_Carries(uint64_t) noexcept { return false; }
#endif
