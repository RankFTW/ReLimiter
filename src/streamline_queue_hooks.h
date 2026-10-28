#pragma once
#include <Windows.h>
#include <cstdint>
struct StreamlineComEvidence;
#ifdef _WIN64
void StreamlineQueue_Offer(const StreamlineComEvidence&) noexcept;
void StreamlineQueue_SetFeatureModule(void* options, void* state) noexcept;
void StreamlineQueue_BlockFeature() noexcept;
void StreamlineQueue_Drain(bool final = false) noexcept;
// The GPU gate's own queue calls (Wait, timestamps, retirement Signal) made
// from a Present hook: inside this scope the queue hooks treat them as nested
// calls and never observe them, as they already do for the gate's calls made
// inside an observed Signal.
struct StreamlineQueueOwnedScope {
    StreamlineQueueOwnedScope() noexcept;
    ~StreamlineQueueOwnedScope();
    StreamlineQueueOwnedScope(const StreamlineQueueOwnedScope&) = delete;
    StreamlineQueueOwnedScope& operator=(const StreamlineQueueOwnedScope&) = delete;
};
#else
inline void StreamlineQueue_Offer(const StreamlineComEvidence&) noexcept {}
inline void StreamlineQueue_SetFeatureModule(void*, void*) noexcept {}
inline void StreamlineQueue_BlockFeature() noexcept {}
inline void StreamlineQueue_Drain(bool = false) noexcept {}
#endif