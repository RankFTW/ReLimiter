#include "dlss_presets.h"
#include "logger.h"
#include <nvapi.h>
#include <NvApiDriverSettings.h>
#include <Windows.h>
#include <cstring>
#include <cstdio>
#include <atomic>
#include <thread>

// DRS setting IDs
static constexpr NvU32 DRS_ID_DLSS_SR_PRESET    = 0x10E41DF3;
static constexpr NvU32 DRS_ID_DLSS_RR_PRESET    = 0x10E41DF7;
static constexpr NvU32 DRS_ID_DLSS_FG_PRESET    = 0x10E41DF1;
static constexpr NvU32 DRS_ID_ENABLE_SR_OVERRIDE = 0x10E41DF8;
static constexpr NvU32 DRS_ID_ENABLE_RR_OVERRIDE = 0x10E41DF9;
static constexpr NvU32 DRS_ID_ENABLE_FG_OVERRIDE = 0x10E41DFA;

static bool s_resolved = false;
static bool s_available = false;
static DLSSPresets s_presets = {};
static std::atomic<bool> s_thread_running{false};
static std::thread s_poll_thread;

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

static void DoPoll() {
    NvDRSSessionHandle hSession = nullptr;
    NvDRSProfileHandle hProfile = nullptr;

    __try {
        NvAPI_Status st = NvAPI_DRS_CreateSession(&hSession);
        if (st != NVAPI_OK || !hSession) return;

        st = NvAPI_DRS_LoadSettings(hSession);
        if (st != NVAPI_OK) { NvAPI_DRS_DestroySession(hSession); return; }

        // Find game profile
        NvDRSProfileHandle hGameProfile = nullptr;
        {
            wchar_t exePath[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, exePath, MAX_PATH);
            wchar_t* exeName = wcsrchr(exePath, L'\\');
            if (exeName) exeName++; else exeName = exePath;

            NVDRS_APPLICATION app = {};
            app.version = NVDRS_APPLICATION_VER;
            NvAPI_UnicodeString appName = {};
            wcsncpy(reinterpret_cast<wchar_t*>(appName), exeName, NVAPI_UNICODE_STRING_MAX - 1);

            st = NvAPI_DRS_FindApplicationByName(hSession, appName, &hGameProfile, &app);
            if (st != NVAPI_OK) hGameProfile = nullptr;
        }

        // Fall back to base profile
        if (!hGameProfile) {
            st = NvAPI_DRS_GetBaseProfile(hSession, &hProfile);
            if (st != NVAPI_OK || !hProfile) { NvAPI_DRS_DestroySession(hSession); return; }
        }

        NvDRSProfileHandle readProfile = hGameProfile ? hGameProfile : hProfile;

        NvU32 sr_preset = ReadDRSSetting(hSession, readProfile, DRS_ID_DLSS_SR_PRESET);
        NvU32 rr_preset = ReadDRSSetting(hSession, readProfile, DRS_ID_DLSS_RR_PRESET);
        NvU32 fg_preset = ReadDRSSetting(hSession, readProfile, DRS_ID_DLSS_FG_PRESET);

        NvAPI_DRS_DestroySession(hSession);
        hSession = nullptr;

        if (sr_preset > 0) strncpy(s_presets.sr, PresetValueToLetter(sr_preset), 3);
        else strncpy(s_presets.sr, "-", 3);

        if (rr_preset > 0) strncpy(s_presets.rr, PresetValueToLetter(rr_preset), 3);
        else strncpy(s_presets.rr, "-", 3);

        if (fg_preset > 0) strncpy(s_presets.fg, PresetValueToLetter(fg_preset), 3);
        else strncpy(s_presets.fg, "-", 3);

        s_presets.available = true;

        static bool s_logged = false;
        if (!s_logged) {
            s_logged = true;
            LOG_INFO("DLSS DRS: profile=%s SR=%s(0x%X) RR=%s(0x%X) FG=%s(0x%X)",
                     hGameProfile ? "game" : "base",
                     s_presets.sr, sr_preset, s_presets.rr, rr_preset, s_presets.fg, fg_preset);
        }

    } __except(EXCEPTION_EXECUTE_HANDLER) {
        if (hSession) NvAPI_DRS_DestroySession(hSession);
        static bool s_logged = false;
        if (!s_logged) { s_logged = true; LOG_WARN("DLSS presets: DRS exception"); }
        s_available = false;
    }
}

static void PollThreadProc() {
    // Initial poll
    DoPoll();

    // Re-poll every 10 seconds so mid-session NVIDIA App changes are picked up
    while (s_thread_running.load(std::memory_order_relaxed)) {
        for (int i = 0; i < 100 && s_thread_running.load(std::memory_order_relaxed); i++)
            Sleep(100);
        if (s_thread_running.load(std::memory_order_relaxed))
            DoPoll();
    }
}

void DLSSPresets_Init() {
    if (s_resolved) return;
    s_resolved = true;

    NvDRSSessionHandle hSession = nullptr;
    NvAPI_Status st = NvAPI_DRS_CreateSession(&hSession);
    if (st == NVAPI_OK && hSession) {
        NvAPI_DRS_DestroySession(hSession);
        s_available = true;
        LOG_INFO("DLSS presets: DRS API available");

        s_thread_running.store(true, std::memory_order_relaxed);
        s_poll_thread = std::thread(PollThreadProc);
    } else {
        LOG_INFO("DLSS presets: DRS API not available (st=%d)", st);
    }
}

void DLSSPresets_Poll() {
    if (!s_resolved) DLSSPresets_Init();
    // Data is updated by background thread — nothing to do here
}

DLSSPresets DLSSPresets_Get() {
    return s_presets;
}
