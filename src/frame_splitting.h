#pragma once

// Display-worker lifecycle only, outside loader lock. Begin is called before
// publishing the worker; it must never reopen admission for a retained worker.
// RequestStop is nonblocking. Restore closes admission, serializes with the last
// driver update, and reports whether the owned display override was restored.
// Manage/Restore are serialized (not suitable for a Present/marker hot path).

#ifdef _WIN64
void BeginFrameSplittingPolling();
void RequestFrameSplittingStop() noexcept;
bool ManageFrameSplitting();
bool RestoreFrameSplitting() noexcept;
#else
inline void BeginFrameSplittingPolling() {}
inline void RequestFrameSplittingStop() noexcept {}
inline bool ManageFrameSplitting() { return true; }
inline bool RestoreFrameSplitting() noexcept { return true; }
#endif
