#pragma once

#include <cstddef>
#include <cstdint>

// Evidence that survives a hang. A 4 KB file next to the log (relimiter_<exe>.crumb) is mapped for the life of the
// process and holds ONE record: the start-up stage the process last reached. Marks are plain stores into the
// mapped page (no lock, allocation, flush or log), so they are safe around a MinHook freeze, and the operating
// system keeps the dirty page when the process is killed or hangs. The next launch reads the record before it
// resets the file and, when the previous process never reached Running and never closed cleanly, logs ONE warning
// naming the last stage. Every failure (read-only folder, a second live launch of the same exe holding the file,
// a missing, short or torn record) silently disables the marks or the report; nothing here can fail or block start-up.

enum class StartupStage : uint8_t {
    None = 0,
    InitBegun,                   // DoInit entered
    InitEnded,                   // DoInit finished
    BatchApplyEntered,           // a hook batch is about to enable its hooks (site = HookBatchSite)
    BatchApplyLeft,              // that batch returned (site = HookBatchSite)
    FirstPresentEvent,           // the first present event reached the first-present block
    FirstNativePresentEntered,   // the first native Present entered
    FirstNativePresentReturned,  // the first native Present returned
    FirstGateWaitEnqueued,       // the first GPU present gate Wait was enqueued
    FirstGateSignal,             // the first GPU present gate Signal
    Running,                     // 32 presents completed on the selected swapchain
    Closed,                      // clean shutdown
};
inline constexpr uint8_t kStartupStageLast = static_cast<uint8_t>(StartupStage::Closed);

// The site byte of the batch stages: the HookBatchSite in the low 5 bits, and in the high bits what the hook
// installer thread (hooks.h) did for the apply. Ran: the freezing MinHook call of the apply ran on the installer
// thread. Protected: an installer thread exists and passed its protection self-check (a requester-run apply
// with this bit alone had the installer unavailable for that call, for example inside a loader callback).
// Unprotected: the installer thread's self-check failed, so every call runs on the requester. No bit: no
// installer thread (kill switch, not started, stopped).
inline constexpr uint8_t kBreadcrumbSiteMask = 0x1F;
inline constexpr uint8_t kBreadcrumbInstallerRan = 0x80;
inline constexpr uint8_t kBreadcrumbInstallerProtected = 0x40;
inline constexpr uint8_t kBreadcrumbInstallerUnprotected = 0x20;

// One call per mark: a relaxed load, a bit test and the record's stores. Every stage except BatchApplyEntered and
// BatchApplyLeft is first-wins (a later call for the same stage is ignored), so callers need no one-shot guard of
// their own. Once Running is stored every further mark except Closed is ignored (a hook batch applied later in
// the session is not start-up), and nothing follows Closed. A mark that finds another thread writing the record is
// dropped rather than waited for.
void Breadcrumb_Mark(StartupStage stage, uint8_t site = 0) noexcept;

// One call per completed present on the selected swapchain, after the first-present block. The 32nd stores Running
// (only when FirstPresentEvent was marked); later calls are one relaxed load.
void Breadcrumb_PresentCompleted() noexcept;

// ---- Launch-to-launch protocol (used by the logger) ----

// The on-disk record: native-endian, 40 bytes at offset 0 of the file; the rest of the page is zero. A writer makes
// seq_begin odd, stores the fields, stores seq_end (even), then seq_begin (even, equal to seq_end). A reader accepts
// the record only when seq_begin == seq_end and the value is even.
struct BreadcrumbRecord {
    uint32_t magic;           // kBreadcrumbMagic
    uint16_t version;         // kBreadcrumbVersion
    uint16_t size;            // sizeof(BreadcrumbRecord)
    uint32_t seq_begin;
    uint32_t process_id;
    uint64_t launch_filetime; // FILETIME at attach (100 ns since 1601, UTC)
    uint64_t stage_qpc;       // QueryPerformanceCounter at the stage
    uint8_t stage;            // StartupStage
    uint8_t site;             // site id of the stage (HookBatchSite in the low 5 bits for the batch stages, the installer flags above), 0 when none
    uint16_t reserved;
    uint32_t seq_end;
};
static_assert(sizeof(BreadcrumbRecord) == 40, "the crumb file layout is fixed");
inline constexpr uint32_t kBreadcrumbMagic = 0x42434C52u;  // "RLCB"
inline constexpr uint16_t kBreadcrumbVersion = 1;
inline constexpr size_t kBreadcrumbFileSize = 4096;

struct BreadcrumbVerdict {
    bool report = false;                      // the previous launch ended before Running without closing
    StartupStage stage = StartupStage::None;  // its last stage
    uint8_t site = 0;   // the site id without the installer flags
    uint8_t flags = 0;  // the installer flags of the site byte (kBreadcrumbInstaller*)
};

// Judges a record read from a previous launch: report only a valid record (magic, version, size, seq_begin ==
// seq_end and even, process id and launch time set, stage within the enum, a stage time for any stage after None)
// whose stage is neither Running nor Closed.
BreadcrumbVerdict Breadcrumb_Judge(const BreadcrumbRecord& record) noexcept;

// "the previous launch ended during start-up; last stage: <name>[ (site <name>)]". Returns the length written
// (truncated to cap, always terminated), 0 when verdict.report is false.
int Breadcrumb_Describe(const BreadcrumbVerdict& verdict, char* out, size_t cap) noexcept;

// Opens or creates the crumb file at `path` (exclusive writer: a second live launch holding it gets a sharing
// violation and neither marks nor reports), reads and judges the previous record, then resets the file for this
// launch and maps it for the life of the process. Returns the previous launch's verdict. Called once per logger
// start by Log_Init, after the new log is open; a second call in the same process (reload) keeps the mapping,
// judges the record in it and resets it. Any failure leaves the marks as no-ops.
BreadcrumbVerdict Breadcrumb_Attach(const char* path) noexcept;

// Releases the mapping and the file (the record stays on disk, as after a kill). Only for tests: the caller
// guarantees no concurrent Mark. Production keeps the mapping until the process ends, because late detours still
// call Mark after shutdown.
void Breadcrumb_Detach() noexcept;
