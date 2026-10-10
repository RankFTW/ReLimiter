#pragma once
#include <Windows.h>
#include <cstdint>
struct StreamlineComEvidence;
#ifdef _WIN64
void StreamlineQueue_Offer(const StreamlineComEvidence&) noexcept;
void StreamlineQueue_SetFeatureModule(void* options, void* state) noexcept;
void StreamlineQueue_BlockFeature() noexcept;
void StreamlineQueue_Drain(bool final = false) noexcept;
#else
inline void StreamlineQueue_Offer(const StreamlineComEvidence&) noexcept {}
inline void StreamlineQueue_SetFeatureModule(void*, void*) noexcept {}
inline void StreamlineQueue_BlockFeature() noexcept {}
inline void StreamlineQueue_Drain(bool = false) noexcept {}
#endif