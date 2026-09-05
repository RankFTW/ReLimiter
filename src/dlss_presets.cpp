#include "dlss_presets.h"
#include "runtime_lifecycle.h"
#include "logger.h"
#include <nvapi.h>
#include <NvApiDriverSettings.h>
#include <Windows.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>

// DRS setting IDs
static constexpr NvU32 DRS_ID_DLSS_SR_PRESET    = 0x10E41DF3;
static constexpr NvU32 DRS_ID_DLSS_RR_PRESET    = 0x10E41DF7;
static constexpr NvU32 DRS_ID_DLSS_FG_PRESET    = 0x10E41DF1;
static constexpr NvU32 DRS_ID_ENABLE_SR_OVERRIDE = 0x10E41DF8;
static constexpr NvU32 DRS_ID_ENABLE_RR_OVERRIDE = 0x10E41DF9;
static constexpr NvU32 DRS_ID_ENABLE_FG_OVERRIDE = 0x10E41DFA;
static constexpr NvU32 DRS_ID_MFG_GENERATION_FACTOR = 0x104D6667;
static constexpr NvU32 DRS_ID_MFG_MODE_OVERRIDE     = 0x10308298;
static constexpr NvU32 DRS_ID_MFG_DYNAMIC_TARGET_FPS = 0x10CF4125;

// Lifecycle operations are serialized independently from the worker's wait.
// Shutdown may therefore hold this lock while joining without blocking worker exit.
static std::mutex s_lifecycle_mutex;
static std::mutex s_worker_wait_mutex;
static std::condition_variable s_worker_wakeup;
static std::atomic<bool> s_stop_requested{false};
static bool s_initialized = false;
static HANDLE s_poll_thread = nullptr;

// All fields are atomic so the sequence protocol is valid under the C++ memory
// model. Writers are rare (poll/UI threads) and serialized; readers stay lock-free.
struct AtomicDLSSPresetsSnapshot {
    std::atomic<uint64_t> sequence{0};
    std::atomic<uint64_t> sr{0};
    std::atomic<uint64_t> rr{0};
    std::atomic<uint64_t> fg{0};
    std::atomic<bool> available{false};
    std::atomic<int> mfg_generation_factor{0};
    std::atomic<int> mfg_mode_override{0};
    std::atomic<int> mfg_dynamic_target_fps{0};
};

static AtomicDLSSPresetsSnapshot s_snapshot;
static std::mutex s_snapshot_writer_mutex;
static std::mutex s_drs_mutex;
static DLSSPresets s_snapshot_writer_copy = {};

static uint64_t EncodePresetText(const char text[8]) noexcept {
    uint64_t encoded = 0;
    memcpy(&encoded, text, sizeof(encoded));
    return encoded;
}

static void DecodePresetText(uint64_t encoded, char text[8]) noexcept {
    memcpy(text, &encoded, sizeof(encoded));
}

static void PublishSnapshotLocked(const DLSSPresets& presets) noexcept {
    s_snapshot.sequence.fetch_add(1, std::memory_order_acq_rel);
    s_snapshot.sr.store(EncodePresetText(presets.sr), std::memory_order_relaxed);
    s_snapshot.rr.store(EncodePresetText(presets.rr), std::memory_order_relaxed);
    s_snapshot.fg.store(EncodePresetText(presets.fg), std::memory_order_relaxed);
    s_snapshot.available.store(presets.available, std::memory_order_relaxed);
    s_snapshot.mfg_generation_factor.store(
        presets.mfg_generation_factor, std::memory_order_relaxed);
    s_snapshot.mfg_mode_override.store(
        presets.mfg_mode_override, std::memory_order_relaxed);
    s_snapshot.mfg_dynamic_target_fps.store(
        presets.mfg_dynamic_target_fps, std::memory_order_relaxed);
    s_snapshot.sequence.fetch_add(1, std::memory_order_release);
}

static void PublishSnapshot(const DLSSPresets& presets) {
    std::lock_guard<std::mutex> lock(s_snapshot_writer_mutex);
    s_snapshot_writer_copy = presets;
    PublishSnapshotLocked(s_snapshot_writer_copy);
}

static void PublishDynamicTargetFPS(int fps) {
    std::lock_guard<std::mutex> lock(s_snapshot_writer_mutex);
    s_snapshot_writer_copy.mfg_dynamic_target_fps = fps;
    PublishSnapshotLocked(s_snapshot_writer_copy);
}

static void WritePresetText(char text[8], NvU32 value) noexcept {
    if (value == 0) {
        strncpy(text, "-", 7);
    } else if (value == 0x00FFFFFF || value == 0x00FFFFFE || value == 0xFFFFFFFF) {
        strncpy(text, "Auto", 7);
    } else if (value >= 1 && value <= 26) {
        text[0] = 'A' + static_cast<char>(value - 1);
        text[1] = '\0';
    } else {
        snprintf(text, 8, "?(%u)", value);
    }
    text[7] = '\0';
}

static NvU32 ReadDRSSetting(
    NvDRSSessionHandle hSession,
    NvDRSProfileHandle hProfile,
    NvU32 settingId) {
    NVDRS_SETTING setting = {};
    setting.version = NVDRS_SETTING_VER;
    NvAPI_Status st = NvAPI_DRS_GetSetting(hSession, hProfile, settingId, &setting);
    if (st != NVAPI_OK) return 0;
    return setting.u32CurrentValue;
}

static void DoPollUnlocked() {
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
        NvU32 mfg_factor = ReadDRSSetting(hSession, readProfile, DRS_ID_MFG_GENERATION_FACTOR);
        NvU32 mfg_mode = ReadDRSSetting(hSession, readProfile, DRS_ID_MFG_MODE_OVERRIDE);
        NvU32 mfg_target_fps = ReadDRSSetting(hSession, readProfile, DRS_ID_MFG_DYNAMIC_TARGET_FPS);

        // If MFG not in game profile, also check base profile (some settings inherit)
        if (mfg_factor == 0 && hGameProfile) {
            NvDRSProfileHandle hBase = nullptr;
            if (NvAPI_DRS_GetBaseProfile(hSession, &hBase) == NVAPI_OK && hBase) {
                mfg_factor = ReadDRSSetting(hSession, hBase, DRS_ID_MFG_GENERATION_FACTOR);
                if (mfg_mode == 0)
                    mfg_mode = ReadDRSSetting(hSession, hBase, DRS_ID_MFG_MODE_OVERRIDE);
                if (mfg_target_fps == 0)
                    mfg_target_fps = ReadDRSSetting(hSession, hBase, DRS_ID_MFG_DYNAMIC_TARGET_FPS);
            }
        }

        NvAPI_DRS_DestroySession(hSession);
        hSession = nullptr;

        DLSSPresets next = {};
        WritePresetText(next.sr, sr_preset);
        WritePresetText(next.rr, rr_preset);
        WritePresetText(next.fg, fg_preset);
        next.mfg_generation_factor = static_cast<int>(mfg_factor);
        next.mfg_mode_override = static_cast<int>(mfg_mode);
        next.mfg_dynamic_target_fps = static_cast<int>(mfg_target_fps);
        next.available = true;
        PublishSnapshot(next);

        static bool s_logged = false;
        if (!s_logged) {
            s_logged = true;
            LOG_WARN("DLSS DRS: profile=%s SR=%s(0x%X) RR=%s(0x%X) FG=%s(0x%X) MFG_Factor=%u MFG_Mode=%u MFG_TargetFPS=0x%X",
                     hGameProfile ? "game" : "base",
                     next.sr, sr_preset, next.rr, rr_preset, next.fg, fg_preset,
                     mfg_factor, mfg_mode, mfg_target_fps);
        }

    } __except(EXCEPTION_EXECUTE_HANDLER) {
        if (hSession) NvAPI_DRS_DestroySession(hSession);
        static bool s_logged = false;
        if (!s_logged) { s_logged = true; LOG_WARN("DLSS presets: DRS exception"); }
    }
}

static void DoPoll() {
    std::lock_guard<std::mutex> drs_lock(s_drs_mutex);
    DoPollUnlocked();
}

static void PollThreadImpl() {
    if (!s_stop_requested.load(std::memory_order_acquire)) DoPoll();

    std::unique_lock<std::mutex> wait_lock(s_worker_wait_mutex);
    while (!s_stop_requested.load(std::memory_order_acquire)) {
        if (s_worker_wakeup.wait_for(
                wait_lock,
                std::chrono::seconds(10),
                [] { return s_stop_requested.load(std::memory_order_acquire); })) {
            break;
        }

        wait_lock.unlock();
        DoPoll();
        wait_lock.lock();
    }
}

static DWORD WINAPI PollThreadProc(LPVOID) noexcept {
    try {
        PollThreadImpl();
    }
    catch (...) {
        OutputDebugStringA("ReLimiter: DLSS preset worker rejected an exception.\n");
    }
    return 0;
}

static NvAPI_Status ProbeDrsAvailability() noexcept {
    NvDRSSessionHandle session = nullptr;
    __try {
        const NvAPI_Status status = NvAPI_DRS_CreateSession(&session);
        if (status == NVAPI_OK && session)
            NvAPI_DRS_DestroySession(session);
        return status;
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        return static_cast<NvAPI_Status>(-1);
    }
}

void DLSSPresets_Init() {
    if (RuntimeLifecycle_IsStopping()) return;
    {
        std::lock_guard<std::mutex> lifecycle_lock(s_lifecycle_mutex);
        if (s_initialized || RuntimeLifecycle_IsStopping()) return;
        // Reserve this initialization generation, then probe outside the
        // lifecycle mutex so shutdown can always publish its stop request.
        s_initialized = true;
    }

    NvAPI_Status st;
    {
        std::lock_guard<std::mutex> drs_lock(s_drs_mutex);
        st = ProbeDrsAvailability();
    }

    std::lock_guard<std::mutex> lifecycle_lock(s_lifecycle_mutex);
    if (!s_initialized || RuntimeLifecycle_IsStopping()) return;
    if (st != NVAPI_OK) {
        PublishSnapshot({});
        LOG_INFO("DLSS presets: DRS API not available (st=%d)", st);
        return;
    }

    LOG_INFO("DLSS presets: DRS API available");
    s_stop_requested.store(false, std::memory_order_release);
    s_poll_thread = CreateThread(
        nullptr, 0, PollThreadProc, nullptr, 0, nullptr);
    if (!s_poll_thread) {
        s_stop_requested.store(true, std::memory_order_release);
        PublishSnapshot({});
        LOG_ERROR("DLSS presets: failed to start poll worker (%lu)", GetLastError());
    }
}

void DLSSPresets_Poll() {
    DLSSPresets_Init();
}

bool DLSSPresets_Shutdown() {
    std::lock_guard<std::mutex> lifecycle_lock(s_lifecycle_mutex);
    s_stop_requested.store(true, std::memory_order_release);
    s_worker_wakeup.notify_all();
    if (s_poll_thread) {
        if (WaitForSingleObject(s_poll_thread, 2500) != WAIT_OBJECT_0) {
            OutputDebugStringA(
                "ReLimiter: DLSS preset worker did not stop; state retained.\n");
            return false;
        }
        CloseHandle(s_poll_thread);
        s_poll_thread = nullptr;
    }
    s_initialized = false;
    s_stop_requested.store(false, std::memory_order_release);
    PublishSnapshot({});
    return true;
}

DLSSPresets DLSSPresets_Get() {
    for (;;) {
        const uint64_t sequence_before =
            s_snapshot.sequence.load(std::memory_order_acquire);
        if ((sequence_before & 1u) != 0) continue;

        DLSSPresets presets = {};
        DecodePresetText(
            s_snapshot.sr.load(std::memory_order_relaxed), presets.sr);
        DecodePresetText(
            s_snapshot.rr.load(std::memory_order_relaxed), presets.rr);
        DecodePresetText(
            s_snapshot.fg.load(std::memory_order_relaxed), presets.fg);
        presets.available = s_snapshot.available.load(std::memory_order_relaxed);
        presets.mfg_generation_factor =
            s_snapshot.mfg_generation_factor.load(std::memory_order_relaxed);
        presets.mfg_mode_override =
            s_snapshot.mfg_mode_override.load(std::memory_order_relaxed);
        presets.mfg_dynamic_target_fps =
            s_snapshot.mfg_dynamic_target_fps.load(std::memory_order_relaxed);

        const uint64_t sequence_after =
            s_snapshot.sequence.load(std::memory_order_acquire);
        if (sequence_before == sequence_after) return presets;
    }
}

static bool WriteDynamicTargetFPSUnlocked(int fps) {
    NvDRSSessionHandle hSession = nullptr;

    __try {
        NvAPI_Status st = NvAPI_DRS_CreateSession(&hSession);
        if (st != NVAPI_OK || !hSession) return false;

        st = NvAPI_DRS_LoadSettings(hSession);
        if (st != NVAPI_OK) { NvAPI_DRS_DestroySession(hSession); return false; }

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
            if (st != NVAPI_OK || !hGameProfile) {
                NvAPI_DRS_DestroySession(hSession);
                LOG_WARN("DRS write: game profile not found");
                return false;
            }
        }

        // Write the setting
        NVDRS_SETTING setting = {};
        setting.version = NVDRS_SETTING_VER;
        setting.settingId = DRS_ID_MFG_DYNAMIC_TARGET_FPS;
        setting.settingType = NVDRS_DWORD_TYPE;
        setting.u32CurrentValue = static_cast<NvU32>(fps);

        st = NvAPI_DRS_SetSetting(hSession, hGameProfile, &setting);
        if (st != NVAPI_OK) {
            LOG_WARN("DRS write: SetSetting failed (st=%d)", st);
            NvAPI_DRS_DestroySession(hSession);
            return false;
        }

        st = NvAPI_DRS_SaveSettings(hSession);
        NvAPI_DRS_DestroySession(hSession);

        if (st != NVAPI_OK) {
            LOG_WARN("DRS write: SaveSettings failed (st=%d)", st);
            return false;
        }

        PublishDynamicTargetFPS(fps);
        LOG_INFO("DRS write: Dynamic Target FPS set to %d (0x%X)", fps, fps);
        return true;

    } __except(EXCEPTION_EXECUTE_HANDLER) {
        if (hSession) NvAPI_DRS_DestroySession(hSession);
        LOG_WARN("DRS write: SEH exception");
        return false;
    }
}

bool DLSSPresets_WriteDynamicTargetFPS(int fps) {
    if (fps < 0) return false;
    std::lock_guard<std::mutex> drs_lock(s_drs_mutex);
    return WriteDynamicTargetFPSUnlocked(fps);
}
