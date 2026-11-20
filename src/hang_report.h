#pragma once

#include "hang_progress.h"
#include "hang_report_policy.h"
#include "lock_owner_diag.h"
#include "logger.h"

#include <cstdint>

// The stall report: a watchdog thread that notices a stalled launch (no present event completed for a while, see
// hang_report_policy.h) and writes two files next to the log, relimiter_<exe>.hang.txt and relimiter_<exe>.hang.dmp
// (.hang.2.* and .hang.3.* for the second and third report of one process). Its only purpose is to say where a
// hung launch is stuck, on the user's machine, without a debugger. It changes no behaviour of the limiter. It is OFF by default (the hang_report key); while it is
// off nothing of it runs: no thread, no log-level floor, no trail marks, no progress counters, no lock-owner words and
// no clock read for them (hang_report_state.h).
//
// The thread is created with the descriptor of the hook installer thread (a DACL that refuses suspend and context
// access, hooks.h), so another module's thread freeze cannot suspend it. It wakes once a second and does nothing
// else; it reads the progress counters (hang_progress.h) and the published lock owners (lock_owner_diag.h).
//
// The text file is written FIRST, from static buffers with WriteFile: no heap allocation, no CRT stream, no logger, no
// loader call and no ReLimiter lock until it is closed, because the process may be stuck holding any of them. Only
// then are dbghelp loaded and the minidump written (either can block for good when the process is stuck on the loader
// lock or the heap), and the result line is appended to the text file.

struct HangReportOptions {
    bool enabled = false;                // the hang_report key (default off); false: nothing starts and no log floor is set
    bool dump_enabled = true;            // the hang_report_dump key; false: the text report only, dbghelp is never loaded
    uint32_t test_after_s = 0;           // hang_report_test_after_s
    const char* log_path = nullptr;      // the open log's path (relimiter_<exe>.log); siblings are derived from it
    const char* process_name = nullptr;  // for the report header and the fallback path
    void (*selection_lock_source)(LockOwnerSnapshot*) = nullptr;  // SwapMgr_SelectionLockSnapshot
    void* (*window_source)() = nullptr;  // the swap manager's window (SwapMgr_GetHWND as an HWND), for the header
    bool automatic_reports = true;       // false (tests): the watchdog only keeps its clock, it never decides to write
};

// Prepares the paths and the ntdll entry points, starts the watchdog and warns once when a report of an earlier
// launch sits next to the log. It also begins the logger's diagnostic floor (Log_BeginLevelFloor: at least info) that
// the watchdog ends 120 s after its start; the configured level is requested by the caller as usual and takes effect
// when the floor ends, unless it is more verbose. When the report files cannot be placed (next to the log or in the
// temporary folder) it warns once with the reason. False: nothing runs and no floor is set.
bool HangReport_Start(const HangReportOptions& options) noexcept;
// Stop event and a bounded join (5 s). False: the thread did not end (it is writing a report that does not finish); its
// handles and statics stay, the caller treats it like any worker that did not stop.
bool HangReport_Stop() noexcept;

struct HangReportResult {
    bool attempted = false;       // false: another report was being written, or the paths are not set
    bool text_ok = false;         // the text file was created, written, flushed to disk and closed without an error
    uint8_t text_stage = 0;       // when it was not: the step that failed (1 create, 2 write, 3 flush, 4 close)
    uint32_t text_error = 0;      // that step's GetLastError (0 for a short write)
    uint32_t text_short_written = 0;   // a short write: the bytes written ...
    uint32_t text_short_expected = 0;  // ... of the bytes asked (0: the failure was not a short write)
    uint64_t text_bytes = 0;
    uint32_t text_ms = 0;         // time of the text step
    uint32_t threads_listed = 0;  // threads other than the writer that were sampled (or listed as not openable)
    uint32_t threads_sampled = 0; // of them, those whose context was read
    bool dump_attempted = false;  // dbghelp was entered: false when the text step failed or hang_report_dump is off
    bool dump_ok = false;
    uint32_t dump_error = 0;      // GetLastError / HRESULT-like code of the dump step when it failed
    uint64_t dump_bytes = 0;
    uint32_t dump_ms = 0;
    bool result_line_ok = false;  // the "[dump]" result line was appended to the text file
};

// Writes report `index` (1..3) for `reason`: the text file, then the dump, then the result line and the warning. The
// watchdog's step; exposed for the tests, which call it from a thread of their own. One report at a time (a second
// concurrent call returns at once with attempted = false).
HangReportResult HangReport_Write(HangReason reason, uint32_t index, uint64_t stalled_ms) noexcept;

// The narrow path of report `index`'s text file / dump file ("" before HangReport_Start). For the log line and tests.
const char* HangReport_TextPath(uint32_t index) noexcept;
const char* HangReport_DumpPath(uint32_t index) noexcept;
// Test seam: the MiniDumpType flags of the dump (default thread info, unloaded modules, indirectly referenced memory,
// handle data).
void HangReport_SetDumpTypeForTest(uint32_t type) noexcept;
// Test seam: the seconds after the watchdog's start at which it ends the log's diagnostic info floor (default 120).
void HangReport_SetLogLevelDelayForTest(uint32_t seconds) noexcept;
// Test seam: the watchdog's own check of whether opening its thread for suspend is refused: 1 refused (protected),
// 0 opened (for example the debug privilege is enabled), 2 could not be determined (OpenThread failed with another
// error), -1 not checked yet. Meaningful after HangReport_Start.
int HangReport_EffectiveProtection() noexcept;
// The outcome of the watchdog's latest automatic report (not discarded: the text step failing there is as visible as
// in a direct call). HangReport_LastResult is valid only after a successful HangReport_Stop (the running watchdog may
// assign it again at any moment, a forced report can be followed by the ordinary one); while it runs it returns an
// empty result. HangReport_AutomaticReportCount, the number of automatic reports the watchdog has completed (result
// assigned, files written, logged) in this load, is valid at any time and is the completion signal to wait on.
HangReportResult HangReport_LastResult() noexcept;
uint32_t HangReport_AutomaticReportCount() noexcept;
// Test seam: forgets the process's report count and the last result, so that a test starts from a fresh policy. Only
// while the watchdog is stopped or HangReportOptions::automatic_reports is false.
void HangReport_ResetPolicyForTest() noexcept;
// Test seam: the thread-state parser on a caller's SystemProcessInformation snapshot (`length` bytes) for `pid`: the
// thread ids of that process into `thread_ids` (up to `capacity`, at most 1024) and their count. Returns the parser's
// verdict: 1 ok, 3 process not in the snapshot, 4 the snapshot does not match the layout.
uint32_t HangReport_ParseSystemInformationForTest(const void* buffer, uint32_t length, uint32_t pid, uint32_t* thread_ids,
                                                  uint32_t capacity, uint32_t* count) noexcept;
