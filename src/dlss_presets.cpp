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
// Padded generously to accommodate newer driver versions that may have
// expanded the struct. The version field encodes sizeof, so we try
// multiple sizes.
struct NV_NGX_OVERRIDE_GET_STATE_PARAMS_V1 {
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

// Larger variant — newer drivers may have expanded FeatureState or added fields
struct NV_NGX_OVERRIDE_GET_STATE_PARAMS_V2 {
    NvU32 version;
    uint32_t processIdentifier;
    uint32_t numFeatures;
    struct FeatureState {
        uint32_t featureId;
        uint32_t presetIndex;
        uint32_t feedbackMask;
        uint32_t reserved[8];  // more reserved space per feature
    } features[8];
    uint32_t reserved[16];     // more reserved space at end
};

// Even larger — try doubling the feature reserved space
struct NV_NGX_OVERRIDE_GET_STATE_PARAMS_V3 {
    NvU32 version;
    uint32_t processIdentifier;
    uint32_t numFeatures;
    struct FeatureState {
        uint32_t featureId;
        uint32_t presetIndex;
        uint32_t feedbackMask;
        uint32_t reserved[12]; // generous padding
    } features[8];
    uint32_t reserved[32];     // generous padding
};

#define NV_NGX_OVERRIDE_VER(type, ver) (NvU32)(sizeof(type) | ((ver) << 16))

using PFN_NGX_GetOverrideState = NvAPI_Status(__cdecl*)(void*);

static PFN_NGX_GetOverrideState s_GetOverrideState = nullptr;
static bool s_resolved = false;
static bool s_available = false;
static DLSSPresets s_presets = {};
static DWORD s_last_poll_tick = 0;
static int s_working_variant = -1;  // which struct variant works (0=V1, 1=V2, 2=V3)

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

    // Try multiple struct variants to find one the driver accepts.
    // Each variant has a different sizeof which is encoded in the version field.
    // Once we find one that works, stick with it.

    uint32_t presetIndex[3] = {};
    uint32_t feedbackMask[3] = {};
    bool success = false;

    // Helper lambda to try a specific struct variant
    auto tryVariant = [&](int variant) -> bool {
        NvAPI_Status st;

        if (variant == 0) {
            NV_NGX_OVERRIDE_GET_STATE_PARAMS_V1 p = {};
            p.version = NV_NGX_OVERRIDE_VER(NV_NGX_OVERRIDE_GET_STATE_PARAMS_V1, 1);
            p.processIdentifier = GetCurrentProcessId();
            p.numFeatures = 3;
            p.features[0].featureId = NGX_FEATURE_SR;
            p.features[1].featureId = NGX_FEATURE_RR;
            p.features[2].featureId = NGX_FEATURE_FG;
            __try { st = s_GetOverrideState(&p); } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
            if (st != NVAPI_OK) return false;
            for (int i = 0; i < 3; i++) { presetIndex[i] = p.features[i].presetIndex; feedbackMask[i] = p.features[i].feedbackMask; }
            return true;
        } else if (variant == 1) {
            NV_NGX_OVERRIDE_GET_STATE_PARAMS_V2 p = {};
            p.version = NV_NGX_OVERRIDE_VER(NV_NGX_OVERRIDE_GET_STATE_PARAMS_V2, 1);
            p.processIdentifier = GetCurrentProcessId();
            p.numFeatures = 3;
            p.features[0].featureId = NGX_FEATURE_SR;
            p.features[1].featureId = NGX_FEATURE_RR;
            p.features[2].featureId = NGX_FEATURE_FG;
            __try { st = s_GetOverrideState(&p); } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
            if (st != NVAPI_OK) return false;
            for (int i = 0; i < 3; i++) { presetIndex[i] = p.features[i].presetIndex; feedbackMask[i] = p.features[i].feedbackMask; }
            return true;
        } else {
            NV_NGX_OVERRIDE_GET_STATE_PARAMS_V3 p = {};
            p.version = NV_NGX_OVERRIDE_VER(NV_NGX_OVERRIDE_GET_STATE_PARAMS_V3, 1);
            p.processIdentifier = GetCurrentProcessId();
            p.numFeatures = 3;
            p.features[0].featureId = NGX_FEATURE_SR;
            p.features[1].featureId = NGX_FEATURE_RR;
            p.features[2].featureId = NGX_FEATURE_FG;
            __try { st = s_GetOverrideState(&p); } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
            if (st != NVAPI_OK) return false;
            for (int i = 0; i < 3; i++) { presetIndex[i] = p.features[i].presetIndex; feedbackMask[i] = p.features[i].feedbackMask; }
            return true;
        }
    };

    if (s_working_variant >= 0) {
        success = tryVariant(s_working_variant);
    } else {
        // Try all variants, also try version 2 of each
        for (int v = 0; v < 3 && !success; v++) {
            success = tryVariant(v);
            if (success) {
                s_working_variant = v;
                LOG_INFO("DLSS presets: variant %d works (V1=%zu, V2=%zu, V3=%zu bytes)",
                         v, sizeof(NV_NGX_OVERRIDE_GET_STATE_PARAMS_V1),
                         sizeof(NV_NGX_OVERRIDE_GET_STATE_PARAMS_V2),
                         sizeof(NV_NGX_OVERRIDE_GET_STATE_PARAMS_V3));
            }
        }
        if (!success) {
            static bool s_fail_logged = false;
            if (!s_fail_logged) {
                s_fail_logged = true;
                LOG_INFO("DLSS presets: all struct variants failed (V1=%zu, V2=%zu, V3=%zu bytes)",
                         sizeof(NV_NGX_OVERRIDE_GET_STATE_PARAMS_V1),
                         sizeof(NV_NGX_OVERRIDE_GET_STATE_PARAMS_V2),
                         sizeof(NV_NGX_OVERRIDE_GET_STATE_PARAMS_V3));
            }
            s_available = false;
            return;
        }
    }

    s_available = true;

    // Decode results — feedbackMask bit 5 indicates override is active
    const uint32_t featureIds[3] = { NGX_FEATURE_SR, NGX_FEATURE_RR, NGX_FEATURE_FG };
    for (int i = 0; i < 3; i++) {
        bool override_active = (feedbackMask[i] & (1 << 5)) != 0;
        const char* letter = override_active ? PresetToLetter(presetIndex[i]) : "-";

        switch (featureIds[i]) {
            case NGX_FEATURE_SR: strncpy(s_presets.sr, letter, 3); break;
            case NGX_FEATURE_RR: strncpy(s_presets.rr, letter, 3); break;
            case NGX_FEATURE_FG: strncpy(s_presets.fg, letter, 3); break;
        }
    }
    s_presets.available = true;

    // Log first successful poll
    static bool s_first_log = true;
    if (s_first_log) {
        s_first_log = false;
        LOG_INFO("DLSS presets: SR=%s RR=%s FG=%s", s_presets.sr, s_presets.rr, s_presets.fg);
        for (int i = 0; i < 3; i++) {
            LOG_INFO("DLSS presets: feature[%d] presetIndex=%u feedbackMask=0x%X",
                     i, presetIndex[i], feedbackMask[i]);
        }
    }
}

DLSSPresets DLSSPresets_Get() {
    return s_presets;
}
