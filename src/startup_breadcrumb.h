#pragma once

#include "hang_report_state.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

// Evidence that survives a hang. A 4 KB file next to the log (relimiter_<exe>.crumb) is mapped for the life of the
// process and holds ONE record: the start-up stage the process last reached. Marks are plain stores into the
// mapped page (no lock, allocation, flush or log), so they are safe around a MinHook freeze, and the operating
// system keeps the dirty page when the process is killed or hangs. The next launch reads the record before it
// resets the file and, when the previous process never reached Running and never closed cleanly, logs ONE warning
// naming the last stage. Every failure (read-only folder, a second live launch of the same exe holding the file,
// a missing, short or torn record) silently disables the marks or the report; nothing here can fail or block start-up.
//
// The same page also holds a TRAIL: a ring of the last 96 marks (time, thread, kind, site) from offset 64, so a
// hang or a kill leaves where each thread was last seen, not only the single record. Every Breadcrumb_Mark appends
// to it (also after Running: the record's rule that nothing follows Running stays, the trail keeps recording) and
// so do the ring-only kinds below. Appending is one interlocked increment of the write index and plain stores into
// the slot; a torn slot (a writer overtaken after 96 more marks) is possible and harmless, one slot is never
// written through another's. The stall report (hang_report.h) prints it; it also stays in the crumb file.

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

// ---- The trail (see the top of this file) ----

// What a ring entry records. Stage entries are the Breadcrumb_Mark calls (site = the StartupStage, aux = the mark's
// site byte); the others are ring-only. Enter and Leave pairs bracket a call that can block: an Enter with no Leave
// on a thread is a thread inside it. Never reorder or renumber (the crumb file holds them).
enum class TrailKind : uint8_t {
    None = 0,
    Stage,                  // a Breadcrumb_Mark
    FreezeEnter,            // hooks.cpp RunFreezing: site = the operation, aux bit 0 = it goes to the installer thread
    FreezeLeave,            // same, extra = the MinHook status, aux bit 0 = it ran on the installer thread
    InitDeviceEnter, InitDeviceLeave,
    DestroyDeviceEnter, DestroyDeviceLeave,
    InitSwapchainEnter, InitSwapchainLeave,           // aux bit 0 = resize
    DestroySwapchainEnter, DestroySwapchainLeave,     // aux bit 0 = resize
    DestroyQueueEnter, DestroyQueueLeave,
    FirstReflexSleep,       // the first Reflex Sleep hook entry
    FirstLatencyMarker,     // the first latency marker hook entry
    FirstQueueExecute,      // the first call through each of the three queue detours (extra = the queue, low 32 bits)
    FirstQueueSignal,
    FirstQueueWait,
    QueueEnsureHooksEnter,  // StreamlineQueue_EnsureHooks
    QueueEnsureHooksLeave,  // site = the result (1 installed)
    QueueOfferEnter,        // StreamlineQueue_Offer past its early returns
    QueueOfferLeave,
    ProbeSessionStart,      // the GPU queue probe's session thread was created (extra = the queue, low 32 bits)
    ProbeSessionEnd,        // the session's end started
};
inline constexpr uint8_t kTrailKindLast = static_cast<uint8_t>(TrailKind::ProbeSessionEnd);

// The lifecycle callbacks' API byte (the site of the device and swapchain entries).
enum class TrailApi : uint8_t { Unknown = 0, D3D9, D3D10, D3D11, D3D12, OpenGL, Vulkan };

// One slot: 24 bytes. `qpc` is stored last (and cleared first), so a slot with qpc 0 is empty or being written.
// `extra` is a 32-bit detail (the low 32 bits of a native pointer tell two objects apart; a status).
struct TrailEntry {
    uint64_t qpc;
    uint32_t thread_id;
    uint32_t extra;
    uint8_t kind;   // TrailKind
    uint8_t site;
    uint16_t aux;
};
static_assert(sizeof(TrailEntry) == 24, "the trail layout is fixed");

// The ring header at offset 64 of the crumb page, the slots right behind it. write_index counts every append ever
// (slot = index % capacity).
struct TrailHeader {
    uint32_t magic;       // kTrailMagic
    uint16_t version;     // kTrailVersion
    uint16_t entry_size;  // sizeof(TrailEntry)
    uint32_t capacity;    // kTrailCapacity
    uint32_t write_index;
};
static_assert(sizeof(TrailHeader) == 16, "the trail layout is fixed");
inline constexpr uint32_t kTrailMagic = 0x52544C52u;  // "RLTR"
inline constexpr uint16_t kTrailVersion = 1;
inline constexpr size_t kTrailOffset = 64;
inline constexpr uint32_t kTrailCapacity = 96;
static_assert(kTrailOffset + sizeof(TrailHeader) + kTrailCapacity * sizeof(TrailEntry) <= 4096, "the trail fits the page");

// Appends one entry (no lock, allocation, flush or log; silent no-op without an attached crumb file and while the
// stall report is off: hang_report_state.h).
void Breadcrumb_Trail(TrailKind kind, uint8_t site = 0, uint32_t extra = 0, uint16_t aux = 0) noexcept;

// The first call through a hot site: `done` is a static flag of the site; once stored, a call is one relaxed load and a
// branch that is not taken. With the stall report off (hang_report_state.h) the trail does not exist: a call is one
// relaxed load of the report's flag and a return, and `done` is never stored.
inline void Breadcrumb_TrailFirst(std::atomic<bool>& done, TrailKind kind, uint32_t extra = 0) noexcept {
    if (!HangReport_Active()) return;
    if (done.load(std::memory_order_relaxed)) return;
    if (!done.exchange(true, std::memory_order_relaxed)) Breadcrumb_Trail(kind, 0, extra);
}

// The entries of the live mapping, oldest first, at most `capacity` (the newest ones when more were written).
// 0 without a mapping. Reads only the mapped page: no lock, allocation or log, safe from a watchdog.
size_t Breadcrumb_TrailSnapshot(TrailEntry* out, size_t capacity) noexcept;
// The same from a copy of the page (a crumb file read back, `page_size` at least kBreadcrumbFileSize): 0 when the
// header is not a trail's.
size_t Breadcrumb_TrailParse(const void* page, size_t page_size, TrailEntry* out, size_t capacity) noexcept;

// Names for the report (never nullptr).
const char* Breadcrumb_TrailKindName(uint8_t kind) noexcept;
const char* Breadcrumb_StageText(uint8_t stage) noexcept;
// The hook batch site name of a batch stage's site byte (the installer flags ignored), "" when none.
const char* Breadcrumb_BatchSiteText(uint8_t site) noexcept;
const char* Breadcrumb_TrailApiName(uint8_t api) noexcept;

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
static_assert(sizeof(BreadcrumbRecord) <= 64, "the record sits below the trail (kTrailOffset)");
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
