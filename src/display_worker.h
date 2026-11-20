#pragma once

// The display state worker (display_state.h): applies the driver's answer about the game's own frames off the threads
// that observe it. The observers are the game's NvAPI_D3D_GetSleepStatus calls and ReLimiter's read in the Reflex
// Sleep, both on game threads (nvapi_hooks.cpp), and applying a changed answer can start or stop the vblank worker,
// which may wait for a vblank. The thread waits on one event and wakes only when the answer changes. Lifecycle calls
// are serialized and run outside the loader lock; Stop joins the thread (false: it did not stop in time and its
// resources are retained). 64-bit only, like the NvAPI hooks that feed it.

#ifdef _WIN64
void DisplayWorker_Start();
bool DisplayWorker_Stop();
// Any thread, any time (also before Start and after Stop: a started worker then applies the newest answer at once).
void DisplayWorker_Notify() noexcept;
#else
inline void DisplayWorker_Start() {}
inline bool DisplayWorker_Stop() { return true; }
inline void DisplayWorker_Notify() noexcept {}
#endif
