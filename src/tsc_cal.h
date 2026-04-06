#pragma once

#include <cstdint>

// TSC ↔ QPC calibration via 32 paired RDTSC/QPC samples.
// Spec §6.3

struct TSCCal {
    double tsc_per_qpc = 0.0;
    uint64_t tsc0 = 0;
    int64_t qpc0 = 0;

    void Calibrate();
    uint64_t QPCToTSC(int64_t qpc) const;
};

extern TSCCal g_tsc_cal;
