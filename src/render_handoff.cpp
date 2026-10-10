#include "render_handoff.h"

#include <Windows.h>

#include <atomic>

namespace
{
    // Seqlock: odd while a publication writes the fields. Waiters sleep on
    // the sequence with WaitOnAddress; every publication wakes them.
    std::atomic<uint64_t> s_sequence{0};
    std::atomic<uint32_t> s_stream{0}; // domain << 24 | producer epoch (low 24 bits)
    std::atomic<uint64_t> s_frame_id{0};
    std::atomic<int64_t> s_qpc{0};
    std::atomic_flag s_writing = ATOMIC_FLAG_INIT;

    uint32_t StreamOf(uint8_t domain, uint32_t producer_epoch) noexcept
    {
        return (static_cast<uint32_t>(domain) << 24) | (producer_epoch & 0x00FFFFFFu);
    }

    struct Snapshot {
        uint64_t sequence = 0;
        uint32_t stream = 0;
        uint64_t frame_id = 0;
        int64_t qpc = 0;
    };

    // A consistent snapshot; false when nothing was published yet.
    bool Read(Snapshot* snapshot) noexcept
    {
        for (;;) {
            const uint64_t before = s_sequence.load(std::memory_order_acquire);
            if (before == 0) return false;
            if (before & 1u) {
                YieldProcessor();
                continue;
            }
            snapshot->stream = s_stream.load(std::memory_order_relaxed);
            snapshot->frame_id = s_frame_id.load(std::memory_order_relaxed);
            snapshot->qpc = s_qpc.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (s_sequence.load(std::memory_order_relaxed) == before) {
                snapshot->sequence = before;
                return true;
            }
        }
    }

    int64_t QpcNow() noexcept
    {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        return now.QuadPart;
    }

    int64_t QpcFrequency() noexcept
    {
        static const int64_t frequency = [] {
            LARGE_INTEGER value{};
            QueryPerformanceFrequency(&value);
            return value.QuadPart;
        }();
        return frequency;
    }
}

void RenderHandoff_Publish(uint8_t domain, uint32_t producer_epoch, uint64_t frame_id, int64_t qpc) noexcept
{
    if (qpc <= 0 || s_writing.test_and_set(std::memory_order_acquire)) return;
    const uint64_t sequence = s_sequence.load(std::memory_order_relaxed);
    s_sequence.store(sequence + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    s_stream.store(StreamOf(domain, producer_epoch), std::memory_order_relaxed);
    s_frame_id.store(frame_id, std::memory_order_relaxed);
    s_qpc.store(qpc, std::memory_order_relaxed);
    s_sequence.store(sequence + 2, std::memory_order_release);
    s_writing.clear(std::memory_order_release);
    WakeByAddressAll(&s_sequence);
}

bool RenderHandoff_Latest(uint8_t domain, uint32_t producer_epoch, uint64_t* frame_id, int64_t* qpc) noexcept
{
    Snapshot snapshot{};
    if (!Read(&snapshot) || snapshot.stream != StreamOf(domain, producer_epoch)) return false;
    if (frame_id != nullptr) *frame_id = snapshot.frame_id;
    if (qpc != nullptr) *qpc = snapshot.qpc;
    return true;
}

RenderHandoffWait RenderHandoff_WaitFor(uint8_t domain, uint32_t producer_epoch, uint64_t frame_id,
    int64_t stall_ticks, int64_t* handoff_qpc) noexcept
{
    const uint32_t stream = StreamOf(domain, producer_epoch);
    const int64_t frequency = QpcFrequency();
    // The render thread's progress: the newest handoff of any frame. Taking
    // the frames queued ahead of frame_id is progress, so a deep queue drains
    // without reaching the deadline.
    int64_t progress_qpc = QpcNow();
    for (;;) {
        Snapshot snapshot{};
        const bool published = Read(&snapshot);
        if (published && snapshot.stream != stream) return RenderHandoffWait::OtherStream;
        if (published && snapshot.frame_id >= frame_id) {
            if (handoff_qpc != nullptr) *handoff_qpc = snapshot.qpc;
            return RenderHandoffWait::Ready;
        }
        if (published && snapshot.qpc > progress_qpc) progress_qpc = snapshot.qpc;
        const int64_t deadline_qpc = progress_qpc + stall_ticks;
        const int64_t now = QpcNow();
        if (frequency <= 0 || stall_ticks <= 0 || now >= deadline_qpc) return RenderHandoffWait::Deadline;
        // Whole milliseconds, at least one; the loop re-checks the deadline.
        const int64_t remaining_ms = ((deadline_qpc - now) * 1000 + frequency - 1) / frequency;
        uint64_t observed = snapshot.sequence;
        WaitOnAddress(&s_sequence, &observed, sizeof(observed),
            static_cast<DWORD>(remaining_ms < 1 ? 1 : remaining_ms));
    }
}
