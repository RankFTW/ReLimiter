#pragma once

// Event-driven frame-splitting worker. It restores its driver override
// on every exit, after its final in-flight update. Lifecycle calls are serialized
// and must run outside the loader lock. False from Stop retains ownership and
// requires keeping the addon and worker dependencies alive; do not restore from
// the stopping thread after a timeout.

#ifdef _WIN64
void StartDisplayPollThread();
bool StopDisplayPollThread();
#else
inline void StartDisplayPollThread() {}
inline bool StopDisplayPollThread() { return true; }
#endif
