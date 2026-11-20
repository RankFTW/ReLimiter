#pragma once
#include <cstdint>

// Diagnostic only: candidate source identity, never an enforcement ticket.
#ifdef _WIN64
void FgInput_BeginWorkerLifetime() noexcept;
void FgInput_RecordMarker(uint64_t frame, uint32_t producer, uint32_t controller,
                          int64_t release_qpc) noexcept;
void FgInput_InstallAndDrain() noexcept; // existing owned installer only
void FgInput_Drain() noexcept; // worker, or after worker + callback rundown
#else
inline void FgInput_BeginWorkerLifetime() noexcept {}
inline void FgInput_RecordMarker(uint64_t, uint32_t, uint32_t, int64_t) noexcept {}
inline void FgInput_InstallAndDrain() noexcept {}
inline void FgInput_Drain() noexcept {}
#endif