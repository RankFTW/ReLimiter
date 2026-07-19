#pragma once

#include "csv_writer.h"

// Starts the optional live-debug worker after the first valid Present. The
// function is idempotent and must not be called from DllMain.
void LiveDebug_StartIfReady() noexcept;

// Orderly shutdown for AddonUninit. Disables producers before joining.
void LiveDebug_Stop() noexcept;

// Loader-lock-safe notification only; never joins or closes worker handles.
void LiveDebug_NotifyProcessDetach() noexcept;

// Fixed-copy, drop-on-full producer calls. Neither function allocates, locks,
// performs IPC, or waits.
void LiveDebug_PushScheduler(const FrameRow& row) noexcept;
void LiveDebug_PushNativePcl(const NativePclRow& row) noexcept;