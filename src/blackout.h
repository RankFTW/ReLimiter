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

// ── OLED Care Mode ──
// Blacks out ALL monitors (including game's primary) and reduces FPS to 20.
// Auto-deactivates on focus loss. Never persists active state.

// Toggle OLED Care on/off. Safe to call from any thread.
void OLEDCare_Toggle();

// Explicitly deactivate OLED Care. Safe to call from any thread.
void OLEDCare_Deactivate();

// Returns true if OLED Care is currently active.
bool OLEDCare_IsActive();
