#pragma once

#include <cstdint>

// Flush protocol: Flush(modules) procedure.
// Spec §IV.6.

// Module flags for selective flush. Bits 1 (stress), 3 (damping) and 4 (the
// old cadence PQI, replaced by PQI_FromSnapshot) are retired with their
// modules. FLUSH_PREDICTOR keeps its historical name: it now starts a new
// controller lifecycle (generation), which resets the scheduler's cadence
// grid, frame plans and lead controller (P2-10).
constexpr uint32_t FLUSH_CORRELATOR = 1 << 0;
constexpr uint32_t FLUSH_PREDICTOR  = 1 << 2;
constexpr uint32_t FLUSH_ALL        = FLUSH_CORRELATOR | FLUSH_PREDICTOR;

// Thread-safe request publication. Every flush invalidates Smoothness and
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
