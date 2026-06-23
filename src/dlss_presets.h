#pragma once

// DLSS preset override query via NvAPI_NGX_GetNGXOverrideState.
// Polls every 5 seconds. Returns preset letters for SR/RR/FG.

struct DLSSPresets {
    char sr[4];  // e.g. "M", "-", "A"
    char rr[4];
    char fg[4];
    bool available;
    int mfg_generation_factor;  // DRS 0x104D6667: 0=app-controlled, 1=2x, 2=3x, 3=4x, 4=5x, 5=6x
    int mfg_mode_override;      // DRS 0x10308298: 0=Off, 2=Fixed, 4=Dynamic
    int mfg_dynamic_target_fps; // DRS 0x10CF4125: 0=Off, 0x01000000=Max Refresh, or raw FPS
};

void DLSSPresets_Init();
void DLSSPresets_Poll();  // Call periodically (e.g. from OSD draw)
DLSSPresets DLSSPresets_Get();
