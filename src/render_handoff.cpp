#include "render_handoff.h"

#include <Windows.h>

#include <atomic>

namespace
{
    // Newest PRESENT_END: its own seqlock (odd while a publication writes the
    // fields), so a reader never pairs one stream with another stream's frame.
    std::atomic<uint64_t> s_present_sequence{0};
    std::atomic<uint32_t> s_present_stream{0}; // domain << 24 | producer epoch (low 24 bits)
    std::atomic<uint64_t> s_present_frame_id{0};
    std::atomic<int64_t> s_present_qpc{0};
    std::atomic_flag s_present_writing = ATOMIC_FLAG_INIT;

    uint32_t StreamOf(uint8_t domain, uint32_t producer_epoch) noexcept
    {
        return (static_cast<uint32_t>(domain) << 24) | (producer_epoch & 0x00FFFFFFu);
    }
}

void RenderHandoff_PublishPresentEnd(uint8_t domain, uint32_t producer_epoch, uint64_t frame_id,
    int64_t qpc) noexcept
{
    if (s_present_writing.test_and_set(std::memory_order_acquire)) return;
    const uint64_t sequence = s_present_sequence.load(std::memory_order_relaxed);
    s_present_sequence.store(sequence + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    s_present_stream.store(StreamOf(domain, producer_epoch), std::memory_order_relaxed);
    s_present_frame_id.store(frame_id, std::memory_order_relaxed);
    s_present_qpc.store(qpc, std::memory_order_relaxed);
    s_present_sequence.store(sequence + 2, std::memory_order_release);
    s_present_writing.clear(std::memory_order_release);
}

namespace
{
    // A consistent PRESENT_END snapshot; false when none was published yet.
    bool ReadPresentEnd(uint32_t* stream, uint64_t* frame_id, int64_t* qpc) noexcept
    {
        for (;;) {
            const uint64_t before = s_present_sequence.load(std::memory_order_acquire);
            if (before == 0) return false;
            if (before & 1u) {
                YieldProcessor();
                continue;
            }
            *stream = s_present_stream.load(std::memory_order_relaxed);
            *frame_id = s_present_frame_id.load(std::memory_order_relaxed);
            *qpc = s_present_qpc.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (s_present_sequence.load(std::memory_order_relaxed) == before) return true;
        }
    }
}

bool RenderHandoff_LeftPresent(uint8_t domain, uint32_t producer_epoch, uint64_t frame_id) noexcept
{
    uint32_t stream = 0;
    uint64_t newest = 0;
    int64_t qpc = 0;
    return ReadPresentEnd(&stream, &newest, &qpc) && stream == StreamOf(domain, producer_epoch) &&
        newest >= frame_id;
}
