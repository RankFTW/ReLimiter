#include "hang_report.h"

#include "hooks.h"
#include "startup_breadcrumb.h"

#include <Windows.h>
#include <winternl.h>
#include <dbghelp.h>
#include <tlhelp32.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <cstddef>
#include <cstring>

#pragma comment(lib, "user32.lib")

// What the writer may call while other threads are stuck holding any lock (see hang_report.h). In the text step:
// system calls that need no lock of ours or of the loader (CreateFileW, WriteFile, FlushFileBuffers, OpenThread,
// SuspendThread, GetThreadContext, ResumeThread, ReadProcessMemory, the Nt* calls, QueryPerformanceCounter,
// GetSystemTime, and the window queries IsWindow/IsHungAppWindow/IsIconic/IsWindowVisible/GetForegroundWindow/
// GetAncestor, which send no message), static buffers, and hand-written formatting. No new, malloc, CRT stream,
// snprintf, logger, GetModuleHandle, GetProcAddress or LoadLibrary there: the ntdll entry points are resolved once at
// start. A thread is suspended only for its GetThreadContext and resumed before anything else is done with it.

namespace {

// ── Statics: the writer never allocates ──

constexpr size_t kBufferSize = 64 * 1024;
constexpr size_t kMaxModules = 512;
constexpr size_t kMaxExecSections = 6;
constexpr size_t kMaxThreadStates = 4096;
constexpr size_t kStackScanLimit = 1024 * 1024;   // bytes from the stack pointer
constexpr size_t kStackHits = 48;                 // return-address-like values kept per thread
constexpr size_t kTrailCapacityLocal = kTrailCapacity;
#ifdef _WIN64
constexpr size_t kSystemInfoSize = 4 * 1024 * 1024;
#else
constexpr size_t kSystemInfoSize = 2 * 1024 * 1024;
#endif
constexpr DWORD kDefaultDumpType = MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules |
                                   MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithHandleData;

struct ModuleRecord {
    uint64_t base;
    uint32_t size;
    uint32_t timestamp;
    char name[40];
    uint32_t exec_count;
    uint32_t exec_rva[kMaxExecSections];
    uint32_t exec_size[kMaxExecSections];
};

struct ThreadState {
    uint32_t tid;
    uint32_t state;
    uint32_t reason;
};

using NtGetNextThreadFn = LONG(NTAPI*)(HANDLE, HANDLE, ACCESS_MASK, ULONG, ULONG, PHANDLE);
using NtQueryInformationThreadFn = LONG(NTAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
using NtQuerySystemInformationFn = LONG(NTAPI*)(int, PVOID, ULONG, PULONG);
using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                           PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);

struct ThreadBasicInfo {  // THREAD_BASIC_INFORMATION
    LONG exit_status;
    PVOID teb;
    PVOID unique_process;
    PVOID unique_thread;
    ULONG_PTR affinity;
    LONG priority;
    LONG base_priority;
};

// Process-wide state, set by HangReport_Start (init thread, heap allowed) and read by the writer.
char s_text_path_a[kHangMaxReports][MAX_PATH];
char s_dump_path_a[kHangMaxReports][MAX_PATH];
wchar_t s_text_path_w[kHangMaxReports][MAX_PATH];
wchar_t s_dump_path_w[kHangMaxReports][MAX_PATH];
wchar_t s_dbghelp_path_w[MAX_PATH];
char s_process_name[64];
bool s_paths_ready = false;
std::atomic<bool> s_started{false};

NtGetNextThreadFn s_nt_get_next_thread = nullptr;
NtQueryInformationThreadFn s_nt_query_thread = nullptr;
NtQuerySystemInformationFn s_nt_query_system = nullptr;
void (*s_selection_source)(LockOwnerSnapshot*) = nullptr;
DWORD s_dump_type = kDefaultDumpType;

HANDLE s_stop_event = nullptr;
HANDLE s_thread = nullptr;
std::atomic<uint32_t> s_watchdog_thread_id{0};
bool s_thread_protected = false;
DWORD s_security_error = 0;
HangPolicyConfig s_policy_config;
HangPolicyState s_policy_state;  // the process's: a reload keeps the count of reports
uint32_t s_floor_delay_s = 120;   // the log's diagnostic floor ends this long after the watchdog's start
std::atomic<bool> s_floor_pending{false};
void* (*s_window_source)() = nullptr;
bool s_dump_enabled = true;
bool s_automatic = true;
std::atomic<int> s_effective_protection{-1};   // the watchdog thread's own check: 1 refused, 0 opened, 2 undetermined, -1 not yet
std::atomic<uint32_t> s_protection_error{0};   // the error of an undetermined check
// The process's CPU time over the watchdog's last wake interval (kept each wake, read by the report).
std::atomic<uint64_t> s_cpu_previous_total{0};   // kernel + user, 100 ns units
std::atomic<uint64_t> s_cpu_previous_qpc{0};
std::atomic<uint64_t> s_cpu_delta{0};
std::atomic<uint64_t> s_cpu_wall{0};             // that interval in QPC ticks; 0 = not measured yet
HangReportResult s_last_result;                  // the watchdog's last automatic report
std::atomic<uint32_t> s_last_result_count{0};

std::atomic<bool> s_writing{false};
uint64_t s_frequency = 0;

// Writer buffers.
alignas(8) char s_buffer[kBufferSize];
size_t s_used = 0;
HANDLE s_file = INVALID_HANDLE_VALUE;
bool s_write_failed = false;
DWORD s_write_error = 0;
uint32_t s_short_written = 0;     // a short write of the text file: bytes written of s_short_expected asked
uint32_t s_short_expected = 0;
uint64_t s_written_total = 0;

ModuleRecord s_modules[kMaxModules];
uint16_t s_order[kMaxModules];  // indexes of s_modules sorted by base
size_t s_module_count = 0;
uint64_t s_module_low = 0, s_module_high = 0;

alignas(16) uint8_t s_system_info[kSystemInfoSize];
ThreadState s_thread_states[kMaxThreadStates];
size_t s_thread_state_count = 0;
enum class SystemInfoState : uint8_t { Unavailable, Ok, BufferTooSmall, NoProcess, Malformed, CallFailed };
SystemInfoState s_system_info_state = SystemInfoState::Unavailable;
LONG s_system_info_status = 0;
ULONG s_system_info_needed = 0;

alignas(16) CONTEXT s_context;
TrailEntry s_trail[kTrailCapacityLocal];

// ── Formatting (no CRT) ──

void Flush() noexcept {
    if (s_used == 0) return;
    if (s_file != INVALID_HANDLE_VALUE && !s_write_failed) {
        DWORD written = 0;
        if (!WriteFile(s_file, s_buffer, static_cast<DWORD>(s_used), &written, nullptr)) {
            s_write_failed = true;
            s_write_error = GetLastError();
        } else if (written != s_used) {
            // The call succeeded: GetLastError means nothing here; the byte counts are the diagnosis.
            s_write_failed = true;
            s_write_error = 0;
            s_short_written = static_cast<uint32_t>(written);
            s_short_expected = static_cast<uint32_t>(s_used);
        } else {
            s_written_total += written;
        }
    }
    s_used = 0;
}

void Put(const char* text, size_t length) noexcept {
    while (length != 0) {
        size_t room = kBufferSize - s_used;
        if (room == 0) {
            Flush();
            room = kBufferSize;
        }
        const size_t chunk = length < room ? length : room;
        std::memcpy(s_buffer + s_used, text, chunk);
        s_used += chunk;
        text += chunk;
        length -= chunk;
    }
}

void PutS(const char* text) noexcept {
    size_t length = 0;
    while (text[length]) ++length;
    Put(text, length);
}

void PutNl() noexcept { Put("\r\n", 2); }

void PutU(uint64_t value) noexcept {
    char digits[24];
    size_t n = 0;
    do {
        digits[n++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value);
    char out[24];
    for (size_t i = 0; i < n; ++i) out[i] = digits[n - 1 - i];
    Put(out, n);
}

void PutI(int64_t value) noexcept {
    if (value < 0) {
        Put("-", 1);
        PutU(static_cast<uint64_t>(-(value + 1)) + 1u);
    } else {
        PutU(static_cast<uint64_t>(value));
    }
}

void PutHex(uint64_t value, unsigned min_digits = 1) noexcept {
    static const char kDigits[] = "0123456789abcdef";
    char out[16];
    size_t n = 0;
    do {
        out[n++] = kDigits[value & 0xF];
        value >>= 4;
    } while (value || n < min_digits);
    char text[16];
    for (size_t i = 0; i < n; ++i) text[i] = out[n - 1 - i];
    Put(text, n);
}

void PutX(uint64_t value, unsigned min_digits = 1) noexcept {
    Put("0x", 2);
    PutHex(value, min_digits);
}

void PutPad(uint64_t value, unsigned width) noexcept {
    char text[24];
    size_t n = 0;
    do {
        text[n++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value);
    while (n < width) text[n++] = '0';
    char out[24];
    for (size_t i = 0; i < n; ++i) out[i] = text[n - 1 - i];
    Put(out, n);
}

void PutBool(bool value) noexcept { PutS(value ? "yes" : "no"); }

int64_t TicksToMs(int64_t ticks) noexcept {
    if (s_frequency == 0) return 0;
    return ticks / static_cast<int64_t>(s_frequency / 1000 ? s_frequency / 1000 : 1);
}

// "<age> ms ago" of a QPC stamp, or "never".
void PutAge(uint64_t stamp, uint64_t now) noexcept {
    if (stamp == 0) {
        PutS("never");
        return;
    }
    PutI(TicksToMs(static_cast<int64_t>(now) - static_cast<int64_t>(stamp)));
    PutS(" ms ago");
}

uint64_t NowQpc() noexcept {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<uint64_t>(now.QuadPart);
}

// ── Modules: the loader's own list, read without the loader lock ──

// SizeOfImage and the executable sections of the image at `base`; false when the headers are not readable.
bool ReadImageLayout(uint64_t base, ModuleRecord* record) noexcept {
    __try {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 4096) return false;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + static_cast<uint64_t>(dos->e_lfanew));
        if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
        record->size = nt->OptionalHeader.SizeOfImage;
        const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
        const unsigned count = nt->FileHeader.NumberOfSections < 96 ? nt->FileHeader.NumberOfSections : 96;
        for (unsigned i = 0; i < count && record->exec_count < kMaxExecSections; ++i) {
            if (!(section[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
            const uint32_t length = section[i].Misc.VirtualSize > section[i].SizeOfRawData
                                        ? section[i].Misc.VirtualSize : section[i].SizeOfRawData;
            record->exec_rva[record->exec_count] = section[i].VirtualAddress;
            record->exec_size[record->exec_count] = length;
            ++record->exec_count;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void CollectModules() noexcept {
    s_module_count = 0;
    s_module_low = ~0ull;
    s_module_high = 0;
    __try {
        const PEB* peb = NtCurrentTeb()->ProcessEnvironmentBlock;
        const PEB_LDR_DATA* ldr = peb ? peb->Ldr : nullptr;
        if (!ldr) return;
        const LIST_ENTRY* head = &ldr->InMemoryOrderModuleList;
        const LIST_ENTRY* link = head->Flink;
        for (unsigned guard = 0; link && link != head && s_module_count < kMaxModules && guard < 4096; ++guard) {
            const auto* entry = CONTAINING_RECORD(link, LDR_DATA_TABLE_ENTRY, InMemoryOrderLinks);
            link = link->Flink;
            if (!entry->DllBase) continue;
            ModuleRecord& record = s_modules[s_module_count];
            std::memset(&record, 0, sizeof(record));
            record.base = reinterpret_cast<uint64_t>(entry->DllBase);
            record.timestamp = entry->TimeDateStamp;
            const wchar_t* path = entry->FullDllName.Buffer;
            const size_t length = entry->FullDllName.Length / sizeof(wchar_t);
            size_t start = 0;
            for (size_t i = 0; i < length; ++i)
                if (path[i] == L'\\' || path[i] == L'/') start = i + 1;
            size_t n = 0;
            for (size_t i = start; i < length && n + 1 < sizeof(record.name); ++i)
                record.name[n++] = path[i] < 0x80 && path[i] >= 0x20 ? static_cast<char>(path[i]) : '?';
            if (!ReadImageLayout(record.base, &record)) record.size = 0;
            ++s_module_count;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    // Sorted by base (insertion sort on indexes: at most 512 entries, no allocation).
    for (size_t i = 0; i < s_module_count; ++i) {
        size_t j = i;
        while (j > 0 && s_modules[s_order[j - 1]].base > s_modules[i].base) {
            s_order[j] = s_order[j - 1];
            --j;
        }
        s_order[j] = static_cast<uint16_t>(i);
        if (s_modules[i].size != 0) {
            if (s_modules[i].base < s_module_low) s_module_low = s_modules[i].base;
            if (s_modules[i].base + s_modules[i].size > s_module_high) s_module_high = s_modules[i].base + s_modules[i].size;
        }
    }
}

// The module containing `address` (by the table), or -1.
int FindModule(uint64_t address) noexcept {
    if (s_module_count == 0 || address < s_module_low || address >= s_module_high) return -1;
    size_t low = 0, high = s_module_count;
    while (low < high) {
        const size_t mid = (low + high) / 2;
        if (s_modules[s_order[mid]].base <= address) low = mid + 1;
        else high = mid;
    }
    if (low == 0) return -1;
    const int index = s_order[low - 1];
    const ModuleRecord& record = s_modules[index];
    return address < record.base + record.size ? index : -1;
}

// A value that lies inside an executable section of a loaded module.
bool IsCodeAddress(uint64_t address, int* module_index) noexcept {
    const int index = FindModule(address);
    if (index < 0) return false;
    const ModuleRecord& record = s_modules[index];
    const uint64_t rva = address - record.base;
    for (uint32_t i = 0; i < record.exec_count; ++i) {
        if (rva >= record.exec_rva[i] && rva < static_cast<uint64_t>(record.exec_rva[i]) + record.exec_size[i]) {
            *module_index = index;
            return true;
        }
    }
    return false;
}

void PutModuleOffset(uint64_t address) noexcept {
    const int index = FindModule(address);
    if (index < 0) {
        PutX(address);
        return;
    }
    PutS(s_modules[index].name);
    PutS("+");
    PutX(address - s_modules[index].base);
}

// ── Kernel thread state (one NtQuerySystemInformation call, a static buffer) ──

const char* ThreadStateName(uint32_t state) noexcept {
    static const char* const kNames[] = {"Initialized", "Ready", "Running", "Standby", "Terminated", "Waiting",
                                         "Transition", "DeferredReady", "GateWaitObsolete", "WaitingForProcessInSwap"};
    return state < sizeof(kNames) / sizeof(kNames[0]) ? kNames[state] : "?";
}

const char* WaitReasonName(uint32_t reason) noexcept {
    static const char* const kNames[] = {
        "Executive", "FreePage", "PageIn", "PoolAllocation", "DelayExecution", "Suspended", "UserRequest",
        "WrExecutive", "WrFreePage", "WrPageIn", "WrPoolAllocation", "WrDelayExecution", "WrSuspended",
        "WrUserRequest", "WrEventPair", "WrQueue", "WrLpcReceive", "WrLpcReply", "WrVirtualMemory", "WrPageOut",
        "WrRendezvous", "WrKeyedEvent", "WrTerminated", "WrProcessInSwap", "WrCpuRateControl", "WrCalloutStack",
        "WrKernel", "WrResource", "WrPushLock", "WrMutex", "WrQuantumEnd", "WrDispatchInt", "WrPreempted",
        "WrYieldExecution", "WrFastMutex", "WrGuardedMutex", "WrRundown", "WrAlertByThreadId", "WrDeferredPreempt"};
    return reason < sizeof(kNames) / sizeof(kNames[0]) ? kNames[reason] : "?";
}

// ── Kernel thread states: SystemProcessInformation, parsed by its layout ──
//
// The records are the ones every Windows 10/11 build returns (ntddk's SYSTEM_PROCESS_INFORMATION with its
// SYSTEM_THREAD_INFORMATION array behind the fixed part). The compiler lays them out for the architecture of this
// module (a 32-bit add-on under WOW64 gets the 32-bit records), and the offsets the layout is known by are asserted.
// Every offset read from the buffer is bounds-checked against the returned length; a snapshot that does not fit, or
// whose process entry or first ClientId does not match, is reported as such instead of guessed at.

struct SysUnicodeString {
    USHORT length;
    USHORT maximum_length;
    PWSTR buffer;
};

struct SysThreadInfo {   // SYSTEM_THREAD_INFORMATION
    LARGE_INTEGER kernel_time;
    LARGE_INTEGER user_time;
    LARGE_INTEGER create_time;
    ULONG wait_time;
    PVOID start_address;
    PVOID client_process;   // CLIENT_ID.UniqueProcess
    PVOID client_thread;    // CLIENT_ID.UniqueThread
    LONG priority;
    LONG base_priority;
    ULONG context_switches;
    ULONG thread_state;
    ULONG wait_reason;
};

struct SysProcessInfo {   // SYSTEM_PROCESS_INFORMATION
    ULONG next_entry_offset;
    ULONG number_of_threads;
    LARGE_INTEGER working_set_private_size;
    ULONG hard_fault_count;
    ULONG number_of_threads_high_watermark;
    ULONGLONG cycle_time;
    LARGE_INTEGER create_time;
    LARGE_INTEGER user_time;
    LARGE_INTEGER kernel_time;
    SysUnicodeString image_name;
    LONG base_priority;
    PVOID unique_process_id;
    PVOID inherited_from_unique_process_id;
    ULONG handle_count;
    ULONG session_id;
    ULONG_PTR unique_process_key;
    SIZE_T peak_virtual_size;
    SIZE_T virtual_size;
    ULONG page_fault_count;
    SIZE_T peak_working_set_size;
    SIZE_T working_set_size;
    SIZE_T quota_peak_paged_pool_usage;
    SIZE_T quota_paged_pool_usage;
    SIZE_T quota_peak_non_paged_pool_usage;
    SIZE_T quota_non_paged_pool_usage;
    SIZE_T pagefile_usage;
    SIZE_T peak_pagefile_usage;
    SIZE_T private_page_count;
    LARGE_INTEGER read_operation_count;
    LARGE_INTEGER write_operation_count;
    LARGE_INTEGER other_operation_count;
    LARGE_INTEGER read_transfer_count;
    LARGE_INTEGER write_transfer_count;
    LARGE_INTEGER other_transfer_count;
    // SysThreadInfo threads[number_of_threads] follow.
};

#ifdef _WIN64
static_assert(sizeof(SysThreadInfo) == 0x50, "SYSTEM_THREAD_INFORMATION is 0x50 bytes on x64");
static_assert(offsetof(SysProcessInfo, unique_process_id) == 0x50, "UniqueProcessId is at 0x50 on x64");
static_assert(sizeof(SysProcessInfo) == 0x100, "the threads follow the 0x100-byte process entry on x64");
#else
static_assert(sizeof(SysThreadInfo) == 0x40, "SYSTEM_THREAD_INFORMATION is 0x40 bytes on x86");
static_assert(offsetof(SysProcessInfo, unique_process_id) == 0x44, "UniqueProcessId is at 0x44 on x86");
static_assert(sizeof(SysProcessInfo) == 0xB8, "the threads follow the 0xB8-byte process entry on x86");
#endif

constexpr LONG kStatusInfoLengthMismatch = static_cast<LONG>(0xC0000004);

// Parses a SystemProcessInformation snapshot of `returned` bytes for process `pid`: the thread states of that process
// into `out` (up to `capacity`, the count in *count). Every length is compared by subtraction from a length already
// known to be inside the buffer (no sum of an untrusted offset can wrap, on Win32 either), the selected process's thread
// array is bounded by that entry's own extent (its NextEntryOffset, or what is left of the buffer for the last entry),
// and the next offset of every entry is validated before it is used. Fields are read with memcpy, so the buffer needs no
// alignment.
SystemInfoState ParseProcessInformation(const uint8_t* buffer, size_t returned, uintptr_t pid, ThreadState* out,
                                        size_t capacity, size_t* count) noexcept {
    *count = 0;
    size_t offset = 0;
    for (unsigned guard = 0; guard < 1u << 16; ++guard) {
        if (offset > returned || returned - offset < sizeof(SysProcessInfo) || (offset & 7u) != 0)
            return SystemInfoState::Malformed;
        const uint8_t* entry = buffer + offset;
        ULONG next = 0, thread_count = 0;
        PVOID entry_pid = nullptr;
        std::memcpy(&next, entry + offsetof(SysProcessInfo, next_entry_offset), sizeof(next));
        std::memcpy(&thread_count, entry + offsetof(SysProcessInfo, number_of_threads), sizeof(thread_count));
        std::memcpy(&entry_pid, entry + offsetof(SysProcessInfo, unique_process_id), sizeof(entry_pid));
        const size_t left = returned - offset;   // from this entry to the end of the buffer
        if (next != 0 && (next < sizeof(SysProcessInfo) || (next & 7u) != 0 || next > left))
            return SystemInfoState::Malformed;
        if (reinterpret_cast<uintptr_t>(entry_pid) == pid) {
            const size_t extent = next != 0 ? next : left;   // this entry's own bytes
            const size_t room = extent - sizeof(SysProcessInfo);   // extent >= the header: checked above
            if (thread_count > room / sizeof(SysThreadInfo)) return SystemInfoState::Malformed;
            const uint8_t* threads = entry + sizeof(SysProcessInfo);
            for (ULONG i = 0; i < thread_count; ++i) {
                SysThreadInfo thread;
                std::memcpy(&thread, threads + static_cast<size_t>(i) * sizeof(SysThreadInfo), sizeof(thread));
                if (i == 0 && reinterpret_cast<uintptr_t>(thread.client_process) != pid) return SystemInfoState::Malformed;
                const uintptr_t tid = reinterpret_cast<uintptr_t>(thread.client_thread);
                if (tid == 0 || tid > 0xFFFFFFFFull || *count >= capacity) continue;
                out[(*count)++] = {static_cast<uint32_t>(tid), thread.thread_state, thread.wait_reason};
            }
            return SystemInfoState::Ok;
        }
        if (next == 0) return SystemInfoState::NoProcess;
        offset += next;   // next <= left: no wrap
    }
    return SystemInfoState::Malformed;
}

void CollectThreadStates() noexcept {
    s_thread_state_count = 0;
    s_system_info_state = SystemInfoState::Unavailable;
    s_system_info_status = 0;
    s_system_info_needed = 0;
    if (!s_nt_query_system) return;
    ULONG returned = 0;
    s_system_info_status = s_nt_query_system(5 /* SystemProcessInformation */, s_system_info,
                                             static_cast<ULONG>(sizeof(s_system_info)), &returned);
    if (s_system_info_status == kStatusInfoLengthMismatch) {
        // The call says how much it needs; the buffer is static, so this is a report, not a retry.
        s_system_info_state = SystemInfoState::BufferTooSmall;
        s_system_info_needed = returned;
        return;
    }
    if (s_system_info_status < 0) {
        s_system_info_state = SystemInfoState::CallFailed;
        return;
    }
    if (returned > sizeof(s_system_info)) {
        s_system_info_state = SystemInfoState::Malformed;
        return;
    }
    s_system_info_state = ParseProcessInformation(s_system_info, returned, static_cast<uintptr_t>(GetCurrentProcessId()),
                                                  s_thread_states, kMaxThreadStates, &s_thread_state_count);
}

void PutSystemSnapshotState() noexcept {
    switch (s_system_info_state) {
    case SystemInfoState::Ok:
        PutS("ok, ");
        PutU(s_thread_state_count);
        PutS(" thread entries of this process (SystemProcessInformation parsed by its layout, process id and first ClientId checked)");
        break;
    case SystemInfoState::BufferTooSmall:
        PutS("buffer too small: the system returns ");
        PutU(s_system_info_needed);
        PutS(" bytes, the report's static buffer holds ");
        PutU(sizeof(s_system_info));
        PutS("; no thread states shown");
        break;
    case SystemInfoState::NoProcess:
        PutS("this process is not in the snapshot; no thread states shown");
        break;
    case SystemInfoState::Malformed:
        PutS("the snapshot does not match the expected layout; no thread states shown");
        break;
    case SystemInfoState::CallFailed:
        PutS("NtQuerySystemInformation failed (status ");
        PutX(static_cast<uint32_t>(s_system_info_status));
        PutS(")");
        break;
    default:
        PutS("unavailable (NtQuerySystemInformation not found)");
        break;
    }
}

const ThreadState* FindThreadState(uint32_t tid) noexcept {
    for (size_t i = 0; i < s_thread_state_count; ++i)
        if (s_thread_states[i].tid == tid) return &s_thread_states[i];
    return nullptr;
}

// ── One thread ──

struct ThreadSample {
    uint32_t tid;
    bool opened;
    DWORD open_error;
    DWORD suspend_previous;   // the count before our suspend; (DWORD)-1 when the suspend failed
    bool got_context;
    DWORD context_error;
    uint64_t ip, sp;
    bool have_stack;
    uint64_t stack_base, stack_limit;
};

#ifdef _WIN64
constexpr DWORD kContextFlags = CONTEXT_CONTROL;
inline uint64_t ContextIp(const CONTEXT& c) noexcept { return c.Rip; }
inline uint64_t ContextSp(const CONTEXT& c) noexcept { return c.Rsp; }
#else
constexpr DWORD kContextFlags = CONTEXT_CONTROL;
inline uint64_t ContextIp(const CONTEXT& c) noexcept { return c.Eip; }
inline uint64_t ContextSp(const CONTEXT& c) noexcept { return c.Esp; }
#endif

// Suspends, reads the context and resumes at once: nothing else runs while the thread is stopped.
void SuspendReadResume(HANDLE thread, ThreadSample* sample) noexcept {
    const DWORD previous = SuspendThread(thread);
    sample->suspend_previous = previous;
    if (previous == static_cast<DWORD>(-1)) {
        sample->context_error = GetLastError();
        return;
    }
    std::memset(&s_context, 0, sizeof(s_context));
    s_context.ContextFlags = kContextFlags;
    if (GetThreadContext(thread, &s_context)) {
        sample->got_context = true;
        sample->ip = ContextIp(s_context);
        sample->sp = ContextSp(s_context);
    } else {
        sample->context_error = GetLastError();
    }
    ResumeThread(thread);
}

bool ReadOwnMemory(uint64_t address, void* out, size_t length, size_t* got) noexcept {
    SIZE_T read = 0;
    const BOOL ok = ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(address)),
                                      out, length, &read);
    *got = read;
    return ok != FALSE;
}

void ReadStackBounds(HANDLE query_handle, ThreadSample* sample) noexcept {
    if (!s_nt_query_thread || !query_handle) return;
    ThreadBasicInfo info{};
    if (s_nt_query_thread(query_handle, 0 /* ThreadBasicInformation */, &info, sizeof(info), nullptr) < 0 || !info.teb) return;
    NT_TIB tib{};
    size_t got = 0;
    if (!ReadOwnMemory(reinterpret_cast<uint64_t>(info.teb), &tib, sizeof(tib), &got) || got < sizeof(tib)) return;
    sample->have_stack = true;
    sample->stack_base = reinterpret_cast<uint64_t>(tib.StackBase);
    sample->stack_limit = reinterpret_cast<uint64_t>(tib.StackLimit);
}

void PutThread(const ThreadSample& sample, uint32_t writer_thread, uint32_t installer_thread, uint32_t watchdog_thread) noexcept {
    PutS("thread ");
    PutU(sample.tid);
    if (sample.tid == installer_thread) PutS(" [hook installer thread]");
    if (sample.tid == watchdog_thread) PutS(" [stall watchdog]");
    if (sample.tid == writer_thread) PutS(" [writing thread]");
    PutNl();
    if (!sample.opened) {
        PutS("  cannot be opened for suspend and context (error ");
        PutU(sample.open_error);
        PutS("); a protected thread (the hook installer, this watchdog) refuses suspend and context access by design; "
             "no context or stack scan");
        PutNl();
    } else if (sample.suspend_previous == static_cast<DWORD>(-1)) {
        PutS("  SuspendThread failed (error ");
        PutU(sample.context_error);
        PutS(")");
        PutNl();
    } else {
        PutS("  suspend_count_before=");
        PutU(sample.suspend_previous);
        if (sample.suspend_previous != 0) PutS("   <-- already suspended by someone else");
        PutNl();
    }
    const ThreadState* state = FindThreadState(sample.tid);
    PutS("  kernel_state=");
    if (state) {
        PutS(ThreadStateName(state->state));
        if (state->state == 5) {
            PutS(" wait_reason=");
            PutS(WaitReasonName(state->reason));
        }
    } else {
        PutS(s_system_info_state == SystemInfoState::Ok ? "not in the system snapshot" : "unavailable");
    }
    PutNl();
    if (sample.got_context) {
        PutS("  ip=");
        PutModuleOffset(sample.ip);
        PutS("  sp=");
        PutX(sample.sp);
        PutNl();
    } else if (sample.opened && sample.suspend_previous != static_cast<DWORD>(-1)) {
        PutS("  GetThreadContext failed (error ");
        PutU(sample.context_error);
        PutS(")");
        PutNl();
    }
}

// Every pointer-sized value from the stack pointer to the stack base that lies inside an executable section of a
// loaded module, the first kStackHits of them. Not an unwind: what can be read without the loader's or the
// unwinder's locks. Reads through ReadProcessMemory so an unreadable page is an error, not a fault.
void PutStackScan(const ThreadSample& sample) noexcept {
    PutS("  stack_scan: ");
    if (!sample.got_context) {
        PutS("no context");
        PutNl();
        return;
    }
    if (!sample.have_stack || sample.sp < sample.stack_limit || sample.sp >= sample.stack_base) {
        PutS(sample.have_stack ? "the stack pointer is outside the TEB's stack range (a fiber or a foreign stack)"
                               : "the TEB's stack bounds could not be read");
        PutNl();
        return;
    }
    const size_t word = sizeof(uintptr_t);
    uint64_t from = sample.sp & ~static_cast<uint64_t>(word - 1);
    uint64_t to = sample.stack_base;
    if (to - from > kStackScanLimit) to = from + kStackScanLimit;
    alignas(8) uint8_t chunk[4096];
    size_t hits = 0;
    uint64_t scanned = 0;
    bool first = true;
    while (from < to && hits < kStackHits) {
        size_t want = static_cast<size_t>(to - from);
        if (want > sizeof(chunk)) want = sizeof(chunk);
        size_t got = 0;
        ReadOwnMemory(from, chunk, want, &got);
        got &= ~(word - 1);
        for (size_t i = 0; i + word <= got && hits < kStackHits; i += word) {
            uintptr_t value;
            std::memcpy(&value, chunk + i, word);
            int module_index;
            if (!IsCodeAddress(value, &module_index)) continue;
            if (!first) PutS(" ");
            first = false;
            PutModuleOffset(value);
            ++hits;
        }
        scanned += got;
        if (got == 0) break;
        from += got;
        if (got < want) break;
    }
    if (hits == 0) PutS("(no code address found)");
    PutS("   [");
    PutU(hits);
    PutS(" hits, ");
    PutU(scanned);
    PutS(" bytes scanned]");
    PutNl();
}

// ── The report ──

void PutProgress(const HangProgressSnapshot& p, uint64_t now) noexcept {
    PutS("[progress]");
    PutNl();
    PutS("present_events: entered=");
    PutU(p.present_entered);
    PutS(" admitted=");
    PutU(p.present_admitted);
    PutS(" completed=");
    PutU(p.present_completed);
    PutS(" last_completed=");
    PutAge(p.present_completed_qpc, now);
    PutNl();
    PutS("native_present: entered=");
    PutU(p.native_entered);
    PutS(" returned=");
    PutU(p.native_returned);
    PutS(" last_entry=");
    PutAge(p.native_entered_qpc, now);
    PutS(" last_return=");
    PutAge(p.native_returned_qpc, now);
    PutNl();
    PutS("finish_present: count=");
    PutU(p.finish_present);
    PutNl();
    PutS("reflex_sleep: entered=");
    PutU(p.sleep_entered);
    PutS(" left=");
    PutU(p.sleep_left);
    PutS(" last_entry=");
    PutAge(p.sleep_entry_qpc, now);
    PutS(" last_entry_thread=");
    PutU(p.sleep_entry_thread);
    PutNl();
    PutS("latency_marker: count=");
    PutU(p.marker_count);
    PutS(" last=");
    PutAge(p.marker_qpc, now);
    PutNl();
    PutS("swapchain_initialised: ");
    PutBool(p.swapchain_inits != 0);
    PutS(" (init_swapchain callbacks=");
    PutU(p.swapchain_inits);
    PutS(", first=");
    PutAge(p.first_swapchain_init_qpc, now);
    PutS(")");
    PutNl();
}

void PutOwner(const char* name, const LockOwnerSnapshot& s, const char* (*user_name)(uint8_t), uint64_t now) noexcept {
    PutS(name);
    PutS(": holder=");
    if (s.holder_thread) {
        PutS("thread ");
        PutU(s.holder_thread);
        PutS(" (");
        PutS(user_name(s.holder_what));
        PutS(")");
        if (s.holder_qpc) {
            PutS(" since ");
            PutAge(s.holder_qpc, now);
        }
    } else {
        PutS("nobody");
    }
    PutS("  waiter=");
    if (s.waiter_thread) {
        PutS("thread ");
        PutU(s.waiter_thread);
        PutS(" (");
        PutS(user_name(s.waiter_what));
        PutS(") since ");
        PutAge(s.waiter_qpc, now);
    } else {
        PutS("nobody");
    }
    PutNl();
}

void PutLocks(uint64_t now) noexcept {
    PutS("[locks]");
    PutNl();
    LockOwnerSnapshot selection;
    if (s_selection_source) {
        s_selection_source(&selection);
        PutOwner("selection_lock", selection, SelectionLockUserName, now);
    } else {
        PutS("selection_lock: no source");
        PutNl();
    }
    HookDiagSnapshot hooks;
    Hooks_DiagSnapshot(&hooks);
    PutOwner("hook_install_lock", hooks.install_lock, InstallLockUserName, now);
    PutS("installer_thread: state=");
    switch (hooks.installer_state) {
    case HookInstallerState::None: PutS("none"); break;
    case HookInstallerState::Starting: PutS("starting"); break;
    case HookInstallerState::Ready: PutS("ready"); break;
    case HookInstallerState::Unprotected: PutS("unprotected"); break;
    case HookInstallerState::Stopped: PutS("stopped"); break;
    }
    PutS(" thread=");
    PutU(hooks.installer_thread_id);
    PutS(" minhook_unusable=");
    PutBool(hooks.minhook_unusable);
    PutNl();
    PutS("installer_job: state=");
    PutS(Hooks_DiagJobStateName(hooks.job_state));
    PutS(" op=");
    PutS(hooks.job_posted_qpc ? Hooks_DiagOpName(hooks.job_op) : "none yet");
    PutS(" requester_thread=");
    PutU(hooks.job_requester_thread);
    PutS(" posted=");
    PutAge(hooks.job_posted_qpc, now);
    PutS(" taken=");
    PutAge(hooks.job_taken_qpc, now);
    PutS(" done=");
    PutAge(hooks.job_done_qpc, now);
    PutNl();
}

void PutTrailEntry(const TrailEntry& e, uint64_t now) noexcept {
    PutS("  ");
    PutI(TicksToMs(static_cast<int64_t>(now) - static_cast<int64_t>(e.qpc)));
    PutS(" ms ago  thread ");
    PutU(e.thread_id);
    PutS("  ");
    PutS(Breadcrumb_TrailKindName(e.kind));
    switch (static_cast<TrailKind>(e.kind)) {
    case TrailKind::Stage: {
        PutS(" ");
        PutS(Breadcrumb_StageText(e.site));
        const StartupStage stage = static_cast<StartupStage>(e.site);
        if (stage == StartupStage::BatchApplyEntered || stage == StartupStage::BatchApplyLeft) {
            PutS(" site=");
            PutS(Breadcrumb_BatchSiteText(static_cast<uint8_t>(e.aux)));
            if (e.aux & kBreadcrumbInstallerRan) PutS(" ran_on_installer");
            else if (e.aux & kBreadcrumbInstallerProtected) PutS(" installer_protected_not_used");
            else if (e.aux & kBreadcrumbInstallerUnprotected) PutS(" installer_unprotected");
        }
        break;
    }
    case TrailKind::FreezeEnter:
        PutS(" op=");
        PutS(Hooks_DiagOpName(e.site));
        PutS(" to_installer=");
        PutU(e.aux & 1u);
        break;
    case TrailKind::FreezeLeave:
        PutS(" op=");
        PutS(Hooks_DiagOpName(e.site));
        PutS(" ran_on_installer=");
        PutU(e.aux & 1u);
        PutS(" status=");
        PutI(static_cast<int32_t>(e.extra));
        break;
    case TrailKind::InitDeviceEnter: case TrailKind::InitDeviceLeave:
    case TrailKind::DestroyDeviceEnter: case TrailKind::DestroyDeviceLeave:
    case TrailKind::InitSwapchainEnter: case TrailKind::InitSwapchainLeave:
    case TrailKind::DestroySwapchainEnter: case TrailKind::DestroySwapchainLeave:
    case TrailKind::DestroyQueueEnter: case TrailKind::DestroyQueueLeave:
        PutS(" api=");
        PutS(Breadcrumb_TrailApiName(e.site));
        PutS(" native=");
        PutX(e.extra);
        if (e.aux & 1u) PutS(" resize");
        break;
    case TrailKind::QueueEnsureHooksLeave:
        PutS(" installed=");
        PutU(e.site);
        break;
    case TrailKind::FirstQueueExecute: case TrailKind::FirstQueueSignal: case TrailKind::FirstQueueWait:
    case TrailKind::ProbeSessionEnd:
        PutS(" queue=");
        PutX(e.extra);
        break;
    case TrailKind::ProbeSessionStart:
        PutS(" role=");
        PutU(e.site);
        PutS(" queue=");
        PutX(e.extra);
        break;
    default:
        break;
    }
    PutNl();
}

void PutTrail(uint64_t now) noexcept {
    const size_t count = Breadcrumb_TrailSnapshot(s_trail, kTrailCapacityLocal);
    PutS("[trail] ");
    PutU(count);
    PutS(" entries, oldest first (the crumb file's ring; age is before this report)");
    PutNl();
    if (count == 0) {
        PutS("  empty or unavailable (no crumb file attached)");
        PutNl();
    }
    for (size_t i = 0; i < count; ++i) PutTrailEntry(s_trail[i], now);
}

void PutModules() noexcept {
    PutS("[modules] ");
    PutU(s_module_count);
    PutS(" (the loader's list read without the loader lock)");
    PutNl();
    for (size_t i = 0; i < s_module_count; ++i) {
        const ModuleRecord& m = s_modules[i];
        PutS("  ");
        PutX(m.base, sizeof(void*) * 2);
        PutS(" size=");
        PutX(m.size);
        PutS(" timestamp=");
        PutX(m.timestamp);
        PutS(" ");
        PutS(m.name);
        PutNl();
    }
}

struct ThreadCounts {
    uint32_t listed = 0;
    uint32_t sampled = 0;
};

void ProcessThread(uint32_t tid, HANDLE limited_handle, uint32_t writer_thread, uint32_t installer_thread,
                   uint32_t watchdog_thread, ThreadCounts* counts) noexcept {
    ThreadSample sample{};
    sample.tid = tid;
    sample.suspend_previous = static_cast<DWORD>(-1);
    if (tid == writer_thread) {
        // The writing thread is running this very code: its context would be a moving target, and it is not stuck.
        PutS("thread ");
        PutU(tid);
        PutS(" [writing thread] not sampled");
        if (tid == watchdog_thread) PutS(" [stall watchdog]");
        PutNl();
        return;
    }
    ++counts->listed;
    HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    HANDLE query = limited_handle;
    if (thread) {
        sample.opened = true;
        query = thread;
        SuspendReadResume(thread, &sample);   // resumed before anything below touches the thread again
    } else {
        sample.open_error = GetLastError();
    }
    ReadStackBounds(query, &sample);
    if (thread) CloseHandle(thread);
    if (sample.got_context) ++counts->sampled;
    PutThread(sample, writer_thread, installer_thread, watchdog_thread);
    if (sample.opened) PutStackScan(sample);
}

// The first call of the Nt path failed or is unavailable: Toolhelp, which allocates (the last thing the report does).
void ProcessThreadsToolhelp(uint32_t writer_thread, uint32_t installer_thread, uint32_t watchdog_thread,
                            ThreadCounts* counts) noexcept {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        PutS("thread list unavailable (NtGetNextThread and the Toolhelp snapshot both failed, error ");
        PutU(GetLastError());
        PutS(")");
        PutNl();
        return;
    }
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    const DWORD pid = GetCurrentProcessId();
    if (Thread32First(snapshot, &entry)) {
        do {
            if (entry.th32OwnerProcessID == pid)
                ProcessThread(entry.th32ThreadID, nullptr, writer_thread, installer_thread, watchdog_thread, counts);
        } while (Thread32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
}

void PutThreads(uint32_t installer_thread, ThreadCounts* counts) noexcept {
    const uint32_t writer_thread = GetCurrentThreadId();
    const uint32_t watchdog_thread = s_watchdog_thread_id.load(std::memory_order_relaxed);
    PutS("[threads] every thread of the process; each is suspended only for its GetThreadContext and resumed before "
         "anything else is done with it");
    PutNl();
    PutS("stack_scan: from the stack pointer to the stack base, values inside an executable section of a loaded "
         "module, the first ");
    PutU(kStackHits);
    PutS(" (not an unwind)");
    PutNl();
#ifdef _WIN64
    PutS("architecture: x64");
#else
    PutS("architecture: x86 (32-bit add-on)");
#endif
    PutS("  system_snapshot: ");
    PutSystemSnapshotState();
    PutNl();
    Flush();

    bool nt_path = false;
    if (s_nt_get_next_thread) {
        HANDLE previous = nullptr;
        for (unsigned guard = 0; guard < 8192; ++guard) {
            HANDLE next = nullptr;
            const LONG status = s_nt_get_next_thread(GetCurrentProcess(), previous, THREAD_QUERY_LIMITED_INFORMATION, 0, 0, &next);
            if (previous) CloseHandle(previous);
            previous = nullptr;
            if (status < 0 || !next) break;
            nt_path = true;
            ThreadBasicInfo info{};
            uint32_t tid = 0;
            if (s_nt_query_thread && s_nt_query_thread(next, 0, &info, sizeof(info), nullptr) >= 0)
                tid = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(info.unique_thread));
            if (tid != 0)
                ProcessThread(tid, next, writer_thread, installer_thread, watchdog_thread, counts);
            previous = next;
        }
        if (previous) CloseHandle(previous);   // the loop may end on its guard with the last handle still held
    }
    if (!nt_path) ProcessThreadsToolhelp(writer_thread, installer_thread, watchdog_thread, counts);
    PutS("[end of threads: ");
    PutU(counts->listed);
    PutS(" listed, ");
    PutU(counts->sampled);
    PutS(" with a context]");
    PutNl();
}

// The game's window as the swap manager last knew it: responding (the shell's own hung-window test, which sends
// nothing), minimised, visible, foreground. Tells a hang from a pause (alt-tab, minimised, loading).
void PutWindowState() noexcept {
    PutS("window: ");
    HWND window = s_window_source ? static_cast<HWND>(s_window_source()) : nullptr;
    if (!window) {
        PutS("none known");
        PutNl();
        return;
    }
    PutX(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(window)));
    if (!IsWindow(window)) {
        PutS(" no longer exists");
        PutNl();
        return;
    }
    PutS(" responding=");
    if (IsHungAppWindow(window)) PutS("NO (IsHungAppWindow)");
    else PutS("yes");
    PutS(" minimised=");
    PutBool(IsIconic(window) != FALSE);
    PutS(" visible=");
    PutBool(IsWindowVisible(window) != FALSE);
    const HWND foreground = GetForegroundWindow();
    PutS(" foreground=");
    PutBool(foreground && (foreground == window || GetAncestor(window, GA_ROOT) == GetAncestor(foreground, GA_ROOT)));
    PutNl();
}

// CPU time of the process over the watchdog's last wake interval (kept each wake; nothing is measured here).
void PutCpuState() noexcept {
    PutS("process_cpu_last_wake: ");
    const uint64_t wall = s_cpu_wall.load(std::memory_order_relaxed);
    const uint64_t cpu = s_cpu_delta.load(std::memory_order_relaxed);
    if (wall == 0 || s_frequency == 0) {
        PutS("not measured yet");
        PutNl();
        return;
    }
    const uint64_t wall_100ns = wall * 10000000ull / s_frequency;
    PutU(cpu / 10000);
    PutS(" ms of CPU time in ");
    PutU(wall_100ns / 10000);
    PutS(" ms of wall time");
    if (wall_100ns != 0) {
        PutS(" (");
        PutU(cpu * 100 / wall_100ns);
        PutS("% of one core)");
    }
    PutNl();
}

void PutHeader(HangReason reason, uint32_t index, uint64_t stalled_ms, uint64_t now, const HangProgressSnapshot& p) noexcept {
    PutS("ReLimiter stall report");
    PutNl();
#ifdef _WIN64
    PutS("architecture: x64");
#else
    PutS("architecture: x86");
#endif
    PutNl();
    PutS("reason=");
    PutS(HangReasonName(reason));
    if (reason == HangReason::ForcedTest) {
        PutS("  (hang_report_test_after_s: the process is not necessarily stalled)");
    } else {
        PutS("  (no present event completed for ");
        PutU(stalled_ms);
        PutS(" ms");
        if (p.present_completed == 0) PutS("; none ever completed, counted from the first swapchain initialisation");
        PutS(")");
    }
    PutNl();
    PutS("report: ");
    PutU(index);
    PutS(" of at most ");
    PutU(kHangMaxReports);
    PutNl();
    PutS("exe: ");
    PutS(s_process_name);
    PutS("  pid: ");
    PutU(GetCurrentProcessId());
    PutNl();
    SYSTEMTIME wall;
    GetSystemTime(&wall);
    PutS("wall_time_utc: ");
    PutPad(wall.wYear, 4);
    PutS("-");
    PutPad(wall.wMonth, 2);
    PutS("-");
    PutPad(wall.wDay, 2);
    PutS("T");
    PutPad(wall.wHour, 2);
    PutS(":");
    PutPad(wall.wMinute, 2);
    PutS(":");
    PutPad(wall.wSecond, 2);
    PutS(".");
    PutPad(wall.wMilliseconds, 3);
    PutS("Z");
    PutNl();
    PutS("qpc: ");
    PutU(now);
    PutS("  qpc_frequency: ");
    PutU(s_frequency);
    PutNl();
    PutS("since_last_completed_present: ");
    if (p.present_completed_qpc) {
        PutI(TicksToMs(static_cast<int64_t>(now) - static_cast<int64_t>(p.present_completed_qpc)));
        PutS(" ms");
    } else {
        PutS("none completed");
    }
    PutNl();
    PutS("watchdog_thread: protected descriptor built=");
    if (s_thread_protected) {
        PutS("yes");
    } else {
        PutS("NO (error ");
        PutU(s_security_error);
        PutS(")");
    }
    PutS("  effective=");
    switch (s_effective_protection.load(std::memory_order_acquire)) {
    case 1: PutS("yes (opening the thread for suspend is refused)"); break;
    case 0: PutS("NO (opening the thread for suspend succeeds, for example the debug privilege is enabled: another module's freeze could suspend it)"); break;
    case 2:
        PutS("UNKNOWN (opening the thread for suspend failed with error ");
        PutU(s_protection_error.load(std::memory_order_relaxed));
        PutS(": neither refused nor opened)");
        break;
    default: PutS("not checked yet"); break;
    }
    PutNl();
    PutWindowState();
    PutCpuState();
}

HANDLE OpenForWrite(const wchar_t* path, bool append) noexcept {
    if (append)
        return CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    return CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

struct TextOutcome {
    bool ok = false;
    uint8_t stage = 0;   // the step that failed: 1 create, 2 write, 3 flush to disk, 4 close; 0 none
    DWORD error = 0;
    uint32_t short_written = 0;     // a short write (no error code: the call itself succeeded): bytes written ...
    uint32_t short_expected = 0;    // ... of bytes asked; 0 when the failure was not a short write
};

// Step 1: the text file. ok only when it was created, written, flushed to disk and closed without an error: the dump
// step is entered only then.
TextOutcome WriteTextReport(uint32_t slot, HangReason reason, uint32_t index, uint64_t stalled_ms, ThreadCounts* counts,
                            uint64_t* bytes) noexcept {
    TextOutcome outcome;
    s_file = OpenForWrite(s_text_path_w[slot], false);
    if (s_file == INVALID_HANDLE_VALUE) {
        outcome.stage = 1;
        outcome.error = GetLastError();
        return outcome;
    }
    s_used = 0;
    s_write_failed = false;
    s_write_error = 0;
    s_short_written = 0;
    s_short_expected = 0;
    s_written_total = 0;

    const uint64_t now = NowQpc();
    const HangProgressSnapshot progress = HangProgress_Read();
    PutHeader(reason, index, stalled_ms, now, progress);
    Flush();
    PutProgress(progress, now);
    Flush();
    PutLocks(now);
    Flush();
    PutTrail(now);
    Flush();
    CollectModules();
    PutModules();
    Flush();
    CollectThreadStates();
    HookDiagSnapshot hooks;
    Hooks_DiagSnapshot(&hooks);
    PutThreads(hooks.installer_thread_id, counts);
    Flush();
    if (s_write_failed) {
        outcome.stage = 2;
        outcome.error = s_write_error;
        outcome.short_written = s_short_written;
        outcome.short_expected = s_short_expected;
    }
    if (!FlushFileBuffers(s_file) && outcome.stage == 0) {
        outcome.stage = 3;
        outcome.error = GetLastError();
    }
    *bytes = s_written_total;
    if (!CloseHandle(s_file) && outcome.stage == 0) {
        outcome.stage = 4;
        outcome.error = GetLastError();
    }
    s_file = INVALID_HANDLE_VALUE;
    outcome.ok = outcome.stage == 0;
    return outcome;
}

// Step 2: dbghelp and the minidump of this process. May block for good (the loader lock for LoadLibrary, the heap
// for dbghelp's own allocations): the text file is closed before it. Structured exception handling around the call.
bool CallMiniDumpWriteDump(MiniDumpWriteDumpFn function, HANDLE file, DWORD type, DWORD* error) noexcept {
    __try {
        if (function(GetCurrentProcess(), GetCurrentProcessId(), file, static_cast<MINIDUMP_TYPE>(type), nullptr, nullptr, nullptr))
            return true;
        *error = GetLastError();
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *error = GetExceptionCode();
        return false;
    }
}

enum class DumpStep : uint8_t { Written, NoFile, NoDbghelp, NoFunction, Failed, CloseFailed, Skipped };

DumpStep WriteDump(uint32_t slot, DWORD* error, uint64_t* bytes) noexcept {
    *error = 0;
    HANDLE library = LoadLibraryExW(s_dbghelp_path_w, nullptr, 0);
    if (!library) {
        *error = GetLastError();
        return DumpStep::NoDbghelp;
    }
    const auto function = reinterpret_cast<MiniDumpWriteDumpFn>(GetProcAddress(static_cast<HMODULE>(library), "MiniDumpWriteDump"));
    if (!function) {
        *error = GetLastError();
        return DumpStep::NoFunction;
    }
    HANDLE file = OpenForWrite(s_dump_path_w[slot], false);
    if (file == INVALID_HANDLE_VALUE) {
        *error = GetLastError();
        return DumpStep::NoFile;
    }
    const bool ok = CallMiniDumpWriteDump(function, file, s_dump_type, error);
    LARGE_INTEGER size{};
    if (GetFileSizeEx(file, &size)) *bytes = static_cast<uint64_t>(size.QuadPart);
    // The close is part of the result: a failed close (a deferred write error) is not a written dump.
    const bool closed = CloseHandle(file) != 0;
    const DWORD close_error = closed ? 0 : GetLastError();
    if (ok && !closed) {
        *error = close_error;
        return DumpStep::CloseFailed;
    }
    return ok ? DumpStep::Written : DumpStep::Failed;
}

// Step 3a: the dump's result as one line appended to the text file.
bool AppendDumpResult(uint32_t slot, DumpStep step, DWORD error, uint64_t bytes, uint32_t ms) noexcept {
    s_file = OpenForWrite(s_text_path_w[slot], true);
    if (s_file == INVALID_HANDLE_VALUE) return false;
    s_used = 0;
    s_write_failed = false;
    PutS("[dump] ");
    switch (step) {
    case DumpStep::Written:
        PutS("written: ");
        PutU(bytes);
        PutS(" bytes in ");
        PutU(ms);
        PutS(" ms");
        break;
    case DumpStep::NoDbghelp: PutS("dbghelp.dll could not be loaded, error "); PutU(error); break;
    case DumpStep::NoFunction: PutS("MiniDumpWriteDump not found in dbghelp.dll, error "); PutU(error); break;
    case DumpStep::NoFile: PutS("the dump file could not be created, error "); PutU(error); break;
    case DumpStep::CloseFailed:
        PutS("the dump was written but closing its file failed, error ");
        PutU(error);
        PutS(", file has ");
        PutU(bytes);
        PutS(" bytes");
        break;
    case DumpStep::Failed:
        PutS("MiniDumpWriteDump failed, error/exception ");
        PutX(error);
        PutS(", file has ");
        PutU(bytes);
        PutS(" bytes after ");
        PutU(ms);
        PutS(" ms");
        break;
    case DumpStep::Skipped: PutS("not attempted: hang_report_dump=false"); break;
    }
    PutNl();
    Flush();
    const bool ok = !s_write_failed;
    CloseHandle(s_file);
    s_file = INVALID_HANDLE_VALUE;
    return ok;
}

uint32_t ElapsedMs(uint64_t start) noexcept {
    return s_frequency ? static_cast<uint32_t>((NowQpc() - start) * 1000 / s_frequency) : 0;
}

const char* TextStageName(uint8_t stage) noexcept {
    switch (stage) {
    case 1: return "creating the file";
    case 2: return "writing";
    case 3: return "flushing to disk";
    case 4: return "closing";
    }
    return "?";
}

const char* DumpStepName(DumpStep step) noexcept {
    switch (step) {
    case DumpStep::Written: return "written";
    case DumpStep::NoFile: return "the dump file could not be created";
    case DumpStep::NoDbghelp: return "dbghelp.dll could not be loaded";
    case DumpStep::NoFunction: return "MiniDumpWriteDump was not found";
    case DumpStep::Failed: return "MiniDumpWriteDump failed";
    case DumpStep::CloseFailed: return "the dump was written but closing its file failed";
    case DumpStep::Skipped: return "skipped by hang_report_dump=false";
    }
    return "?";
}

// ── Paths ──

// "<dir>\relimiter_<exe>.log" -> "<dir>\relimiter_<exe><suffix>"; false when `log_path` is not a .log path or the
// result does not fit.
bool SiblingPath(const char* log_path, const char* suffix, char* out, size_t capacity) noexcept {
    const size_t length = std::strlen(log_path);
    if (length < 4 || _stricmp(log_path + length - 4, ".log") != 0) return false;
    const size_t base = length - 4;
    const size_t suffix_length = std::strlen(suffix);
    if (base + suffix_length + 1 > capacity) return false;
    std::memcpy(out, log_path, base);
    std::memcpy(out + base, suffix, suffix_length + 1);
    return true;
}

// The six file names of the three reports from `log_path`, narrow and wide; false when one does not fit MAX_PATH or
// does not convert.
bool BuildSiblingPaths(const char* log_path) noexcept {
    static const char* const kTextSuffix[kHangMaxReports] = {".hang.txt", ".hang.2.txt", ".hang.3.txt"};
    static const char* const kDumpSuffix[kHangMaxReports] = {".hang.dmp", ".hang.2.dmp", ".hang.3.dmp"};
    for (uint32_t i = 0; i < kHangMaxReports; ++i) {
        if (!SiblingPath(log_path, kTextSuffix[i], s_text_path_a[i], MAX_PATH) ||
            !SiblingPath(log_path, kDumpSuffix[i], s_dump_path_a[i], MAX_PATH)) return false;
        if (!MultiByteToWideChar(CP_ACP, 0, s_text_path_a[i], -1, s_text_path_w[i], MAX_PATH) ||
            !MultiByteToWideChar(CP_ACP, 0, s_dump_path_a[i], -1, s_dump_path_w[i], MAX_PATH)) return false;
    }
    return true;
}

// "<temp>relimiter_<exe>.log" for the fallback.
bool TempLogPath(char* out, size_t capacity) noexcept {
    char temp[MAX_PATH];
    const DWORD length = GetTempPathA(MAX_PATH, temp);
    if (length == 0 || length >= MAX_PATH) return false;
    const int n = std::snprintf(out, capacity, "%srelimiter_%s.log", temp, s_process_name);
    return n > 0 && static_cast<size_t>(n) < capacity;
}

// The report files sit next to the log. When that cannot be (no log path, not a .log path, or the report names would
// exceed MAX_PATH) the same names in the temporary folder are used and one warning names the reason; when even that
// fails the report is off and one warning says why.
bool PreparePaths(const HangReportOptions& options) noexcept {
    char log_path[MAX_PATH] = "";
    const char* why = nullptr;   // why the log's own folder is not used
    const char* given = options.log_path;
    if (!given || !*given) {
        why = "no log file path is known";
    } else if (std::strlen(given) >= MAX_PATH) {
        why = "the log path is longer than MAX_PATH";
    } else {
        std::memcpy(log_path, given, std::strlen(given) + 1);
        const size_t length = std::strlen(log_path);
        if (length < 4 || _stricmp(log_path + length - 4, ".log") != 0)
            why = "the log path does not end in .log";
        else if (!BuildSiblingPaths(log_path))
            why = "the report file names next to the log would exceed MAX_PATH";
    }
    if (why) {
        char fallback[MAX_PATH];
        if (!TempLogPath(fallback, sizeof(fallback)) || !BuildSiblingPaths(fallback)) {
            LOG_WARN("Stall report is off: %s, and the temporary folder cannot hold the report files either", why);
            return false;
        }
        LOG_WARN("Stall report: %s; the report files go to the temporary folder instead, for example %s", why,
                 s_text_path_a[0]);
    }
    const UINT system_length = GetSystemDirectoryW(s_dbghelp_path_w, MAX_PATH);
    if (system_length == 0 || system_length + 16 >= MAX_PATH) {
        LOG_WARN("Stall report is off: the system folder path could not be read");
        return false;
    }
    std::memcpy(s_dbghelp_path_w + system_length, L"\\dbghelp.dll", sizeof(L"\\dbghelp.dll"));
    return true;
}

// ── The watchdog ──

// The process's CPU time over the last wake interval, kept each wake: GetProcessTimes (the QPC is read anyway).
void UpdateCpuTimes(uint64_t now) noexcept {
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return;
    const uint64_t total = ((static_cast<uint64_t>(kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime) +
                           ((static_cast<uint64_t>(user.dwHighDateTime) << 32) | user.dwLowDateTime);
    const uint64_t previous_total = s_cpu_previous_total.load(std::memory_order_relaxed);
    const uint64_t previous_qpc = s_cpu_previous_qpc.load(std::memory_order_relaxed);
    if (previous_qpc != 0 && now > previous_qpc && total >= previous_total) {
        s_cpu_delta.store(total - previous_total, std::memory_order_relaxed);
        s_cpu_wall.store(now - previous_qpc, std::memory_order_relaxed);
    }
    s_cpu_previous_total.store(total, std::memory_order_relaxed);
    s_cpu_previous_qpc.store(now, std::memory_order_relaxed);
}

void Tick(uint64_t start_qpc) noexcept {
    const uint64_t now = NowQpc();
    UpdateCpuTimes(now);
    if (s_floor_pending.load(std::memory_order_relaxed) &&
        now - start_qpc >= static_cast<uint64_t>(s_floor_delay_s) * s_frequency) {
        // The diagnostic window ends: the log's own requested level (whatever was requested meanwhile) is effective.
        s_floor_pending.store(false, std::memory_order_relaxed);
        Log_EndLevelFloor();
    }
    if (!s_automatic) return;
    const HangProgressSnapshot p = HangProgress_Read();
    HangPolicyInput input;
    input.now_qpc = now;
    input.frequency = s_frequency;
    input.completed = p.present_completed;
    input.first_completed_qpc = p.present_first_completed_qpc;
    input.last_completed_qpc = p.present_completed_qpc;
    input.swapchain_inits = p.swapchain_inits;
    input.first_swapchain_init_qpc = p.first_swapchain_init_qpc;
    const HangDecision decision = HangPolicy_Evaluate(s_policy_config, &s_policy_state, input);
    if (decision.reason != HangReason::None) {
        // The outcome is kept (HangReport_LastResult), not discarded: Write logs it per file as well.
        s_last_result = HangReport_Write(decision.reason, decision.index, decision.stalled_ms);
        s_last_result_count.fetch_add(1, std::memory_order_release);
    }
}

DWORD WINAPI WatchdogMain(void*) {
    s_watchdog_thread_id.store(GetCurrentThreadId(), std::memory_order_relaxed);
    // As the installer thread checks itself: the descriptor only counts when opening this very thread for suspend is
    // actually refused (a process with the debug privilege enabled bypasses the descriptor).
    HANDLE self = OpenThread(THREAD_SUSPEND_RESUME, FALSE, GetCurrentThreadId());
    if (self) {
        CloseHandle(self);
        s_effective_protection.store(0, std::memory_order_release);
    } else {
        const DWORD error = GetLastError();
        if (error == ERROR_ACCESS_DENIED) {
            s_effective_protection.store(1, std::memory_order_release);
        } else {
            // Neither refused nor opened: the probe is inconclusive, and the report says so with the error.
            s_protection_error.store(error, std::memory_order_relaxed);
            s_effective_protection.store(2, std::memory_order_release);
        }
    }
    const uint64_t start = NowQpc();
    UpdateCpuTimes(start);
    for (;;) {
        if (WaitForSingleObject(s_stop_event, 1000) != WAIT_TIMEOUT) return 0;
        Tick(start);
    }
}

void WarnAboutEarlierReport() noexcept {
    if (s_policy_state.reports != 0) return;   // this process's own report (a reload), not an earlier launch's
    if (GetFileAttributesW(s_text_path_w[0]) == INVALID_FILE_ATTRIBUTES) return;
    LOG_WARN("A report of an earlier launch is next to this log: %s (and %s). The earlier launch stopped responding, or "
             "a forced test report was written; send both files. They are not deleted.",
             s_text_path_a[0], s_dump_path_a[0]);
}

} // namespace

bool HangReport_Start(const HangReportOptions& options) noexcept {
    if (!options.enabled) return false;
    if (s_thread) return true;   // already running (the same load)

    std::memset(s_process_name, 0, sizeof(s_process_name));
    if (options.process_name) {
        size_t n = 0;
        while (options.process_name[n] && n + 1 < sizeof(s_process_name)) { s_process_name[n] = options.process_name[n]; ++n; }
    }
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    s_frequency = static_cast<uint64_t>(frequency.QuadPart);
    s_paths_ready = PreparePaths(options);   // it warns by itself when the report cannot be set up
    if (!s_paths_ready) return false;

    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll) {
        s_nt_get_next_thread = reinterpret_cast<NtGetNextThreadFn>(GetProcAddress(ntdll, "NtGetNextThread"));
        s_nt_query_thread = reinterpret_cast<NtQueryInformationThreadFn>(GetProcAddress(ntdll, "NtQueryInformationThread"));
        s_nt_query_system = reinterpret_cast<NtQuerySystemInformationFn>(GetProcAddress(ntdll, "NtQuerySystemInformation"));
    }
    s_selection_source = options.selection_lock_source;
    s_window_source = options.window_source;
    s_dump_enabled = options.dump_enabled;
    s_automatic = options.automatic_reports;
    s_policy_config.enabled = true;
    s_policy_config.test_after_s = options.test_after_s;

    s_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!s_stop_event) return false;
    // The installer thread's descriptor (hooks.h): another module's thread freeze cannot suspend the watchdog. Without
    // it the thread is created anyway and the report says so; whether it is effective the thread checks itself.
    HookThreadSecurity security;
    SECURITY_ATTRIBUTES* attributes = nullptr;
    s_thread_protected = Hooks_BuildProtectedThreadSecurity(&security);
    if (s_thread_protected) {
        attributes = &security.attributes;
        s_security_error = 0;
    } else {
        s_security_error = GetLastError();
    }
    s_effective_protection.store(-1, std::memory_order_release);
    // The diagnostic window: the log stays at least at info until the watchdog ends it, whatever is requested meanwhile.
    s_floor_pending.store(true, std::memory_order_relaxed);
    Log_BeginLevelFloor(LogLevel::Info);
    s_thread = CreateThread(attributes, 0, WatchdogMain, nullptr, 0, nullptr);
    if (!s_thread) {
        s_floor_pending.store(false, std::memory_order_relaxed);
        Log_EndLevelFloor();
        CloseHandle(s_stop_event);
        s_stop_event = nullptr;
        return false;
    }
    s_started.store(true, std::memory_order_release);
    if (s_dump_enabled)
        LOG_INFO("Stall report is on (hang_report=true): a stalled launch writes %s and %s", s_text_path_a[0], s_dump_path_a[0]);
    else
        LOG_INFO("Stall report is on (hang_report=true, hang_report_dump=false): a stalled launch writes %s", s_text_path_a[0]);
    WarnAboutEarlierReport();
    return true;
}

bool HangReport_Stop() noexcept {
    if (!s_thread) return true;
    SetEvent(s_stop_event);
    if (WaitForSingleObject(s_thread, 5000) != WAIT_OBJECT_0) return false;
    CloseHandle(s_thread);
    s_thread = nullptr;
    CloseHandle(s_stop_event);
    s_stop_event = nullptr;
    s_started.store(false, std::memory_order_release);
    s_watchdog_thread_id.store(0, std::memory_order_relaxed);
    if (s_floor_pending.exchange(false, std::memory_order_relaxed)) Log_EndLevelFloor();
    return true;
}

HangReportResult HangReport_Write(HangReason reason, uint32_t index, uint64_t stalled_ms) noexcept {
    HangReportResult result;
    if (!s_paths_ready) return result;
    if (s_writing.exchange(true, std::memory_order_acquire)) return result;
    result.attempted = true;
    if (index < 1 || index > kHangMaxReports) index = 1;
    const uint32_t slot = index - 1;
    const char* const text_path = s_text_path_a[slot];
    const char* const dump_path = s_dump_path_a[slot];

    const uint64_t text_start = NowQpc();
    ThreadCounts counts;
    uint64_t text_bytes = 0;
    const TextOutcome text = WriteTextReport(slot, reason, index, stalled_ms, &counts, &text_bytes);
    result.text_ok = text.ok;
    result.text_stage = text.stage;
    result.text_error = text.error;
    result.text_short_written = text.short_written;
    result.text_short_expected = text.short_expected;
    result.text_bytes = text_bytes;
    result.text_ms = ElapsedMs(text_start);
    result.threads_listed = counts.listed;
    result.threads_sampled = counts.sampled;

    if (!text.ok) {
        // The text-first rule failed: nothing is loaded and no dump is attempted.
        char detail[96];
        if (text.short_expected != 0)
            std::snprintf(detail, sizeof(detail), "short write: %lu of %lu bytes",
                          static_cast<unsigned long>(text.short_written), static_cast<unsigned long>(text.short_expected));
        else
            std::snprintf(detail, sizeof(detail), "error %lu", static_cast<unsigned long>(text.error));
        LOG_WARN("Stall report FAILED: %s was not completed (%s failed, %s); the dump was not attempted (reason %s)",
                 text_path, TextStageName(text.stage), detail, HangReasonName(reason));
        s_writing.store(false, std::memory_order_release);
        return result;
    }

    DumpStep step = DumpStep::Skipped;
    DWORD error = 0;
    uint64_t dump_bytes = 0;
    const uint64_t dump_start = NowQpc();
    if (s_dump_enabled) {
        result.dump_attempted = true;
        step = WriteDump(slot, &error, &dump_bytes);
    }
    result.dump_ms = ElapsedMs(dump_start);
    result.dump_ok = step == DumpStep::Written;
    result.dump_error = error;
    result.dump_bytes = dump_bytes;
    result.result_line_ok = AppendDumpResult(slot, step, error, dump_bytes, result.dump_ms);

    const char* const note = result.result_line_ok ? "" : " (the result line could not be appended to the text file)";
    if (step == DumpStep::Written) {
        LOG_WARN("Stall report written: %s and %s (the process did not complete a present event for %llu ms, reason %s)%s",
                 text_path, dump_path, static_cast<unsigned long long>(stalled_ms), HangReasonName(reason), note);
    } else if (step == DumpStep::Skipped) {
        LOG_WARN("Stall report written: %s (no dump: hang_report_dump=false; the process did not complete a present event "
                 "for %llu ms, reason %s)%s", text_path, static_cast<unsigned long long>(stalled_ms), HangReasonName(reason), note);
    } else {
        LOG_WARN("Stall report: %s written, but the dump %s was not (%s, error 0x%lx; the process did not complete a present "
                 "event for %llu ms, reason %s)%s", text_path, dump_path, DumpStepName(step), static_cast<unsigned long>(error),
                 static_cast<unsigned long long>(stalled_ms), HangReasonName(reason), note);
    }
    s_writing.store(false, std::memory_order_release);
    return result;
}

HangReportResult HangReport_LastResult() noexcept {
    // The watchdog may assign s_last_result again while it runs (a forced report can be followed by the ordinary stall
    // report), so a copy is valid only once it has ended: after a successful HangReport_Stop. Otherwise empty.
    if (s_thread != nullptr) return HangReportResult{};
    if (s_last_result_count.load(std::memory_order_acquire) == 0) return HangReportResult{};
    return s_last_result;
}

uint32_t HangReport_AutomaticReportCount() noexcept {
    return s_last_result_count.load(std::memory_order_acquire);
}

int HangReport_EffectiveProtection() noexcept {
    return s_effective_protection.load(std::memory_order_acquire);
}

const char* HangReport_TextPath(uint32_t index) noexcept {
    return index >= 1 && index <= kHangMaxReports && s_paths_ready ? s_text_path_a[index - 1] : "";
}

const char* HangReport_DumpPath(uint32_t index) noexcept {
    return index >= 1 && index <= kHangMaxReports && s_paths_ready ? s_dump_path_a[index - 1] : "";
}

void HangReport_SetDumpTypeForTest(uint32_t type) noexcept {
    s_dump_type = type;
}

void HangReport_SetLogLevelDelayForTest(uint32_t seconds) noexcept {
    s_floor_delay_s = seconds;
}

void HangReport_ResetPolicyForTest() noexcept {
    s_policy_state = HangPolicyState{};
    s_last_result = HangReportResult{};
    s_last_result_count.store(0, std::memory_order_release);
}

uint32_t HangReport_ParseSystemInformationForTest(const void* buffer, uint32_t length, uint32_t pid, uint32_t* thread_ids,
                                                  uint32_t capacity, uint32_t* count) noexcept {
    ThreadState states[1024];
    size_t found = 0;
    const SystemInfoState verdict = ParseProcessInformation(static_cast<const uint8_t*>(buffer), length, pid, states,
                                                            capacity < 1024 ? capacity : 1024, &found);
    for (size_t i = 0; i < found; ++i) thread_ids[i] = states[i].tid;
    *count = static_cast<uint32_t>(found);
    return static_cast<uint32_t>(verdict);
}
