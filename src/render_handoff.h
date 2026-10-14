#pragma once

// Render handoff: the newest RENDERSUBMIT_START
// of the presenting device, the moment the game's render thread took a frame.
// The render handoff limit releases a frame that is not held no earlier than
// the handoff of the newest released frame plus a learned offset, so the game
// cannot simulate frames that only wait for its render thread.
// Lock-free; the publisher never blocks.

#include <cstdint>

constexpr double RENDER_HANDOFF_ACTIVITY_S = 1.0;      // a handoff this recent: the stream is live
// A wait ends when the render thread took no frame for this many of its own
// frame times (the median interval between its newest handoffs), and never
// sooner than this many grid intervals: a hitch, or a render thread that
// waits for the next release. Below the cap the render thread's frame time
// is longer than the grid interval, so grid intervals alone would call its
// ordinary frames stalls.
constexpr double RENDER_HANDOFF_STALL_INTERVALS = 3.0;
// Handoff intervals of consecutive frames that set the render thread's frame
// time, and the fewest that give one.
constexpr uint32_t RENDER_HANDOFF_CYCLE_WINDOW = 32;
constexpr uint32_t RENDER_HANDOFF_CYCLE_MIN = 8;
// Stalled waits in a row that turn the limit off while the game has not yet
// shown its render thread taking a frame before the next release. A render
// thread that waits for the next release stalls every wait; a hitch rarely
// spans two waits.
constexpr uint32_t RENDER_HANDOFF_BLOCK_STALLS = 2;

enum class RenderHandoffWait : uint8_t {
    Ready = 0,       // the handoff of the frame (or a later one) is published
    Deadline = 1,    // no handoff of any frame for stall_ticks
    OtherStream = 2  // the newest handoff belongs to another domain or producer epoch
};

// Render thread (marker hooks): publishes the handoff of (domain, producer
// epoch, frame) at qpc and wakes waiters. A publication racing another is
// dropped.
void RenderHandoff_Publish(uint8_t domain, uint32_t producer_epoch, uint64_t frame_id, int64_t qpc) noexcept;

// Newest published handoff of (domain, producer epoch); false when none was
// published for it.
bool RenderHandoff_Latest(uint8_t domain, uint32_t producer_epoch, uint64_t* frame_id, int64_t* qpc) noexcept;

// The render thread's frame time for (domain, producer epoch): the median of
// the newest RENDER_HANDOFF_CYCLE_WINDOW intervals between handoffs of
// consecutive frames, in QPC ticks, without the handoffs that came after a
// stalled wait for them; 0 before RENDER_HANDOFF_CYCLE_MIN of them or for
// another stream.
int64_t RenderHandoff_CycleTicks(uint8_t domain, uint32_t producer_epoch) noexcept;

// Waits, without spinning, until the handoff of frame_id or a later frame of
// (domain, producer epoch) is published, or until the render thread took no
// frame for the stall span (from the start of the wait or its newest handoff,
// whichever is later). The span is RENDER_HANDOFF_STALL_INTERVALS render
// thread frame times, at least min_stall_ticks. *handoff_qpc receives the
// handoff's QPC when Ready.
RenderHandoffWait RenderHandoff_WaitFor(uint8_t domain, uint32_t producer_epoch, uint64_t frame_id,
    int64_t min_stall_ticks, int64_t* handoff_qpc) noexcept;

// Render thread (marker hooks): the render thread left the Present of (domain,
// producer epoch, frame) at qpc, its PRESENT_END. A publication racing another
// is dropped.
void RenderHandoff_PublishPresentEnd(uint8_t domain, uint32_t producer_epoch, uint64_t frame_id,
    int64_t qpc) noexcept;

// True once a PRESENT_END of frame_id or a later frame of (domain, producer
// epoch) is published: the render thread has left that frame's Present.
// False while it has not, and when the newest PRESENT_END belongs to another
// stream or none was published.
bool RenderHandoff_LeftPresent(uint8_t domain, uint32_t producer_epoch, uint64_t frame_id) noexcept;

// Newest published PRESENT_END of (domain, producer epoch) and its QPC (the
// render handoff GPU floor's real-time bound); false when none was published
// for it.
bool RenderHandoff_LatestPresentEnd(uint8_t domain, uint32_t producer_epoch, uint64_t* frame_id,
    int64_t* qpc) noexcept;
