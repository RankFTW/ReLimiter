#pragma once

#include <Windows.h>

// Focus Lock: prevents the game from detecting focus loss.
// Swallows WM_ACTIVATEAPP/WM_ACTIVATE/WM_KILLFOCUS and hooks
// GetForegroundWindow/GetActiveWindow/GetFocus to return game HWND.

void FocusLock_Install(HWND hwnd);
void FocusLock_Remove();
bool FocusLock_IsActive();

// Returns the REAL foreground window (bypasses our hook).
// Used by the scheduler for background FPS cap detection.
HWND FocusLock_RealGetForegroundWindow();
