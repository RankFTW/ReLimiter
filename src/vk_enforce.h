#pragma once

#include <cstdint>

// Vulkan enforcement point. FR-3.
// Called from EnfDisp_OnPresent when enforcement path is PresentBased.
// Uses shared scheduler core (OnMarker).

void VkEnforce_Init();
void VkEnforce_Shutdown();

// Called before each vkQueuePresentKHR via ReShade present event.
void VkEnforce_OnPresent(int64_t now_qpc);
