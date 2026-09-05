#pragma once

// Monitor blackout — covers all non-game monitors with black windows.
// Toggle via keybind or UI button. Windows are topmost, no-activate,
// tool windows (hidden from Alt+Tab).

// Initialize the blackout system. Call once during addon init.
void Blackout_Init();

// Shut down — destroys any active blackout windows and stops the thread.
bool Blackout_Shutdown();

// Toggle blackout on/off. Safe to call from any thread.
void Blackout_Toggle();

// Set blackout state explicitly. Safe to call from any thread.
void Blackout_SetActive(bool active);

// Returns true if blackout windows are currently showing.
bool Blackout_IsActive();

// OLED Care uses the same worker/lifetime, independently of secondary blackout.
// Requests are asynchronous and thread-safe. Toggle/SetActive are manual;
// only sessions started by the idle timer dismiss on ordinary input. Callers
// must keep manual keybind/UI polling alive even while rendering is capped.
void OLEDCare_Toggle();
void OLEDCare_SetActive(bool active);
// True only after the worker has created the requested covering windows.
bool OLEDCare_IsActive();
// Defaults: all monitors, idle disabled. Minutes are clamped to 0..120.
// Safe before Init and from config/UI threads; no mutable config is read by
// the worker. Disabling idle dismisses automatic sessions, not manual ones.
void OLEDCare_SetConfig(bool all_monitors, int idle_minutes);
