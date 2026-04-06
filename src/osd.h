#pragma once

// ImGui must be included BEFORE reshade.hpp so that reshade_overlay.hpp
// can provide inline function table wrappers that replace ImGui declarations.
#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

// OSD rendering via ReShade + ImGui. Spec §III.5.
void DrawSettings(reshade::api::effect_runtime* rt);
void DrawOSD(reshade::api::effect_runtime* rt);
void RegisterOSD();
