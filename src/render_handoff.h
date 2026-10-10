#pragma once

// Render handoff (phase4_design.md section 23): the newest RENDERSUBMIT_START
// of the presenting device, the moment the game's render thread took a frame.
// The render handoff limit releases a frame that is not held no earlier than
// the handoff of the newest released frame plus a learned offset, so the game
// cannot simulate frames that only wait for its render thread.
// Lock-free; the publisher never blocks.

#include <cstdint>

constexpr double RENDER_HANDOFF_ACTIVITY_S = 1.0;      // a handoff this recent: the stream is live
// A wait ends when the render thread took no frame for this many grid
// intervals (the 3 I outlier bound of the lead): a stall, or a render thread
// that waits for the next release.
constexpr double RENDER_HANDOFF_STALL_INTERVALS = 3.0;

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

// Waits, without spinning, until the handoff of frame_id or a later frame of
// (domain, producer epoch) is published, or until the render thread took no
// frame for stall_ticks (from the start of the wait or its newest handoff,
// whichever is later). *handoff_qpc receives the handoff's QPC when Ready.
RenderHandoffWait RenderHandoff_WaitFor(uint8_t domain, uint32_t producer_epoch, uint64_t frame_id,
    int64_t stall_ticks, int64_t* handoff_qpc) noexcept;
