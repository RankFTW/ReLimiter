#pragma once

#include <Windows.h>

// The system's display notifications, for the display state (display_state.h). A hidden top-level window on its
// own thread receives display mode and topology changes (WM_DISPLAYCHANGE, WM_DEVICECHANGE with
// DBT_DEVNODES_CHANGED) and the console display turning on (GUID_CONSOLE_DISPLAY_STATE), and reads the G-Sync state
// again for each one (DisplayState_OnDisplayEvent): a display that was asleep at the game's start, was reconnected or
// had G-SYNC switched on while the game ran is recognised without waiting for the game's next swapchain. Nothing else
// wakes the thread. Lifecycle calls are serialized and run outside the loader lock; Stop joins the thread (false: it
// did not stop in time and its resources are retained).
void DisplayEvents_Start(HMODULE module);
bool DisplayEvents_Stop();
