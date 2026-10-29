#pragma once
// Developer queue priority probe (gpu_priority_probe, 64-bit, from the next
// launch): an experiment towards a full pacer for frame generation that
// presents its own frames. Two DIRECT queues of ReLimiter's own on the
// presenting device, one HIGH and one NORMAL priority, each with its own
// creator ID, take turns waiting on a fence this probe signals from the CPU.
// The GPU timestamp written right after the wait, minus the signal time, is
// how long that queue's work waited for the GPU; the log reports both every
// 10 s. It shows whether a higher queue priority gets past the game's running
// GPU work (no preemption between equal priorities on the test machine).
#ifdef _WIN64
bool GpuPriorityProbe_Start() noexcept; // no-op unless the key was on at load
bool GpuPriorityProbe_Stop() noexcept;  // releases its waits, then joins; false: resources retained
#else
inline bool GpuPriorityProbe_Start() noexcept { return true; }
inline bool GpuPriorityProbe_Stop() noexcept { return true; }
#endif
