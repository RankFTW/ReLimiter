#pragma once

// Render handoff publication: the render thread's PRESENT_END of the
// presenting device, the moment it left a frame's Present. The FG input wait's
// late-frame spacing asks whether it has (gpu_present_gate_d3d12.cpp).
// Lock-free; the publisher never blocks.

#include <cstdint>

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
