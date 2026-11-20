#pragma once

#include "hang_report_state.h"

#include <atomic>
#include <cstdint>

// Who holds a lock and who waits for it, published in plain atomics so a watchdog can say it when the process is
// stuck (the stall report, hang_report.h). The lock itself is never touched: a holder stores one word after it
// acquired and clears it before it releases; a thread that is about to block stores its word and the time first and
// leaves the waiter slot when it got the lock. Nothing here allocates, locks, logs or waits. With the report off
// (hang_report_state.h) every note is one relaxed load of the report's flag and a branch that returns.
//
// A word is the thread id in the low 32 bits and a nonzero "what" (the callback or caller, a small enum of the lock's
// owner) above it; zero means nobody. One holder slot and one waiter slot: with several waiters the slot shows the
// last one that arrived (a waiter clears the slot only when it still holds its own word).

struct LockOwnerSnapshot {
    uint32_t holder_thread = 0;
    uint8_t holder_what = 0;
    uint64_t holder_qpc = 0;  // 0: not recorded (the present event's hold stores no time)
    uint32_t waiter_thread = 0;
    uint8_t waiter_what = 0;
    uint64_t waiter_qpc = 0;
};

class LockOwnerDiag {
public:
    // About to block on the lock.
    void NoteWaiting(uint32_t thread, uint8_t what, uint64_t qpc) noexcept {
        if (!HangReport_Active()) return;
        waiter_qpc_.store(qpc, std::memory_order_relaxed);
        waiter_.store(Pack(thread, what), std::memory_order_release);
    }
    // The lock is held (one store: the hot path of a try-acquired hold).
    void NoteHolding(uint32_t thread, uint8_t what) noexcept {
        if (!HangReport_Active()) return;
        holder_.store(Pack(thread, what), std::memory_order_release);
    }
    // The lock is held after an acquire that may have blocked: leaves the waiter slot (when it is this thread's)
    // and records the time of the hold.
    void NoteHoldingAfterWait(uint32_t thread, uint8_t what, uint64_t qpc) noexcept {
        if (!HangReport_Active()) return;
        uint64_t mine = waiter_.load(std::memory_order_relaxed);
        if ((mine & 0xFFFFFFFFull) == thread) waiter_.compare_exchange_strong(mine, 0, std::memory_order_release);
        holder_qpc_.store(qpc, std::memory_order_relaxed);
        holder_.store(Pack(thread, what, true), std::memory_order_release);
    }
    // Before the release.
    void NoteReleased() noexcept {
        if (!HangReport_Active()) return;
        holder_.store(0, std::memory_order_release);
    }

    LockOwnerSnapshot Read() const noexcept {
        LockOwnerSnapshot out;
        const uint64_t holder = holder_.load(std::memory_order_acquire);
        out.holder_qpc = holder_qpc_.load(std::memory_order_relaxed);
        const uint64_t waiter = waiter_.load(std::memory_order_acquire);
        out.waiter_qpc = waiter_qpc_.load(std::memory_order_relaxed);
        out.holder_thread = static_cast<uint32_t>(holder & 0xFFFFFFFFull);
        out.holder_what = static_cast<uint8_t>(holder >> 32);
        out.waiter_thread = static_cast<uint32_t>(waiter & 0xFFFFFFFFull);
        out.waiter_what = static_cast<uint8_t>(waiter >> 32);
        if (!holder || !(holder & kTimed)) out.holder_qpc = 0;
        if (!waiter) out.waiter_qpc = 0;
        return out;
    }

private:
    // Bit 40 of the holder word: the hold's time was recorded (a hold stored with NoteHolding carries none, and a
    // time left over from an earlier hold must not be read as its).
    static constexpr uint64_t kTimed = 1ull << 40;
    static uint64_t Pack(uint32_t thread, uint8_t what, bool timed = false) noexcept {
        return static_cast<uint64_t>(thread) | (static_cast<uint64_t>(what ? what : 1) << 32) | (timed ? kTimed : 0);
    }
    std::atomic<uint64_t> holder_{0};
    std::atomic<uint64_t> waiter_{0};
    std::atomic<uint64_t> holder_qpc_{0};
    std::atomic<uint64_t> waiter_qpc_{0};
};

// The swap manager's selection lock: who takes it (the present event try-acquires, the lifecycle callbacks block).
enum class SelectionLockUser : uint8_t {
    None = 0,
    PresentEvent,
    InitSwapchain,
    DestroySwapchain,
    InitDevice,
    DestroyDevice,
    ReleaseRuntimeReferences,
};
inline const char* SelectionLockUserName(uint8_t what) noexcept {
    switch (static_cast<SelectionLockUser>(what)) {
    case SelectionLockUser::None: return "none";
    case SelectionLockUser::PresentEvent: return "present event";
    case SelectionLockUser::InitSwapchain: return "init_swapchain";
    case SelectionLockUser::DestroySwapchain: return "destroy_swapchain";
    case SelectionLockUser::InitDevice: return "init_device";
    case SelectionLockUser::DestroyDevice: return "destroy_device";
    case SelectionLockUser::ReleaseRuntimeReferences: return "release runtime references";
    }
    return "unknown";
}

// The hook install lock (hooks.cpp): who takes it.
enum class InstallLockUser : uint8_t {
    None = 0,
    InstallGuard,       // HookInstallGuard: an installer, batch or a site's own guard
    ResetAdmission,
    ShutdownBarrier,    // Hooks_BeginShutdown's round trip
    TeardownDisable,    // DisableAllHooks / DisableAllHooksExcept
    ForgetTarget,
};
inline const char* InstallLockUserName(uint8_t what) noexcept {
    switch (static_cast<InstallLockUser>(what)) {
    case InstallLockUser::None: return "none";
    case InstallLockUser::InstallGuard: return "install guard";
    case InstallLockUser::ResetAdmission: return "reset admission";
    case InstallLockUser::ShutdownBarrier: return "shutdown barrier";
    case InstallLockUser::TeardownDisable: return "teardown disable";
    case InstallLockUser::ForgetTarget: return "forget target";
    }
    return "unknown";
}
