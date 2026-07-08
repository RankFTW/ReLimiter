#pragma once

#include <cstdint>

// Vulkan/Present-based enforcement point.
// Called from EnfDisp_OnPresent when enforcement path is PresentBased.
// Uses shared scheduler core (OnMarker). Works for DX9, DX11, Vulkan, OpenGL.

void VkEnforce_Init();
void VkEnforce_Shutdown();

// Called before each present via ReShade present event.
void VkEnforce_OnPresent(int64_t now_qpc);
