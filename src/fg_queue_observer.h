#pragma once
#include <Windows.h>
#include <array>
#include <cstdint>

// Internal diagnostic interfaces; these snapshots NEVER authorize queue waits.
struct FgInputTransaction {
    uint64_t transaction=0, frame=0, selected=0, generation=0;
    uint32_t producer=0, controller=0;
};
FgInputTransaction FgInput_CurrentTransaction() noexcept;
struct GpuPresentFgInput;
bool FgInput_TakeGpuHandoff(uint64_t caller,GpuPresentFgInput* input) noexcept;
bool FgInput_ReviewModule(HMODULE module, const std::array<BYTE,32>& digest,
    bool (*matches)(HMODULE,const void*) noexcept) noexcept;
void FgQueue_InstallAndDrain() noexcept;
void FgQueue_Drain() noexcept;
void FgQueue_BeginWorkerLifetime() noexcept;