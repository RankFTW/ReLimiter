#include "tsc_cal.h"
#include <Windows.h>
#include <intrin.h>

TSCCal g_tsc_cal;

void TSCCal::Calibrate() {
    struct S { uint64_t t; int64_t q; } s[32];
    for (int i = 0; i < 32; i++) {
        _mm_lfence();
        s[i].t = __rdtsc();
        LARGE_INTEGER li;
        QueryPerformanceCounter(&li);
        s[i].q = li.QuadPart;
        _mm_lfence();
    }
    tsc_per_qpc = static_cast<double>(s[31].t - s[0].t) /
                  static_cast<double>(s[31].q - s[0].q);
    tsc0 = s[0].t;
    qpc0 = s[0].q;
}

uint64_t TSCCal::QPCToTSC(int64_t qpc) const {
    return tsc0 + static_cast<uint64_t>((qpc - qpc0) * tsc_per_qpc);
}
