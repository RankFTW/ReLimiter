#pragma once

#include <Windows.h>

#include <cstdint>
#include <memory>

struct EtwStatus;
struct PMTraceConsumer;
struct PMTraceSession;

class NativePclDxgkrnlEventIsolation final
{
public:
    NativePclDxgkrnlEventIsolation(
        PMTraceSession& session,
        PMTraceConsumer& consumer) noexcept;
    ~NativePclDxgkrnlEventIsolation();

    NativePclDxgkrnlEventIsolation(
        const NativePclDxgkrnlEventIsolation&) = delete;
    NativePclDxgkrnlEventIsolation& operator=(
        const NativePclDxgkrnlEventIsolation&) = delete;

    ULONG Initialize() noexcept;
    bool Observe(const EtwStatus& status, ULONGLONG now_ms) noexcept;
    bool AbortAndRestore() noexcept;

private:
    struct Implementation;
    std::unique_ptr<Implementation> implementation_;
};
