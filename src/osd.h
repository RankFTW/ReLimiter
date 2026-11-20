#pragma once

// ImGui must be included BEFORE reshade.hpp so that reshade_overlay.hpp
// can provide inline function table wrappers that replace ImGui declarations.
#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

// OSD rendering via ReShade + ImGui.
void RegisterOSD();
void StopOSD();

// Move clockwise to the next exact screen corner and persist the position.
void OSD_CyclePosition();
