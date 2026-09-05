#pragma once

// Two-second frame-splitting poll loop. The worker restores its driver override
// on every exit, after its final in-flight update. Lifecycle calls are serialized
// and must run outside the loader lock. False from Stop retains ownership and
// requires keeping the addon and worker dependencies alive; do not restore from
// the stopping thread after a timeout.

void StartDisplayPollThread();
bool StopDisplayPollThread();
