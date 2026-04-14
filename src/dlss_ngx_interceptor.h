/**
 * NGX_Interceptor — Intercepts NVIDIA NGX parameter queries to lock the
 * DLSS internal render ratio at a constant scale factor `s`.
 *
 * Hooks NVSDK_NGX_Parameter::Get for "OutRenderOptimalWidth" and
 * "OutRenderOptimalHeight" to override the values returned to the game,
 * enforcing s × fake_output_resolution as the DLSS internal render size.
 *
 * Detected via loadlib_hooks.cpp when nvngx_dlss.dll is loaded.
 * On failure, calls SwapProxy_ForcePassthrough() to disable the feature.
 *
 * Feature: adaptive-dlss-scaling
 */

#pragma once
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

// Shutdown: remove hooks.
void NGXInterceptor_Shutdown();

// Called by K_Controller when fake output resolution changes.
// Updates the optimal render dimensions returned to the game.
void NGXInterceptor_UpdateOutputRes(uint32_t fake_w, uint32_t fake_h);

// Get current state for OSD/telemetry.
NGXInterceptorState NGXInterceptor_GetState();

// Returns true if Ray Reconstruction is detected.
bool NGXInterceptor_IsRayReconstructionActive();

// Called by loadlib_hooks.cpp when nvngx_dlss.dll is loaded.
// Installs the NGX parameter Get vtable hook.
void NGXInterceptor_OnDLSSDllLoaded(void* hModule);
