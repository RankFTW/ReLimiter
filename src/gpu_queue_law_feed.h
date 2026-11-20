#pragma once

// The probe's finished per-frame records on their way to the unified pacer's queue law (developer
// key unified_queue_law; unified_queue_law.h). A bounded wait-free single-producer single-consumer
// ring of 64 PacingGpuQueueRecord: the producer is the probe's completion thread (it calls Push
// where it pushes the record to the telemetry), the consumer is the scheduler owner (it drains
// the ring before it plans). A full ring drops the new record and counts it; nothing blocks.
// No Windows, logger or configuration dependencies.
//
// Record freshness. A record is pushed when the probe finishes its frame (the frame's last
// submission completed, or the frame was emitted incomplete), so the newest record the owner pops
// at a plan describes a frame one to two frames older than the one being planned.

#include "pacing_telemetry_types.h"

#include <cstdint>

// Producer thread only (one thread at a time).
void GpuQueueLawFeed_Push(const PacingGpuQueueRecord& record) noexcept;
// Consumer (scheduler owner) only: the oldest record, false when empty.
bool GpuQueueLawFeed_Pop(PacingGpuQueueRecord* out) noexcept;
// Records refused because the ring was full, cumulative (any thread).
uint64_t GpuQueueLawFeed_Dropped() noexcept;
// Consumer only: discards every record published so far (a toggle or a restart must not carry
// stale records). Records pushed concurrently afterwards are kept. The drop count is kept.
void GpuQueueLawFeed_Reset() noexcept;
// The probe's adopted queue was destroyed: the session (PacingGpuQueueRecord::session_generation) is
// retired. ANY thread (the producer is one thread, a queue's destruction runs on another), wait-free;
// the value only grows. The consumer reads it before it drains and retires the law's session
// (UnifiedPacer::RetireQueueSession), so records of that session still in the ring are dropped.
void GpuQueueLawFeed_RetireSession(uint32_t session) noexcept;
// The newest retired session (0 = none); consumer.
uint64_t GpuQueueLawFeed_RetiredSession() noexcept;
