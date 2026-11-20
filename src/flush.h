#pragma once

#include <cstdint>

// Flush protocol: Flush(modules) procedure.
// Selective flush by module flags.

// Module flags for selective flush. Bits 1 (stress), 3 (damping) and 4 (the old cadence quality score) are
// retired with their modules. FLUSH_PREDICTOR keeps its historical name: it now starts a new
// controller lifecycle (generation), which resets the scheduler's cadence
// grid, frame plans and lead controller.
constexpr uint32_t FLUSH_CORRELATOR = 1 << 0;
constexpr uint32_t FLUSH_PREDICTOR  = 1 << 2;
constexpr uint32_t FLUSH_ALL        = FLUSH_CORRELATOR | FLUSH_PREDICTOR;

// Thread-safe request publication. Every flush invalidates the cadence readout and
// requests feedback resets; the scheduler owner
// consumes those requests at the start of its next OnMarker.
void Flush(uint32_t modules);

// Swapchain lifecycle handlers (registered as ReShade events).
void OnInitSwapchain(void* native_swapchain);
void OnDestroySwapchain();

// Event-driven flush triggers
void OnPresentFailure();
void OnDeviceLost();
void OnOccluded(bool occluded);
void OnFGStateChange();
void OnCorrelatorOverflow();
