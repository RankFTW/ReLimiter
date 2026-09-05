#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

struct ReflexReportAdmission {
    bool ingest = false;
    bool cadence_available = false;
};

inline int ReflexReport_SelectNextIndex(
    const uint64_t* first_frame_id,
    size_t frame_count,
    size_t frame_stride_bytes,
    uint64_t last_frame_id) noexcept
{
    if (first_frame_id == nullptr || frame_count == 0 ||
        frame_stride_bytes < sizeof(uint64_t) ||
        frame_stride_bytes % alignof(uint64_t) != 0) {
        return -1;
    }
    const size_t stride = frame_stride_bytes / sizeof(uint64_t);
    if (last_frame_id == 0) {
        for (size_t offset = 0; offset < frame_count; ++offset) {
            const size_t index = frame_count - 1 - offset;
            if (first_frame_id[index * stride] != 0) {
                return static_cast<int>(index);
            }
        }
        return -1;
    }
    for (size_t index = 0; index < frame_count; ++index) {
        if (first_frame_id[index * stride] > last_frame_id) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

inline ReflexReportAdmission ReflexReport_SelectAdmission(
    uint64_t frame_id,
    uint64_t last_frame_id,
    uint32_t gpu_frame_time_us,
    double workload_demand_us,
    uint32_t ai_frame_time_us) noexcept
{
    ReflexReportAdmission admission{};
    if (frame_id == 0 || frame_id <= last_frame_id) {
        return admission;
    }
    admission.cadence_available = gpu_frame_time_us != 0;
    admission.ingest = admission.cadence_available ||
        (std::isfinite(workload_demand_us) && workload_demand_us > 0.0) ||
        ai_frame_time_us != 0;
    return admission;
}