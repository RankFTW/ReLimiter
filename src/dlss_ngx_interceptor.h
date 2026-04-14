/**
 * NGX_Interceptor — Intercepts DLSS evaluation for Adaptive DLSS Scaling.
 *
 * Two hooking strategies, selected automatically:
 *
 * 1. STREAMLINE MODE (preferred for Streamline games):
 *    Hooks slEvaluateFeature from sl.interposer.dll. This is the game-facing
 *    Streamline API — no internal proxy DLLs are touched. Avoids the black
 *    screen caused by MinHook on _nvngx.dll corrupting Streamline's dispatch.
 *
 * 2. DIRECT NGX MODE (fallback for non-Streamline games):
 *    Hooks NVSDK_NGX_D3D12_EvaluateFeature from nvngx_dlss.dll directly.
 *    Safe when there's no Streamline proxy in the way.
 *
 * In both modes, the hook:
 *   1. Swaps the DLSS output target from the game's backbuffer (D) to our
 *      intermediate buffer (k×D)
 *   2. Calls the original evaluate — DLSS upscales to k×D
 *   3. Lanczos downscales from k×D back to the game's original output (D)
 *   4. Game continues with its backbuffer at D, completely unaware
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
// Only call at addon unload — NOT on swapchain destroy/recreate.
void NGXInterceptor_Shutdown();

// Release GPU resources (intermediate buffer, device pointer) without
// removing hooks. Call on swapchain destroy/recreate cycles.
// Hooks must survive because the game will call slEvaluateFeature again.
void NGXInterceptor_ReleaseGPUResources();

// Called by K_Controller when fake output resolution changes.
void NGXInterceptor_UpdateOutputRes(uint32_t fake_w, uint32_t fake_h);

// Get current state for OSD/telemetry.
NGXInterceptorState NGXInterceptor_GetState();

// Returns true if Ray Reconstruction is detected.
bool NGXInterceptor_IsRayReconstructionActive();

// Called by loadlib_hooks.cpp when nvngx_dlss.dll is loaded.
// In Streamline mode, this only installs CreateFeature hook for RR detection.
// In non-Streamline mode, this installs the direct EvaluateFeature hook.
void NGXInterceptor_OnDLSSDllLoaded(void* hModule);

// Called when sl.interposer.dll is loaded (from loadlib_hooks.cpp).
// Installs the slEvaluateFeature hook — the preferred Streamline path.
void NGXInterceptor_OnStreamlineLoaded(void* hModule);

// Set the DX12 device for intermediate buffer allocation.
void NGXInterceptor_SetDevice(ID3D12Device* device);

// Set current k value and display dimensions for the EvaluateFeature hook.
void NGXInterceptor_SetScalingParams(double k, uint32_t display_w, uint32_t display_h);

// Returns the name of the active hook mode ("Streamline", "DirectNGX", or "None").
const char* NGXInterceptor_GetHookModeName();
