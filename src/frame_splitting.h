#pragma once

// ManageFrameSplitting: disable frame splitting when FG active + G-Sync,
// re-enable when no longer needed.
// Also: RestoreFrameSplitting for shutdown cleanup.

#ifdef _WIN64
void ManageFrameSplitting();
void RestoreFrameSplitting();
#else
inline void ManageFrameSplitting() {}
inline void RestoreFrameSplitting() {}
#endif
