/**
 * NGX_Interceptor — Intercepts NVIDIA NGX at the EvaluateFeature level.
 *
 * Instead of faking swapchain dimensions (which crashes games that validate
 * them), we intercept DLSS at the NGX evaluation level:
 *
 *   1. Game renders at s×D (normal DLSS internal resolution)
 *   2. We hook NVSDK_NGX_D3D12_EvaluateFeature — when DLSS runs, we swap
 *      its output target from the game's backbuffer (D) to our intermediate
 *      buffer (k×D)
 *   3. DLSS upscales from s×D to k×D (higher quality than normal)
 *   4. After DLSS completes, we Lanczos downscale from k×D back to the
 *      game's original output buffer (D)
 *   5. Game continues with its backbuffer at D, completely unaware
 *
 * The game never sees fake dimensions. No GetDesc lies. No GetBuffer lies.
 *
 * Also hooks NVSDK_NGX_Parameter::Get for "OutRenderOptimalWidth" and
 * "OutRenderOptimalHeight" to override the values returned to the game,
 * enforcing s × k × D as the DLSS internal render size.
 *
 * Detected via loadlib_hooks.cpp when nvngx_dlss.dll is loaded.
 *
 * Feature: adaptive-dlss-scaling
 */

#pragma once
#include <d3d12.h>
#include <cstdint>

struct NGXInterceptorState {
    double   scale_factor;          // s (e.g. 0.33)
    uint32_t optimal_render_w;      // s × fake_width
    uint32_t optimal_render_h;      // s × fake_height
    bool     active;                // True if NGX interception is working
    bool     ray_reconstruction;    // True if DLSS-RR detected
};

// Initialize NGX hooks. Called from DoInit() after LoadLibrary hooks.
void NGXInterceptor_Init(double scale_factor);

// Shutdown: remove hooks, release intermediate buffer.
void NGXInterceptor_Shutdown();

// Called by K_Controller when fake output resolution changes.
// Updates the optimal render dimensions returned to the game.
void NGXInterceptor_UpdateOutputRes(uint32_t fake_w, uint32_t fake_h);

// Get current state for OSD/telemetry.
NGXInterceptorState NGXInterceptor_GetState();

// Returns true if Ray Reconstruction is detected.
bool NGXInterceptor_IsRayReconstructionActive();

// Called by loadlib_hooks.cpp when nvngx_dlss.dll is loaded.
// Installs the NGX parameter Get vtable hook and EvaluateFeature hook.
void NGXInterceptor_OnDLSSDllLoaded(void* hModule);

// Set the DX12 device for intermediate buffer allocation.
void NGXInterceptor_SetDevice(ID3D12Device* device);

// Set current k value and display dimensions for the EvaluateFeature hook.
void NGXInterceptor_SetScalingParams(double k, uint32_t display_w, uint32_t display_h);
