#pragma once

#include <cstdint>

// Hardware spin tiers detected at startup via CPUID.
// TPAUSE: CPUID.7.0.ECX bit 5 (Intel 12th gen+ / WAITPKG)
// MWAITX: CPUID.80000001.ECX bit 29 (AMD Zen+)
// Fallback: RDTSC spin (universal)

enum class SpinMethod { TPAUSE, MWAITX, RDTSC };

void DetectSpinMethod();
SpinMethod GetSpinMethod();

// QPC-based spin with TSC acceleration and QPC bailout.
// Dispatches to TPAUSE/MWAITX/RDTSC based on detected method.
void HWSpin(int64_t target_qpc);

// Name string for logging
const char* GetSpinMethodName();
