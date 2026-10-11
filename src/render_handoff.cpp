#include "render_handoff.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>

namespace
{
    // Seqlock: odd while a publication writes the fields. Waiters sleep on
    // the sequence with WaitOnAddress; every publication wakes them.
    std::atomic<uint64_t> s_sequence{0};
    std::atomic<uint32_t> s_stream{0}; // domain << 24 | producer epoch (low 24 bits)
    std::atomic<uint64_t> s_frame_id{0};
    std::atomic<int64_t> s_qpc{0};
    std::atomic_flag s_writing = ATOMIC_FLAG_INIT;

    // The render thread's frame time. Written by the publisher holding
    // s_writing; waiters read the two atomics (a stale pair only shortens or
    // lengthens one stall span).
    std::atomic<uint32_t> s_cycle_stream{0};
    std::atomic<int64_t> s_cycle_ticks{0};
    uint32_t s_interval_stream = 0;
    uint64_t s_previous_frame = 0;
    int64_t s_previous_qpc = 0;
    std::array<int64_t, RENDER_HANDOFF_CYCLE_WINDOW> s_intervals{};
    uint32_t s_interval_count = 0;
    uint32_t s_interval_next = 0;
    // The newest wait that stalled: its stream and frame. Handoffs of that
    // frame and earlier ones came after the stall, so their intervals hold the
    // wait, not the render thread's frame time (a render thread that waits
    // for the next release would otherwise lengthen its own stall span).
    std::atomic<uint32_t> s_stalled_stream{0};
    std::atomic<uint64_t> s_stalled_frame{0};

    // Newest PRESENT_END: its own seqlock, so a reader never pairs one
    // stream with another stream's frame.
    std::atomic<uint64_t> s_present_sequence{0};
    std::atomic<uint32_t> s_present_stream{0};
    std::atomic<uint64_t> s_present_frame_id{0};
    std::atomic_flag s_present_writing = ATOMIC_FLAG_INIT;

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

    // Publisher only (s_writing held): the interval to the previous handoff
    // when both are consecutive frames of one stream and no stalled wait was
    // for this frame or a later one; the median of the window, lower-middle
    // element for an even count.
    void RecordInterval(uint32_t stream, uint64_t frame_id, int64_t qpc) noexcept
    {
        if (stream != s_interval_stream) {
            s_interval_stream = stream;
            s_interval_count = 0;
            s_interval_next = 0;
            s_cycle_ticks.store(0, std::memory_order_relaxed);
        } else if (frame_id == s_previous_frame + 1 && qpc > s_previous_qpc &&
                   (s_stalled_stream.load(std::memory_order_relaxed) != stream ||
                    frame_id > s_stalled_frame.load(std::memory_order_relaxed))) {
            s_intervals[s_interval_next] = qpc - s_previous_qpc;
            s_interval_next = (s_interval_next + 1) % RENDER_HANDOFF_CYCLE_WINDOW;
            if (s_interval_count < RENDER_HANDOFF_CYCLE_WINDOW) ++s_interval_count;
            if (s_interval_count >= RENDER_HANDOFF_CYCLE_MIN) {
                std::array<int64_t, RENDER_HANDOFF_CYCLE_WINDOW> sorted = s_intervals;
                const auto middle = sorted.begin() + (s_interval_count - 1) / 2;
                std::nth_element(sorted.begin(), middle, sorted.begin() + s_interval_count);
                s_cycle_ticks.store(*middle, std::memory_order_relaxed);
            }
        }
        s_previous_frame = frame_id;
        s_previous_qpc = qpc;
        s_cycle_stream.store(stream, std::memory_order_relaxed);
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
    RecordInterval(StreamOf(domain, producer_epoch), frame_id, qpc);
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

int64_t RenderHandoff_CycleTicks(uint8_t domain, uint32_t producer_epoch) noexcept
{
    if (s_cycle_stream.load(std::memory_order_relaxed) != StreamOf(domain, producer_epoch)) return 0;
    return s_cycle_ticks.load(std::memory_order_relaxed);
}

void RenderHandoff_PublishPresentEnd(uint8_t domain, uint32_t producer_epoch, uint64_t frame_id) noexcept
{
    if (s_present_writing.test_and_set(std::memory_order_acquire)) return;
    const uint64_t sequence = s_present_sequence.load(std::memory_order_relaxed);
    s_present_sequence.store(sequence + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    s_present_stream.store(StreamOf(domain, producer_epoch), std::memory_order_relaxed);
    s_present_frame_id.store(frame_id, std::memory_order_relaxed);
    s_present_sequence.store(sequence + 2, std::memory_order_release);
    s_present_writing.clear(std::memory_order_release);
}

bool RenderHandoff_LeftPresent(uint8_t domain, uint32_t producer_epoch, uint64_t frame_id) noexcept
{
    for (;;) {
        const uint64_t before = s_present_sequence.load(std::memory_order_acquire);
        if (before == 0) return false;
        if (before & 1u) {
            YieldProcessor();
            continue;
        }
        const uint32_t stream = s_present_stream.load(std::memory_order_relaxed);
        const uint64_t newest = s_present_frame_id.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (s_present_sequence.load(std::memory_order_relaxed) == before)
            return stream == StreamOf(domain, producer_epoch) && newest >= frame_id;
    }
}

RenderHandoffWait RenderHandoff_WaitFor(uint8_t domain, uint32_t producer_epoch, uint64_t frame_id,
    int64_t min_stall_ticks, int64_t* handoff_qpc) noexcept
{
    const uint32_t stream = StreamOf(domain, producer_epoch);
    const int64_t frequency = QpcFrequency();
    const int64_t cycle_ticks = RenderHandoff_CycleTicks(domain, producer_epoch);
    const int64_t stall_ticks = min_stall_ticks <= 0 ? 0 : (std::max)(min_stall_ticks,
        static_cast<int64_t>(std::llround(RENDER_HANDOFF_STALL_INTERVALS * static_cast<double>(cycle_ticks))));
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
        if (frequency <= 0 || stall_ticks <= 0 || now >= deadline_qpc) {
            s_stalled_frame.store(frame_id, std::memory_order_relaxed);
            s_stalled_stream.store(stream, std::memory_order_relaxed);
            return RenderHandoffWait::Deadline;
        }
        // Whole milliseconds, at least one; the loop re-checks the deadline.
        const int64_t remaining_ms = ((deadline_qpc - now) * 1000 + frequency - 1) / frequency;
        uint64_t observed = snapshot.sequence;
        WaitOnAddress(&s_sequence, &observed, sizeof(observed),
            static_cast<DWORD>(remaining_ms < 1 ? 1 : remaining_ms));
    }
}
