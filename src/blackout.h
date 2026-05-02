#pragma once

// Monitor blackout — covers all non-game monitors with black windows.
// Toggle via keybind or UI button. Windows are topmost, no-activate,
// tool windows (hidden from Alt+Tab).

// Initialize the blackout system. Call once during addon init.
void Blackout_Init();

// Shut down — destroys any active blackout windows and stops the thread.
void Blackout_Shutdown();

// Toggle blackout on/off. Safe to call from any thread.
void Blackout_Toggle();

// Set blackout state explicitly. Safe to call from any thread.
void Blackout_SetActive(bool active);

// Returns true if blackout windows are currently showing.
bool Blackout_IsActive();
