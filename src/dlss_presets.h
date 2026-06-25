#pragma once

// DLSS preset override query via NvAPI_NGX_GetNGXOverrideState.
// Polls every 5 seconds. Returns preset letters for SR/RR/FG.

struct DLSSPresets {
    char sr[8];  // e.g. "M", "-", "A", "Auto"
    char rr[8];
    char fg[8];
    bool available;
    int mfg_generation_factor;  // DRS 0x104D6667: 0=app-controlled, 1=2x, 2=3x, 3=4x, 4=5x, 5=6x
    int mfg_mode_override;      // DRS 0x10308298: 0=Off, 2=Fixed, 4=Dynamic
    int mfg_dynamic_target_fps; // DRS 0x10CF4125: 0=Off, 0x01000000=Max Refresh, or raw FPS
};

void DLSSPresets_Init();
void DLSSPresets_Poll();  // Call periodically (e.g. from OSD draw)
DLSSPresets DLSSPresets_Get();

// Write the Dynamic MFG Target FPS to the game's driver profile.
// fps=0 means Off, fps>0 writes the raw FPS value to DRS 0x10CF4125.
// Returns true on success.
bool DLSSPresets_WriteDynamicTargetFPS(int fps);
