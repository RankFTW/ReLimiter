#pragma once

// 2-second poll loop: PollGSyncState, QueryVRRCeiling, QueryVRRFloor,
// ManageFrameSplitting. Runs at THREAD_PRIORITY_NORMAL.

void StartDisplayPollThread();
void StopDisplayPollThread();
