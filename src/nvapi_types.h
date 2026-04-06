#pragma once

#include <Windows.h>
#include <Unknwn.h>
#include <cstdint>

// Minimal NvAPI type definitions for hooking.
// These match the NVAPI SDK structures we intercept.

typedef int NvAPI_Status;
constexpr NvAPI_Status NVAPI_OK = 0;

typedef uint32_t NvU32;

struct NV_SET_SLEEP_MODE_PARAMS {
    NvU32 version;
    uint8_t bLowLatencyMode;          // NvBool
    uint8_t bLowLatencyBoost;         // NvBool
    uint32_t minimumIntervalUs;
    uint8_t bUseMarkersToOptimize;    // NvBool
    uint8_t bUseMinQueueTime;         // NvBool
    uint8_t rsvd[30];
};

// NV_SLEEP_PARAMS_VER: sizeof | (1 << 16)
#define NV_SLEEP_PARAMS_VER (sizeof(NV_SET_SLEEP_MODE_PARAMS) | (1 << 16))

// Latency marker types matching NV_LATENCY_MARKER_TYPE
enum NV_LATENCY_MARKER_TYPE : uint32_t {
    SIMULATION_START       = 0,
    SIMULATION_END         = 1,
    RENDERSUBMIT_START     = 2,
    RENDERSUBMIT_END       = 3,
    PRESENT_START          = 4,
    PRESENT_END            = 5,
    // INPUT_SAMPLE etc. exist but we don't use them
};

struct NV_LATENCY_MARKER_PARAMS {
    NvU32 version;
    uint64_t frameID;
    NV_LATENCY_MARKER_TYPE markerType;
    uint64_t rsvd0;
    uint8_t rsvd[56];
};

// Function pointer types for NvAPI hooks
using PFN_NvAPI_D3D_SetSleepMode       = NvAPI_Status(__cdecl*)(IUnknown*, NV_SET_SLEEP_MODE_PARAMS*);
using PFN_NvAPI_D3D_Sleep              = NvAPI_Status(__cdecl*)(IUnknown*);
using PFN_NvAPI_D3D_SetLatencyMarker   = NvAPI_Status(__cdecl*)(IUnknown*, NV_LATENCY_MARKER_PARAMS*);
using PFN_NvAPI_QueryInterface         = void*(__cdecl*)(NvU32);
