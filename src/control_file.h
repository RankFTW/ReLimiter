#pragma once
#include <Windows.h>
#include <cstddef>
#include <cstdint>

// Developer control file (developer key control_file, set at launch): lets a
// test harness drive ReLimiter without the OSD, for unattended captures and
// A/B runs within one game session.
//
// A poller thread reads relimiter_control.txt next to the INI every 100 ms.
// Each line is "<seq> <command>", seq a positive integer that only grows;
// only complete lines (ending in a newline) count, so write the file whole
// (a temporary file renamed over it). Commands with a seq above the last one
// queued run in the selected presenter's present callback, the thread that
// owns g_config, as the OSD's edits do:
//   set <key> <value>   one live INI key (Config_SetLiveKey); not saved by the
//                       command, but the addon saves its settings when the
//                       game exits, so restore the INI after a session
//   capture start|stop  CSV telemetry on or off; each start is a new capture
//                       epoch (relimiter_pacing_<process>_<pid>_<session>_capture_<epoch>.csv)
//   mark <text>         a log line
//   ping                no effect
// Commands already in the file when the thread starts belong to an earlier
// launch and are skipped (the status file's queued_seq says which).
//
// The poller rewrites relimiter_control_status.txt (key=value lines) after
// each result and at least twice a second: pid, QPC, presents seen, the last
// result (seq, ok, QPC on the present thread, message), capture state and
// epoch, target_fps and the resolved pacing pipeline (rung, reason,
// placement), frame generation and whether NvAPI markers flow.
bool ControlFile_Start(HMODULE module) noexcept; // no-op unless control_file is on
bool ControlFile_Stop() noexcept;                // false: the join timed out; retain the module
void ControlFile_OnPresent() noexcept;           // selected presenter's present callback

// Longest command text (after the seq) a line may carry.
inline constexpr size_t kControlFileCommandMax = 191;

// Parser (pure; exposed for tests). Calls on_command(seq, command, length)
// for each complete line whose seq is above every earlier one (and above
// after_seq), in file order; the command has its surrounding blanks removed.
// Lines without a leading seq, without a command or with a command longer
// than kControlFileCommandMax are skipped. on_command returns false to stop
// (the command was not taken, for example a full queue); the result is the
// highest seq taken (after_seq if none).
template <typename OnCommand>
uint64_t ControlFile_Parse(const char* text, size_t size, uint64_t after_seq, OnCommand&& on_command)
{
    const auto blank = [](char c) { return c == ' ' || c == '\t'; };
    uint64_t taken = after_seq;
    size_t start = 0;
    for (size_t i = 0; i < size; ++i) {
        if (text[i] != '\n') continue;
        const char* line = text + start;
        size_t length = i - start;
        start = i + 1;
        if (length && line[length - 1] == '\r') --length;
        size_t p = 0;
        while (p < length && blank(line[p])) ++p;
        uint64_t seq = 0;
        size_t digits = 0;
        while (p < length && line[p] >= '0' && line[p] <= '9') {
            if (++digits > 18) break;
            seq = seq * 10 + static_cast<uint64_t>(line[p] - '0');
            ++p;
        }
        if (digits == 0 || digits > 18 || seq == 0 || p >= length || !blank(line[p])) continue;
        while (p < length && blank(line[p])) ++p;
        size_t end = length;
        while (end > p && blank(line[end - 1])) --end;
        if (end == p || end - p > kControlFileCommandMax || seq <= taken) continue;
        if (!on_command(seq, line + p, end - p)) break;
        taken = seq;
    }
    return taken;
}
