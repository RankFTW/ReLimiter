#include "dlss_presets.h"
#include "logger.h"

// Include the real NVAPI header for DRS types (NVDRS_SETTING, etc.)
#include <nvapi.h>
#include <NvApiDriverSettings.h>

#include <Windows.h>
#include <cstring>
#include <cstdio>

// ── DLSS preset override reading via NvAPI DRS (Driver Settings) ──

// DRS setting IDs (from Profile Inspector / emoose's research)
static constexpr NvU32 DRS_ID_DLSS_SR_PRESET    = 0x10E41DF3;
static constexpr NvU32 DRS_ID_ENABLE_SR_OVERRIDE = 0x10E41DF8;
static constexpr NvU32 DRS_ID_ENABLE_RR_OVERRIDE = 0x10E41DF9;
static constexpr NvU32 DRS_ID_ENABLE_FG_OVERRIDE = 0x10E41DFA;
static constexpr NvU32 DRS_ID_DLSS_RR_PRESET    = 0x10E41DFB;

static bool s_resolved = false;
static bool s_available = false;
static DLSSPresets s_presets = {};
static DWORD s_last_poll_tick = 0;

static const char* PresetValueToLetter(NvU32 val) {
    static char buf[16];
    if (val == 0) return "-";
    if (val == 0x00FFFFFF) return "Latest";
    if (val >= 1 && val <= 26) {
        buf[0] = 'A' + static_cast<char>(val - 1);
        buf[1] = '\0';
        return buf;
    }
    snprintf(buf, sizeof(buf), "?(%u)", val);
    return buf;
}

static NvU32 ReadDRSSetting(NvDRSSessionHandle hSession, NvDRSProfileHandle hProfile, NvU32 settingId) {
    NVDRS_SETTING setting = {};
    setting.version = NVDRS_SETTING_VER;

    NvAPI_Status st = NvAPI_DRS_GetSetting(hSession, hProfile, settingId, &setting);
    if (st != NVAPI_OK) return 0;

    return setting.u32CurrentValue;
}

void DLSSPresets_Init() {
    if (s_resolved) return;
    s_resolved = true;

    // NvAPI_Initialize should already be called by HWMonitor or NvAPI hooks.
    // DRS functions are part of the standard NVAPI — no QueryInterface needed
    // when linking against nvapi64.lib. But we're not linking — we use runtime
    // resolution. However, the DRS functions ARE exported through the standard
    // NVAPI interface, so NvAPI_DRS_* calls should work directly if nvapi.h
    // is included and NvAPI_Initialize has been called.

    // Test if DRS works by trying to create a session
    NvDRSSessionHandle hSession = nullptr;
    NvAPI_Status st = NvAPI_DRS_CreateSession(&hSession);
    if (st == NVAPI_OK && hSession) {
        NvAPI_DRS_DestroySession(hSession);
        s_available = true;
        LOG_INFO("DLSS presets: DRS API available");
    } else {
        LOG_INFO("DLSS presets: DRS API not available (st=%d)", st);
    }
}

void DLSSPresets_Poll() {
    if (!s_resolved) DLSSPresets_Init();
    if (!s_available) return;

    DWORD now = GetTickCount();
    if (s_last_poll_tick != 0 && (now - s_last_poll_tick) < 5000) return;
    s_last_poll_tick = now;

    NvDRSSessionHandle hSession = nullptr;
    NvDRSProfileHandle hProfile = nullptr;

    __try {
        NvAPI_Status st = NvAPI_DRS_CreateSession(&hSession);
        if (st != NVAPI_OK || !hSession) return;

        st = NvAPI_DRS_LoadSettings(hSession);
        if (st != NVAPI_OK) { NvAPI_DRS_DestroySession(hSession); return; }

        // Try to find the game's specific profile
        NvDRSProfileHandle hGameProfile = nullptr;
        {
            wchar_t exePath[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, exePath, MAX_PATH);
            wchar_t* exeName = wcsrchr(exePath, L'\\');
            if (exeName) exeName++; else exeName = exePath;

            NVDRS_APPLICATION app = {};
            app.version = NVDRS_APPLICATION_VER;

            // NvAPI_UnicodeString is wchar_t[4096] — copy exe name into it
            NvAPI_UnicodeString appName = {};
            wcsncpy(reinterpret_cast<wchar_t*>(appName), exeName, NVAPI_UNICODE_STRING_MAX - 1);

            st = NvAPI_DRS_FindApplicationByName(hSession, appName, &hGameProfile, &app);
            if (st == NVAPI_OK && hGameProfile) {
                LOG_INFO("DLSS DRS: found game profile for %ls", exeName);
            } else {
                hGameProfile = nullptr;
            }
        }

        // Fall back to base profile
        if (!hGameProfile) {
            st = NvAPI_DRS_GetBaseProfile(hSession, &hProfile);
            if (st != NVAPI_OK || !hProfile) {
                NvAPI_DRS_DestroySession(hSession);
                return;
            }
        }

        NvDRSProfileHandle readProfile = hGameProfile ? hGameProfile : hProfile;

        // Read settings
        NvU32 sr_preset = ReadDRSSetting(hSession, readProfile, DRS_ID_DLSS_SR_PRESET);
        NvU32 rr_preset = ReadDRSSetting(hSession, readProfile, DRS_ID_DLSS_RR_PRESET);
        NvU32 sr_enable = ReadDRSSetting(hSession, readProfile, DRS_ID_ENABLE_SR_OVERRIDE);
        NvU32 rr_enable = ReadDRSSetting(hSession, readProfile, DRS_ID_ENABLE_RR_OVERRIDE);
        NvU32 fg_enable = ReadDRSSetting(hSession, readProfile, DRS_ID_ENABLE_FG_OVERRIDE);

        // Diagnostic scan on first poll
        static bool s_scanned = false;
        if (!s_scanned) {
            s_scanned = true;
            LOG_INFO("DLSS DRS: profile=%s SR=0x%X(en=%u) RR=0x%X(en=%u) FG_en=%u",
                     hGameProfile ? "game" : "base",
                     sr_preset, sr_enable, rr_preset, rr_enable, fg_enable);

            // Scan a range of IDs for any non-zero values
            for (NvU32 id = 0x10E41DF0; id <= 0x10E41E10; id++) {
                NvU32 val = ReadDRSSetting(hSession, readProfile, id);
                if (val != 0)
                    LOG_INFO("DLSS DRS: 0x%08X = %u (0x%X)", id, val, val);
            }
        }

        NvAPI_DRS_DestroySession(hSession);
        hSession = nullptr;

        // Populate presets — show if value is non-zero
        if (sr_preset > 0)
            strncpy(s_presets.sr, PresetValueToLetter(sr_preset), 3);
        else
            strncpy(s_presets.sr, "-", 3);

        if (rr_preset > 0)
            strncpy(s_presets.rr, PresetValueToLetter(rr_preset), 3);
        else
            strncpy(s_presets.rr, "-", 3);

        strncpy(s_presets.fg, fg_enable ? "On" : "-", 3);

        s_presets.available = true;

    } __except(EXCEPTION_EXECUTE_HANDLER) {
        if (hSession) NvAPI_DRS_DestroySession(hSession);
        static bool s_logged = false;
        if (!s_logged) { s_logged = true; LOG_WARN("DLSS presets: DRS exception"); }
        s_available = false;
    }
}

DLSSPresets DLSSPresets_Get() {
    return s_presets;
}
