#include "control_file.h"
#include "bounded_mpsc_queue.h"
#include "capture_lifecycle.h"
#include "config.h"
#include "frame_timeline.h"
#include "health.h"
#include "logger.h"
#include "native_present_gate.h"
#include "nvapi_hooks.h"
#include "pacing_resolver.h"
#include "scheduler.h"
#include "streamline_hooks.h"
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwchar>

namespace {
constexpr DWORD kPollMs = 100;
constexpr size_t kFileMax = 16 * 1024; // a longer file's tail is cut: its last line has no newline

struct Command {
    uint64_t seq = 0;
    std::array<char, kControlFileCommandMax + 1> text{};
};
struct Result {
    uint64_t seq = 0;
    int64_t qpc = 0; // when the present callback ran the command
    bool ok = false;
    std::array<char, 160> message{};
};

// Poller -> present callback and back. The present callback only touches
// these, the flags and the counters, never a handle, so a callback still
// running during shutdown cannot use a closed one.
relimiter::BoundedMpscQueue<Command, 16> s_commands;
relimiter::BoundedMpscQueue<Result, 16> s_results;
std::atomic<bool> s_active{false};
std::atomic<bool> s_pending{false};
std::atomic<uint64_t> s_presents{0};
std::atomic<uint64_t> s_lost_results{0};

// Start and the poller thread only.
HANDLE s_thread = nullptr;
HANDLE s_stop = nullptr;
wchar_t s_command_path[MAX_PATH]{};
wchar_t s_status_path[MAX_PATH]{};
wchar_t s_status_temp[MAX_PATH]{};
uint64_t s_queued_seq = 0;
int64_t s_frequency = 0;
std::array<char, kFileMax> s_buffer{};

int64_t Qpc() noexcept
{
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

bool ReadCommandFile(size_t* read) noexcept
{
    const HANDLE file = CreateFileW(s_command_path, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD got = 0;
    const BOOL ok = ReadFile(file, s_buffer.data(), static_cast<DWORD>(s_buffer.size()), &got, nullptr);
    CloseHandle(file);
    *read = ok ? got : 0;
    return ok != FALSE;
}

// Splits off the next blank-separated word of *text into word (truncated to
// its size); false when the word does not fit.
bool NextWord(const char** text, char* word, size_t size) noexcept
{
    const char* p = *text;
    while (*p == ' ' || *p == '\t') ++p;
    size_t n = 0;
    while (*p && *p != ' ' && *p != '\t') {
        if (n + 1 >= size) return false;
        word[n++] = *p++;
    }
    word[n] = '\0';
    while (*p == ' ' || *p == '\t') ++p;
    *text = p;
    return true;
}

void Run(const Command& command, Result* result) noexcept
{
    char* message = result->message.data();
    const size_t message_size = result->message.size();
    const char* rest = command.text.data();
    char verb[16]{};
    if (!NextWord(&rest, verb, sizeof(verb))) {
        snprintf(message, message_size, "unknown command");
    } else if (strcmp(verb, "set") == 0) {
        char key[64]{};
        if (!NextWord(&rest, key, sizeof(key)) || !*key || !*rest) {
            snprintf(message, message_size, "usage: set <key> <value>");
        } else if (Config_SetLiveKey(key, rest, message, message_size)) {
            result->ok = true;
            snprintf(message, message_size, "%s=%s", key, rest);
        }
    } else if (strcmp(verb, "capture") == 0) {
        const FrameTimelineCaptureState before = FrameTimeline_CurrentCaptureState();
        if (strcmp(rest, "start") == 0) {
            // A start while capturing rotates to a new epoch, so each start is its own capture.
            const FrameTimelineCaptureState after = FrameTimeline_StartCapture();
            result->ok = after.enabled && after.epoch != before.epoch;
            g_config.csv_enabled = after.enabled; // as the OSD's Telemetry Logging toggle
            snprintf(message, message_size, result->ok ? "capture on epoch %u"
                : "capture start rejected (epoch %u; the CSV writer is behind)", after.epoch);
        } else if (strcmp(rest, "stop") == 0) {
            FrameTimeline_SetEnabled(false);
            const FrameTimelineCaptureState after = FrameTimeline_CurrentCaptureState();
            result->ok = !after.enabled;
            g_config.csv_enabled = after.enabled;
            snprintf(message, message_size, result->ok ? "capture off epoch %u" : "capture stop rejected (epoch %u)",
                after.epoch);
        } else {
            snprintf(message, message_size, "usage: capture start|stop");
        }
    } else if (strcmp(verb, "mark") == 0) {
        LOG_INFO("Control: mark %s", rest);
        result->ok = true;
        snprintf(message, message_size, "%s", rest);
    } else if (strcmp(verb, "ping") == 0 && !*rest) {
        result->ok = true;
        snprintf(message, message_size, "pong");
    } else {
        snprintf(message, message_size, "unknown command");
    }
    if (!result->ok) LOG_WARN("Control: seq %llu '%s' rejected: %s",
        static_cast<unsigned long long>(command.seq), command.text.data(), message);
    result->qpc = Qpc();
}

void WriteStatus(const Result* last, int64_t now) noexcept
{
    const PacingPipelineSnapshot pipeline = PacingPipeline_Read();
    const FrameTimelineCaptureState capture = FrameTimeline_CurrentCaptureState();
    char text[1536];
    const int length = snprintf(text, sizeof(text),
        "pid=%lu\nqpc=%lld\nqpc_frequency=%lld\npresents=%llu\nqueued_seq=%llu\nlost_results=%llu\n"
        "last_seq=%llu\nlast_ok=%d\nlast_qpc=%lld\nlast_message=%s\n"
        "capture=%d\ncapture_epoch=%u\ntarget_fps=%d\n"
        "mode=%s\nrung=%s\nauto_rung=%s\nreason=%s\nplacement=%s\npipeline_generation=%u\n"
        "fg_presenting=%d\nfg_multiplier=%d\nnvapi_markers=%d\ncross_thread_verified=%d\n"
        "unified_driver_pacing=%d\npacing_test_mode=%s\n",
        GetCurrentProcessId(), static_cast<long long>(now), static_cast<long long>(s_frequency),
        static_cast<unsigned long long>(s_presents.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(s_queued_seq),
        static_cast<unsigned long long>(s_lost_results.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(last ? last->seq : 0), last && last->ok ? 1 : 0,
        static_cast<long long>(last ? last->qpc : 0), last ? last->message.data() : "",
        capture.enabled ? 1 : 0, capture.epoch, g_user_target_fps.load(std::memory_order_relaxed),
        PacingTimingMode_Text(pipeline.mode), PacingRung_Text(pipeline.rung), PacingRung_Text(pipeline.auto_rung),
        PacingFallbackReason_Text(pipeline.reason), PacingSourcePlacement_Text(pipeline.placement),
        pipeline.generation, g_fg_presenting.load(std::memory_order_relaxed) ? 1 : 0,
        g_fg_multiplier.load(std::memory_order_relaxed), AreNvAPIMarkersFlowing() ? 1 : 0,
        NativeGate_CrossThreadVerified() ? 1 : 0,
        g_unified_driver_pacing.load(std::memory_order_relaxed) ? 1 : 0,
        PacingTestMode_Text(PacingPipeline_TestMode()));
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(text)) return;
    const HANDLE file = CreateFileW(s_status_temp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    const BOOL ok = WriteFile(file, text, static_cast<DWORD>(length), &written, nullptr);
    CloseHandle(file);
    // A reader sees the old or the new file, never a partial one.
    if (ok && written == static_cast<DWORD>(length))
        MoveFileExW(s_status_temp, s_status_path, MOVEFILE_REPLACE_EXISTING);
}

DWORD WINAPI Poller(void*)
{
    Result last{};
    bool have_last = false;
    int64_t last_status = 0;
    for (;;) {
        if (WaitForSingleObject(s_stop, kPollMs) == WAIT_OBJECT_0) break;
        size_t read = 0;
        if (ReadCommandFile(&read)) {
            bool queued = false;
            s_queued_seq = ControlFile_Parse(s_buffer.data(), read, s_queued_seq,
                [&](uint64_t seq, const char* text, size_t length) {
                    Command command;
                    command.seq = seq;
                    memcpy(command.text.data(), text, length);
                    command.text[length] = '\0';
                    if (!s_commands.Push(command)) return false; // full: retried next poll
                    queued = true;
                    return true;
                });
            if (queued) s_pending.store(true, std::memory_order_release);
        }
        bool changed = false;
        Result result;
        while (s_results.Pop(&result)) {
            last = result;
            have_last = changed = true;
        }
        const int64_t now = Qpc();
        if (changed || now - last_status >= s_frequency / 2) {
            WriteStatus(have_last ? &last : nullptr, now);
            last_status = now;
        }
    }
    return 0;
}
}

bool ControlFile_Start(HMODULE module) noexcept
{
    if (!g_config.control_file || s_thread) return true;
    wchar_t directory[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(module, directory, MAX_PATH);
    if (!length || length >= MAX_PATH) return false;
    wchar_t* slash = wcsrchr(directory, L'\\');
    if (!slash) return false;
    *slash = L'\0';
    const auto path = [&](wchar_t* out, const wchar_t* name) {
        const int written = swprintf(out, MAX_PATH, L"%ls\\%ls", directory, name);
        return written > 0 && written < MAX_PATH;
    };
    if (!path(s_command_path, L"relimiter_control.txt") || !path(s_status_path, L"relimiter_control_status.txt") ||
        !path(s_status_temp, L"relimiter_control_status.tmp"))
        return false;
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    s_frequency = frequency.QuadPart;
    // A retained module restarts here, after the previous lifetime's
    // callbacks and poller are gone: nothing it queued may run now.
    Command stale_command;
    while (s_commands.Pop(&stale_command)) {}
    Result stale_result;
    while (s_results.Pop(&stale_result)) {}
    s_pending.store(false, std::memory_order_relaxed);
    s_presents.store(0, std::memory_order_relaxed);
    s_lost_results.store(0, std::memory_order_relaxed);
    // Commands already in the file are an earlier launch's.
    s_queued_seq = 0;
    size_t read = 0;
    if (ReadCommandFile(&read))
        s_queued_seq = ControlFile_Parse(s_buffer.data(), read, 0,
            [](uint64_t, const char*, size_t) { return true; });
    const uint64_t stale_seq = s_queued_seq; // the poller owns s_queued_seq once it runs
    s_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!s_stop) return false;
    s_active.store(true, std::memory_order_release);
    s_thread = CreateThread(nullptr, 0, Poller, nullptr, 0, nullptr);
    if (!s_thread) {
        s_active.store(false, std::memory_order_release);
        CloseHandle(s_stop);
        s_stop = nullptr;
        return false;
    }
    LOG_INFO("Control file: polling %ls (commands up to seq %llu are from an earlier launch and skipped)",
        s_command_path, static_cast<unsigned long long>(stale_seq));
    return true;
}

bool ControlFile_Stop() noexcept
{
    s_active.store(false, std::memory_order_release);
    if (!s_thread) return true;
    SetEvent(s_stop);
    // On timeout keep the thread and its event: it may still be writing the status.
    if (WaitForSingleObject(s_thread, 2000) != WAIT_OBJECT_0) return false;
    CloseHandle(s_thread);
    s_thread = nullptr;
    CloseHandle(s_stop);
    s_stop = nullptr;
    return true;
}

void ControlFile_OnPresent() noexcept
{
    if (!s_active.load(std::memory_order_acquire)) return;
    s_presents.fetch_add(1, std::memory_order_relaxed);
    // Only while no OSD callback edits g_config (another runtime's thread):
    // otherwise the commands wait for a later present.
    if (!s_pending.load(std::memory_order_acquire) || !Config_TryLockOwner()) return;
    s_pending.store(false, std::memory_order_release);
    Command command;
    while (s_commands.Pop(&command)) {
        Result result;
        result.seq = command.seq;
        Run(command, &result);
        if (!s_results.Push(result)) s_lost_results.fetch_add(1, std::memory_order_relaxed);
    }
    Config_UnlockOwner();
}
