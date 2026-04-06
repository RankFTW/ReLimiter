#pragma once

// Hook NvAPI_QueryInterface to block flip metering (ID 0xF3148C42)
// on pre-Blackwell GPUs. Spec §II.2.

void InstallFlipMeteringHook();
