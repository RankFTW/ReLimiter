#pragma once

#include <Windows.h>
#include <Unknwn.h>
#include <cstdint>

// Minimal NvAPI type definitions for hooking.
// These match the NVAPI SDK structures we intercept.

typedef int NvAPI_Status;
constexpr NvAPI_Status NVAPI_OK = 0;
constexpr NvAPI_Status NVAPI_ERROR = -1;

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
    INPUT_SAMPLE           = 6,
    TRIGGER_FLASH          = 7,
    PC_LATENCY_PING        = 8,
};

struct NV_LATENCY_MARKER_PARAMS {
    NvU32 version;
    uint64_t frameID;
    NV_LATENCY_MARKER_TYPE markerType;
    uint64_t rsvd0;
    uint8_t rsvd[56];
};

// NvAPI_D3D12_SetAsyncFrameMarker parameters (nvapi.h
// NV_ASYNC_FRAME_MARKER_PARAMS_V1). markerType is an NV_LATENCY_MARKER_TYPE:
// 9..12 are OUT_OF_BAND_RENDERSUBMIT_START .. OUT_OF_BAND_PRESENT_END.
struct NV_ASYNC_FRAME_MARKER_PARAMS {
    NvU32 version;
    uint64_t frameID;
    uint32_t markerType;
    uint64_t presentFrameID;
    uint8_t vendorInternal;     // NvBool
    uint8_t rsvd[55];
};
static_assert(sizeof(NV_ASYNC_FRAME_MARKER_PARAMS) == 88, "layout of nvapi.h's V1 structure");

// NvAPI_D3D12_SetFlipConfig configuration (nvapi.h NV_FLIP_CONFIG_V1 / V2, both
// 24 bytes; the version word is sizeof | (n << 16)). V1 has reserved words where
// V2 has nPresentedFramesPerBatch and timePerBatchMs.
struct NV_FLIP_CONFIG {
    NvU32 version;
    NvU32 nFramesPerBatch;          // 0: flip metering off
    NvU32 flipConfigFeedbackMask;   // from the driver: bit 0 NO_IFLIP, bit 1 MIRRORING_DETECTED
    NvU32 nPresentedFramesPerBatch; // V2
    float timePerBatchMs;           // V2, <= 0 if unknown
    NvU32 reserved;
};
static_assert(sizeof(NV_FLIP_CONFIG) == 24, "layout of nvapi.h's V1 and V2 structures");

// NvAPI_GPU_GetArchInfo result (nvapi.h NV_GPU_ARCH_INFO_V2, 16 bytes; the version word is sizeof | (2 << 16)).
// architecture is an NV_GPU_ARCHITECTURE_ID: 0x190 Ada (AD100), 0x1B0 Blackwell (GB200); Tegra IDs are 0xE00000xx.
typedef void* NvPhysicalGpuHandle;
struct NV_GPU_ARCH_INFO {
    NvU32 version;
    NvU32 architecture;
    NvU32 implementation;
    NvU32 revision;
};
static_assert(sizeof(NV_GPU_ARCH_INFO) == 16, "layout of nvapi.h's V2 structure");
#define NV_GPU_ARCH_INFO_VER (sizeof(NV_GPU_ARCH_INFO) | (2 << 16))
constexpr NvU32 kNvApiMaxPhysicalGpus = 64; // nvapi.h NVAPI_MAX_PHYSICAL_GPUS: NvAPI_EnumPhysicalGPUs fills up to this many

// NvAPI_D3D_SetReflexSync parameters (nvapi.h NV_SET_REFLEX_SYNC_PARAMS_V1).
struct NV_SET_REFLEX_SYNC_PARAMS {
    NvU32 version;
    NvU32 bEnable : 1;
    NvU32 bDisable : 1;
    NvU32 flagsRsvd : 30;
    NvU32 vblankIntervalUs;         // 0: not available
    int32_t timeInQueueUs;          // can be negative; 0: not available
    NvU32 timeInQueueUsTarget;      // 0: not available
    uint8_t fgMultiplier;           // 0: the driver detects it
    uint8_t rsvd[27];
};
static_assert(sizeof(NV_SET_REFLEX_SYNC_PARAMS) == 48, "layout of nvapi.h's V1 structure");

// NvAPI_D3D_GetSleepStatus result (nvapi.h NV_GET_SLEEP_STATUS_PARAMS_V1).
struct NV_GET_SLEEP_STATUS_PARAMS {
    NvU32 version;
    uint8_t bLowLatencyMode;        // NvBool
    uint8_t bFsVrr;                 // NvBool
    uint8_t bCplVsyncOn;            // NvBool
    NvU32 sleepIntervalUs;
    uint8_t bUseGameSleep;          // NvBool
    uint8_t bFullscreenIFlip;       // NvBool
    uint8_t fgMultiplier;
    uint8_t rsvd[119];
};
static_assert(sizeof(NV_GET_SLEEP_STATUS_PARAMS) == 136, "layout of nvapi.h's V1 structure");
#define NV_GET_SLEEP_STATUS_PARAMS_VER (sizeof(NV_GET_SLEEP_STATUS_PARAMS) | (1 << 16))

// Function pointer types for NvAPI hooks
using PFN_NvAPI_D3D_SetSleepMode       = NvAPI_Status(__cdecl*)(IUnknown*, NV_SET_SLEEP_MODE_PARAMS*);
using PFN_NvAPI_D3D_Sleep              = NvAPI_Status(__cdecl*)(IUnknown*);
using PFN_NvAPI_D3D_SetLatencyMarker   = NvAPI_Status(__cdecl*)(IUnknown*, NV_LATENCY_MARKER_PARAMS*);
using PFN_NvAPI_D3D12_SetAsyncFrameMarker = NvAPI_Status(__cdecl*)(IUnknown*, NV_ASYNC_FRAME_MARKER_PARAMS*);
using PFN_NvAPI_D3D12_SetFlipConfig = NvAPI_Status(__cdecl*)(IUnknown*, UINT, NV_FLIP_CONFIG*);
using PFN_NvAPI_D3D_SetReflexSync = NvAPI_Status(__cdecl*)(IUnknown*, NV_SET_REFLEX_SYNC_PARAMS*);
using PFN_NvAPI_D3D_GetSleepStatus = NvAPI_Status(__cdecl*)(IUnknown*, NV_GET_SLEEP_STATUS_PARAMS*);
// The second argument is an NV_OUT_OF_BAND_CQ_TYPE (0 render, 1 present, 2 ignore, 3 render_present).
using PFN_NvAPI_D3D12_NotifyOutOfBandCommandQueue = NvAPI_Status(__cdecl*)(IUnknown*, int);
using PFN_NvAPI_D3D12_SetCreateCommandQueueLowLatencyHint = NvAPI_Status(__cdecl*)(IUnknown*);
using PFN_NvAPI_QueryInterface         = void*(__cdecl*)(NvU32);
using PFN_NvAPI_Initialize = NvAPI_Status(__cdecl*)();
using PFN_NvAPI_EnumPhysicalGPUs = NvAPI_Status(__cdecl*)(NvPhysicalGpuHandle*, NvU32*);
using PFN_NvAPI_GPU_GetArchInfo = NvAPI_Status(__cdecl*)(NvPhysicalGpuHandle, NV_GPU_ARCH_INFO*);
