/**
 * Mip_Corrector — Hooks ID3D12Device::CreateSampler to apply corrected mip LOD bias.
 *
 * Applies log2(s × k) bias correction to samplers with non-zero MipLODBias,
 * leaving zero-bias samplers unchanged. Logs warnings for static samplers
 * in root signatures that cannot be corrected at runtime.
 *
 * Feature: adaptive-dlss-scaling
 */

#pragma once
#include <d3d12.h>

// Install CreateSampler vtable hook on the DX12 device.
void MipCorrector_Init(ID3D12Device* device);

// Remove hook.
void MipCorrector_Shutdown();

// Update the mip bias correction value. Called on tier transition.
// new_bias = log2(s × k)
void MipCorrector_Update(double new_bias);

// Get current applied bias (for OSD/telemetry).
double MipCorrector_GetBias();
