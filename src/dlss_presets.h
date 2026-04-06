#pragma once

// DLSS preset override query via NvAPI_NGX_GetNGXOverrideState.
// Polls every 5 seconds. Returns preset letters for SR/RR/FG.

struct DLSSPresets {
    char sr[4];  // e.g. "M", "-", "A"
    char rr[4];
    char fg[4];
    bool available;
};

void DLSSPresets_Init();
void DLSSPresets_Poll();  // Call periodically (e.g. from OSD draw)
DLSSPresets DLSSPresets_Get();
