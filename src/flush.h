#pragma once

#include <cstdint>

// Flush protocol: Flush(modules) procedure.
// Spec §IV.6.

// Module flags for selective flush
constexpr uint32_t FLUSH_CORRELATOR = 1 << 0;
constexpr uint32_t FLUSH_STRESS     = 1 << 1;
constexpr uint32_t FLUSH_PREDICTOR  = 1 << 2;
constexpr uint32_t FLUSH_DAMPING    = 1 << 3;
constexpr uint32_t FLUSH_PQI        = 1 << 4;
constexpr uint32_t FLUSH_ALL        = FLUSH_CORRELATOR | FLUSH_STRESS |
                                      FLUSH_PREDICTOR | FLUSH_DAMPING |
                                      FLUSH_PQI;

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
