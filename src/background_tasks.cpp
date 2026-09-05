#include "background_tasks.h"

#include "runtime_lifecycle.h"

#include <array>

namespace
{
    constexpr size_t kMaximumBackgroundTasks = 32;
    std::array<HANDLE, kMaximumBackgroundTasks> s_tasks{};
    SRWLOCK s_task_lock = SRWLOCK_INIT;

    void ReapCompletedTasks() noexcept
    {
        for (HANDLE& task : s_tasks) {
            if (task && WaitForSingleObject(task, 0) == WAIT_OBJECT_0) {
                CloseHandle(task);
                task = nullptr;
            }
        }
    }
}

bool BackgroundTasks_Start(
    BackgroundTaskProc procedure,
    void* context) noexcept
{
    if (!procedure || RuntimeLifecycle_IsStopping()) return false;

    AcquireSRWLockExclusive(&s_task_lock);
    ReapCompletedTasks();
    if (RuntimeLifecycle_IsStopping()) {
        ReleaseSRWLockExclusive(&s_task_lock);
        return false;
    }

    HANDLE* available = nullptr;
    for (HANDLE& task : s_tasks) {
        if (!task) {
            available = &task;
            break;
        }
    }
    if (!available) {
        ReleaseSRWLockExclusive(&s_task_lock);
        return false;
    }

    HANDLE task = CreateThread(nullptr, 0, procedure, context, 0, nullptr);
    if (task) *available = task;
    ReleaseSRWLockExclusive(&s_task_lock);
    return task != nullptr;
}

bool BackgroundTasks_Shutdown(DWORD timeout_ms) noexcept
{
    std::array<HANDLE, kMaximumBackgroundTasks> tasks{};
    AcquireSRWLockExclusive(&s_task_lock);
    tasks = s_tasks;
    s_tasks.fill(nullptr);
    ReleaseSRWLockExclusive(&s_task_lock);

    const ULONGLONG start = GetTickCount64();
    bool all_stopped = true;
    for (HANDLE& task : tasks) {
        if (!task) continue;
        DWORD remaining = timeout_ms;
        if (timeout_ms != INFINITE) {
            const ULONGLONG elapsed = GetTickCount64() - start;
            remaining = elapsed >= timeout_ms
                ? 0
                : static_cast<DWORD>(timeout_ms - elapsed);
        }
        if (WaitForSingleObject(task, remaining) == WAIT_OBJECT_0) {
            CloseHandle(task);
            task = nullptr;
        }
        else {
            all_stopped = false;
        }
    }

    if (!all_stopped) {
        AcquireSRWLockExclusive(&s_task_lock);
        for (HANDLE task : tasks) {
            if (!task) continue;
            for (HANDLE& slot : s_tasks) {
                if (!slot) {
                    slot = task;
                    break;
                }
            }
        }
        ReleaseSRWLockExclusive(&s_task_lock);
    }
    return all_stopped;
}
