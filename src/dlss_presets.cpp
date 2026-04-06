#include "dlss_presets.h"
#include "nvapi_types.h"
#include "logger.h"
#include <Windows.h>
#include <cstring>
#include <atomic>

// NvAPI_NGX_GetNGXOverrideState ordinal
constexpr NvU32 NVAPI_ID_NGX_GetOverrideState = 0x3FD96FBA;

// NGX feature IDs
constexpr uint32_t NGX_FEATURE_SR = 0;  // Super Resolution (DLSS SR)
constexpr uint32_t NGX_FEATURE_RR = 1;  // Ray Reconstruction
constexpr uint32_t NGX_FEATURE_FG = 2;  // Frame Generation

// Custom struct matching NV_NGX_DLSS_OVERRIDE_GET_STATE_PARAMS layout
struct NV_NGX_OVERRIDE_GET_STATE_PARAMS {
    NvU32 version;
    uint32_t processIdentifier;
    uint32_t numFeatures;
    struct FeatureState {
        uint32_t featureId;
        uint32_t presetIndex;
        uint32_t feedbackMask;
        uint32_t reserved[4];
    } features[8];
    uint32_t reserved[8];
};
#define NV_NGX_OVERRIDE_GET_STATE_VER (sizeof(NV_NGX_OVERRIDE_GET_STATE_PARAMS) | (1 << 16))

using PFN_NGX_GetOverrideState = NvAPI_Status(__cdecl*)(NV_NGX_OVERRIDE_GET_STATE_PARAMS*);

static PFN_NGX_GetOverrideState s_GetOverrideState = nullptr;
static bool s_resolved = false;
static bool s_available = false;
static DLSSPresets s_presets = {};
static DWORD s_last_poll_tick = 0;

static const char* PresetToLetter(uint32_t index) {
    // 0=Default, 1=A, 2=B, ..., 13=M, etc.
    static char buf[4];
    if (index == 0) return "-";
    if (index >= 1 && index <= 26) {
        buf[0] = 'A' + static_cast<char>(index - 1);
        buf[1] = '\0';
        return buf;
    }
    return "?";
}

void DLSSPresets_Init() {
    if (s_resolved) return;
    s_resolved = true;

    HMODULE nvapi = GetModuleHandleW(L"nvapi64.dll");
    if (!nvapi) return;

    auto qi = reinterpret_cast<void*(*)(NvU32)>(
        GetProcAddress(nvapi, "nvapi_QueryInterface"));
    if (!qi) return;

    s_GetOverrideState = reinterpret_cast<PFN_NGX_GetOverrideState>(
        qi(NVAPI_ID_NGX_GetOverrideState));

    if (s_GetOverrideState)
        LOG_INFO("DLSS presets: NGX_GetOverrideState resolved");
    else
        LOG_INFO("DLSS presets: NGX_GetOverrideState not available");
}

void DLSSPresets_Poll() {
    if (!s_resolved) DLSSPresets_Init();
    if (!s_GetOverrideState) return;

    // Poll every 5 seconds
    DWORD now = GetTickCount();
    if (s_last_poll_tick != 0 && (now - s_last_poll_tick) < 5000) return;
    s_last_poll_tick = now;

    NV_NGX_OVERRIDE_GET_STATE_PARAMS params = {};
    params.version = NV_NGX_OVERRIDE_GET_STATE_VER;
    params.processIdentifier = GetCurrentProcessId();
    params.numFeatures = 3;
    params.features[0].featureId = NGX_FEATURE_SR;
    params.features[1].featureId = NGX_FEATURE_RR;
    params.features[2].featureId = NGX_FEATURE_FG;

    NvAPI_Status st;
    __try {
        st = s_GetOverrideState(&params);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        s_available = false;
        return;
    }

    if (st != NVAPI_OK) {
        s_available = false;
        return;
    }

    s_available = true;

    // Check feedbackMask bit 5 — if set, a preset override is active
    for (uint32_t i = 0; i < 3; i++) {
        bool override_active = (params.features[i].feedbackMask & (1 << 5)) != 0;
        const char* letter = override_active
            ? PresetToLetter(params.features[i].presetIndex)
            : "-";

        switch (params.features[i].featureId) {
            case NGX_FEATURE_SR: strncpy(s_presets.sr, letter, 3); break;
            case NGX_FEATURE_RR: strncpy(s_presets.rr, letter, 3); break;
            case NGX_FEATURE_FG: strncpy(s_presets.fg, letter, 3); break;
        }
    }
    s_presets.available = true;
}

DLSSPresets DLSSPresets_Get() {
    return s_presets;
}
