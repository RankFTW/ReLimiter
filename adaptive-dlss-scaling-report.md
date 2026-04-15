# Adaptive DLSS Scaling — Comprehensive Technical Report

---

## TL;DR FOR HUMANS

**What we're trying to do**: Make DLSS upscale to a bigger resolution than the screen (e.g. 6020×2520 instead of 3440×1440), then shrink it back down with a high-quality Lanczos filter. This gives you a sharper image because DLSS is doing more work. The "adaptive" part means we automatically pick how much bigger based on how much GPU headroom you have — if your GPU is barely sweating, we crank it up; if it's struggling, we back off.

**What game**: Crimson Desert. It uses NVIDIA's "Streamline" framework, which is a middleware layer that sits between the game and DLSS. The game never talks to DLSS directly — everything goes through Streamline.

**What works**: Everything EXCEPT the actual interception. The brain that decides when to scale up/down (K_Controller) works perfectly. The shader that shrinks the image back down (Lanczos) works. The OSD, telemetry, tier system — all good. We can hook into every Streamline function, read the game's settings, see the output resource, read DLSS dimensions. The plumbing is all there.

**What doesn't work**: Swapping the output texture. To make DLSS render bigger, we need to give it a bigger texture to write to. We tried 8 different ways to do this. Every single one either crashes the game (DEVICE_REMOVED) or produces a broken image (half the geometry missing).

**Why it doesn't work**: Streamline is a control freak. When the game registers a texture with Streamline ("here's where DLSS should write"), Streamline memorizes everything about that texture — its size, format, and apparently the actual pointer address. If we swap in a different texture at ANY point — before registration, after registration, during evaluation, via local tags — Streamline detects the mismatch and kills the D3D12 device. We tried swapping before Streamline sees it, after, during, at the Streamline API level, at the NGX level inside the driver proxy — all crash.

The one thing we CAN'T do is replace the DLSS DLL itself (like OptiScaler does), because we're a ReShade addon, not a DLL replacement. OptiScaler works because it IS the upscaler — it owns the output texture from the start. We're trying to modify someone else's texture from the outside, and Streamline won't let us.

**What's next**: Three options:
1. **OptiScaler integration** — Let OptiScaler handle the texture swap (it can), and we just tell it what multiplier to use via its config file. Requires user to install OptiScaler.
2. **Post-DLSS sharpening** — Give up on making DLSS render bigger, and instead apply a sharpening/enhancement filter after DLSS finishes. Less impactful but actually works.
3. **Non-Streamline games** — The feature would work as-is for games that use `nvngx_dlss.dll` directly (no Streamline). Those games expose the standard DLSS parameter interface where we CAN read and swap the output texture.

---

## Executive Summary

The goal is to intercept DLSS Super Resolution in Crimson Desert (DX12 + Streamline + DLSS-FG) to make DLSS upscale to a higher resolution (k×D) then Lanczos-3 downscale back to display resolution (D), dynamically adjusting k based on GPU headroom. After exhaustive testing of 8+ distinct approaches across ~20 builds, **we have not found a way to swap the DLSS output resource in a Streamline game without triggering DXGI_ERROR_DEVICE_REMOVED**. The supporting infrastructure (K_Controller, Lanczos shader, tier system, OSD, telemetry) all work correctly. The blocker is Streamline's resource management layer.

---

## Target Game Architecture

**Crimson Desert** uses NVIDIA Streamline for all DLSS features. The rendering pipeline is:

```
Game → sl.interposer.dll (Streamline API layer)
    → sl.dlss.dll (Streamline DLSS plugin)
        → _nvngx.dll (NVIDIA NGX runtime / driver proxy)
            → model .bin files (actual DLSS neural network weights)
```

**Key architectural facts:**
- There is **NO `nvngx_dlss.dll`** — Streamline loads DLSS through `sl.dlss.dll` + model `.bin` files from `C:\ProgramData\NVIDIA\NGX\models\dlss\`
- `_nvngx.dll` is loaded from the driver directory (`C:\WINDOWS\System32\DriverStore\...`)
- The game calls `slEvaluateFeature(kFeatureDLSS, ...)` — NOT `NVSDK_NGX_D3D12_EvaluateFeature` directly
- Resources are tagged via `slSetTag()` with `kBufferTypeScalingOutputColor` for the DLSS output
- DLSS options (output dimensions, quality mode) are set via `slDLSSSetOptions(viewport, options)`
- Display: 3440×1440, 164.9Hz G-Sync, RTX GPU with MWAITX spin

---

## What Works (Stable Infrastructure)

### K_Controller
- Uses Reflex `gpuActiveRenderTimeUs` (~4ms) vs target interval (6.37ms at 157fps) to decide tiers
- 5 tiers: k=1.0 (T1) through k=2.25 (T5) in 0.25 increments
- Asymmetric hysteresis: 30 frames below threshold to drop, 60 frames above to raise
- Correctly identifies GPU headroom and ramps to T5 k=2.25
- Frame Generation awareness (divides FPS by FG multiplier for decisions)
- Ray Reconstruction quality floor (minimum effective quality ≥ 0.5)
- Safety tier lock on repeated spikes
- Zero-FPS guard prevents running during loading screens

### Lanczos-3 Compute Shader
- Two-pass separable Lanczos-3 downscale (horizontal then vertical)
- DX12 compute shader with root signature, PSOs, descriptor heap
- Intermediate texture management with lazy resize
- Format-aware (matches game's output format, typically DXGI_FORMAT_R16G16B16A16_FLOAT = format 10)
- Initializes successfully, allocates intermediate textures correctly

### Streamline Hook Infrastructure
- `slEvaluateFeature` hook on `sl.interposer.dll` — fires stably (thousands of calls, no crash)
- `slDLSSSetOptions` hook via `slGetFeatureFunction` — captures game's output dimensions (3440×1440)
- `slSetTag` / `slSetTagForFrame` hooks — capture the game's output resource pointer
- `NVSDK_NGX_D3D12_EvaluateFeature` hook on `_nvngx.dll` — fires stably in passthrough mode
- `NVSDK_NGX_D3D12_CreateFeature` hook — detects Ray Reconstruction

### OSD & Telemetry
- Shows active tier, k value, effective quality
- CSV telemetry with DLSS tier, k, effective quality, internal resolution columns

---

## Streamline Struct Layout Discoveries

Through binary probing, we reverse-engineered the Streamline struct layouts:

### sl::BaseStructure (32 bytes)
```
Offset  Size  Field
0       8     BaseStructure* next
8       16    StructType structType (GUID)
24      8     size_t structVersion
```

### sl::DLSSOptions (inherits BaseStructure)
```
Offset  Size  Field
0-31    32    BaseStructure header
32      4     DLSSMode mode (uint32_t enum)
36      4     uint32_t outputWidth
40      4     uint32_t outputHeight
44      4     float sharpness
...
```
Source: Official Streamline SDK headers (MIT licensed, github.com/NVIDIA-RTX/Streamline)

### sl::ResourceTag (64 bytes — NOT 56 as calculated from SDK)
```
Offset  Size  Field
0-31    32    BaseStructure header
32      8     sl::Resource* resource
40      4     uint32_t type (buffer type ID, e.g. 4 = kBufferTypeScalingOutputColor)
44      4     uint32_t lifecycle
48      8     sl::Extent* extent
56      8     [padding — confirmed by stride probing]
```
**Discovery method**: Stride probing. With stride=56, tag[1] read garbage. With stride=64, tag[1] correctly read bufType=4 (kBufferTypeScalingOutputColor) with a valid resource pointer. Tested strides 48-128 in 8-byte increments.

### sl::Resource (inherits BaseStructure — NOT a flat struct)
```
Offset  Size  Field
0-31    32    BaseStructure header (next, structType GUID, structVersion)
32      4     uint32_t type (ResourceType enum, 0 = eTex2d)
36      4     [padding]
40      8     void* native (ID3D12Resource*)
48+     0     All zeros (no cached dimension fields)
```
**Discovery method**: Memory dump of first 96 bytes. Offset +8 contained GUID bytes (not a pointer), confirming BaseStructure inheritance. Offset +40 contained the valid ID3D12Resource* pointer. Offsets 48-96 were all zeros.

**Critical finding**: The `native` pointer is at offset **40**, not offset 8. Initial assumption was that sl::Resource was a flat struct (type at 0, native at 8), but it inherits BaseStructure like everything else in Streamline.

### NVSDK_NGX_Parameter vtable (from official NVIDIA DLSS SDK)
```
Index  Signature
[0]    Set(const char*, unsigned long long)
[1]    Set(const char*, float)
[2]    Set(const char*, double)
[3]    Set(const char*, unsigned int)        ← SetUI
[4]    Set(const char*, int)
[5]    Set(const char*, ID3D11Resource*)
[6]    Set(const char*, ID3D12Resource*)     ← SetD3d12Resource
[7]    Set(const char*, void*)
[8]    Get(const char*, unsigned long long*)
[9]    Get(const char*, float*)
[10]   Get(const char*, double*)
[11]   Get(const char*, unsigned int*)       ← GetUI
[12]   Get(const char*, int*)
[13]   Get(const char*, ID3D11Resource**)
[14]   Get(const char*, ID3D12Resource**)    ← GetD3d12Resource
[15]   Get(const char*, void**)
[16]   Reset()
```
Source: `nvsdk_ngx_params.h` from github.com/NVIDIA/DLSS (public SDK)

**Our earlier code used wrong indices**: 5 for SetResource, 9 for GetUInt, 12 for GetResource. The correct indices are 6, 11, and 14 respectively.

---

## Approaches Tried and Results

### Approach 1: Local Tag Injection in slEvaluateFeature
**Commit**: c5c32f2 (v3 initial)
**Idea**: Build a local `sl::ResourceTag` struct pointing to our k×D intermediate buffer, append it to the `inputs` array passed to `slEvaluateFeature`. Per Streamline docs, local tags override global tags.
**Result**: `DXGI_ERROR_DEVICE_REMOVED` (0x887A0001). The fabricated ResourceTag/Resource structs didn't match Streamline's internal expectations. The struct layout was wrong (we hadn't yet discovered the correct sizes and offsets).

### Approach 2: In-place Resource Swap in slSetTag (after call)
**Commit**: 38e989c
**Idea**: In the `slSetTag` hook, swap the `sl::Resource.native` pointer to our intermediate buffer, call original `slSetTag`, then restore. Streamline would use our buffer during the evaluate phase.
**Result**: `DXGI_ERROR_DEVICE_REMOVED`. Streamline caches resource metadata when `slSetTag` is called. The cached metadata (from the original D-sized resource) didn't match the swapped k×D buffer at evaluate time.

### Approach 3: Dimension-only Override in slDLSSSetOptions
**Commit**: b6ca037
**Idea**: Only override `outputWidth`/`outputHeight` in `slDLSSSetOptions` to k×D, without swapping the output resource. If DLSS internally handles the mismatch by downscaling to fit the output resource, we'd get better quality for free.
**Result**: **Missing geometry**. DLSS wrote k×D pixels into the D-sized output buffer, producing a partial/clipped image. DLSS does NOT downscale to fit — it writes the full k×D output regardless of the resource size. No crash, but visually broken.

### Approach 4: Atomic Resource Swap in slEvaluateFeature
**Commit**: 42cfada
**Idea**: Do the resource swap atomically inside `slEvaluateFeature` — swap the `sl::Resource.native` pointer right before calling original, then restore right after. No dimension override. The theory was that DLSS would see the larger resource and upscale to fill it.
**Result**: `DXGI_ERROR_DEVICE_REMOVED`. Same root cause as Approach 2 — Streamline validates the resource at evaluate time against cached metadata from `slSetTag`.

### Approach 5: Direct _nvngx.dll EvaluateFeature Hook with Correct Vtable Indices
**Commit**: 78e1935 (v5)
**Idea**: Hook `NVSDK_NGX_D3D12_EvaluateFeature` on `_nvngx.dll` (the real NGX evaluation called by `sl.dlss.dll`). Use the correct vtable indices from the official NVIDIA DLSS SDK (index 14 for GetD3d12Resource, index 6 for SetD3d12Resource) to read/write the "Output" parameter.
**Result**: **Get("Output") returned null** (result=0xBAD00010 = NVSDK_NGX_Result_FAIL_UnsupportedParameter). All resource parameter names returned null. However, `GetUI` for "Width", "Height", "OutWidth", "OutHeight" worked correctly (returned 3440, 1440, 3440, 1440).

**Root cause**: Streamline's `_nvngx.dll` proxy stores integer parameters but NOT resource pointers in the named parameter map. Resources are passed through Streamline's internal pipeline (`slSetTag` → `sl.dlss.dll` → DLSS engine) and never exposed through the `NVSDK_NGX_Parameter` interface.

**Parameter names probed** (all returned null/0xBAD00010):
- "Output", "Color", "DLSS.Output", "DLSS.Output.Color"
- "ScalingOutputColor", "OutputColor", "OutColor"
- "#\x22" (NVSDK_NGX_EParameter_Output), "#\x1e" (NVSDK_NGX_EParameter_Color)

### Approach 6: Pre-call Resource Swap in slSetTag (before call)
**Commit**: f71a858 (v6)
**Idea**: Swap the `sl::Resource.native` pointer BEFORE calling original `slSetTag`. Streamline would cache metadata from our k×D buffer (via `GetDesc()` on the native pointer), so there would be no metadata mismatch at evaluate time.
**Result**: `DXGI_ERROR_DEVICE_REMOVED`. Crash ~1 second after the first swap (when k goes above 1.0 after focus gain). Streamline validates more than just the resource dimensions — it likely validates the `ID3D12Resource*` pointer identity or has internal state tied to the specific resource object.

### Approach 6b: Pre-call Swap + slDLSSSetOptions Dimension Override
**Commit**: 5c3882a
**Idea**: Same as v6, but also override `outputWidth`/`outputHeight` in `slDLSSSetOptions` to match k×D. Both changes happen before Streamline sees them — fully consistent k×D resource + k×D dimensions.
**Result**: `DXGI_ERROR_DEVICE_REMOVED`. The `slDLSSSetOptions` override never actually fired (the game only calls it once at init, before our hook is installed). But even the resource swap alone caused the crash.

### Approach 6c: Pre-call Swap + NGX SetUI OutWidth/OutHeight Override
**Commit**: fb097e0
**Idea**: Same as v6, but override `OutWidth`/`OutHeight` directly on the `_nvngx.dll` parameter object via vtable[3] (SetUI) every frame in the NGX EvaluateFeature hook. This ensures dimensions are overridden even if `slDLSSSetOptions` isn't called.
**Result**: `DXGI_ERROR_DEVICE_REMOVED`. The NGX dimension override didn't reach the active code path before the crash from the slSetTag resource swap.

---

## Crashes Fixed During Development (Non-interception)

1. **Null trampoline crash**: `NGXInterceptor_Shutdown` was called on swapchain destroy, nulling the MinHook trampoline while the detour was still active. Fix: `NGXInterceptor_ReleaseGPUResources()` for swapchain cycles, full `Shutdown()` only at addon unload.

2. **DX11 device destroy killing Lanczos**: `on_destroy_device` called `Lanczos_Shutdown()` for the DX11 launcher device. Fix: only shut down for DX12 device.

3. **Zero-FPS K_Controller crash**: `KController_Update` with `ema_fps=0.0` during loading caused immediate tier transitions. Fix: skip when `ema_fps <= 0`.

4. **`GetDesc` thread safety crash**: `IDXGISwapChain::GetDesc` on scheduler thread corrupted Streamline proxy. Fix: use `GetClientRect(HWND)` instead.

5. **g_device null after swapchain recreate**: `ReleaseGPUResources` nulled `g_device`, but the DX12 device outlives the swapchain. Fix: only null device in `Shutdown()`, not `ReleaseGPUResources()`.

6. **Intermediate buffer allocation failure**: `EnsureIntermediateBuffer` failed because `g_device` was null (see #5 above). This caused the `slDLSSSetOptions` dimension override to fire without the resource swap, producing missing geometry.

7. **Invalid captured resource pointer**: The `sl::Resource.native` offset was wrong (8 instead of 40) because `sl::Resource` inherits `BaseStructure`. The "captured" pointer was GUID bytes, not a real pointer. Fix: correct offset to 40.

---

## Key Technical Findings

### Streamline Resource Validation
Streamline validates resources at multiple levels:
- **slSetTag time**: Caches resource metadata (dimensions, format, state) — likely by calling `ID3D12Resource::GetDesc()` on the native pointer
- **slEvaluateFeature time**: Validates that the resource matches cached metadata
- **Resource identity**: Streamline appears to track the actual `ID3D12Resource*` pointer, not just its properties. Swapping to a different resource with identical dimensions/format still triggers DEVICE_REMOVED.

### _nvngx.dll Parameter Object in Streamline
The `NVSDK_NGX_Parameter` object passed to `NVSDK_NGX_D3D12_EvaluateFeature` on `_nvngx.dll` is a **partial proxy**:
- Integer parameters work: `GetUI("Width")` = 3440, `GetUI("Height")` = 1440, `GetUI("OutWidth")` = 3440, `GetUI("OutHeight")` = 1440
- Resource parameters are NOT stored: `Get("Output")` returns `NVSDK_NGX_Result_FAIL_UnsupportedParameter` (0xBAD00010) for ALL resource parameter names
- The vtable layout matches the official NVIDIA DLSS SDK (indices 3/11 for SetUI/GetUI work correctly)
- Resources flow through Streamline's internal pipeline, bypassing the named parameter map entirely

### GLOM (Global Override Mode)
Tested with and without GLOM (NVIDIA DLSS Global Override Mode). Results were identical — GLOM does not affect the parameter object layout or Streamline's resource validation. GLOM only changes which model `.bin` files are loaded.

---

## Remaining Viable Approaches

### 1. OptiScaler Integration
OptiScaler replaces `nvngx_dlss.dll` entirely and owns the upscaling pipeline. Its "Pseudo SuperSampling" feature does exactly what we want — upscale to k×D then downsample. Our K_Controller could drive OptiScaler's `SuperSamplingMultiplier` setting via INI file writes. Requires user to install OptiScaler separately. Unknown whether OptiScaler hot-reloads the multiplier during gameplay.

### 2. Post-DLSS Enhancement Pass
Instead of making DLSS render at higher resolution, apply a post-processing enhancement pass (RCAS sharpening, CAS, or custom filter) to the DLSS output after it writes to the game's backbuffer. Less ambitious — doesn't improve DLSS's internal quality — but achievable without any Streamline manipulation. The K_Controller could dynamically adjust sharpening intensity based on GPU headroom.

### 3. Non-Streamline Game Support
The feature would work as designed for games that use `nvngx_dlss.dll` directly (not through Streamline). The standard NGX parameter vtable has full resource access. The entire infrastructure (K_Controller, Lanczos shader, tier system) is ready. Only the interception layer needs to target `nvngx_dlss.dll` instead of `_nvngx.dll`.

### 4. Hook Inside sl.dlss.dll
The Streamline DLSS plugin (`sl.dlss.dll`) internally calls the real DLSS evaluation with standard parameters. Binary analysis of `sl.dlss.dll` could reveal the internal call site where resources are passed to the DLSS engine. Hooking at that level would give access to the real resource pointers. This is fragile (version-specific offsets) but technically possible.

---

## File Inventory

| File | Purpose | Status |
|------|---------|--------|
| `src/dlss_ngx_interceptor.cpp` | Main interception code, all Streamline + NGX hooks | All hooks passthrough, capture-only |
| `src/dlss_ngx_interceptor.h` | Public API declarations | Stable |
| `src/dlss_k_controller.cpp` | K_Controller with Reflex GPU active time logic | Working correctly |
| `src/dlss_k_controller.h` | K_Controller API | Stable |
| `src/dlss_lanczos_shader.h` | Lanczos dispatch interface | Stable |
| `src/dlss_resolution_math.h` | Resolution computation helpers | Stable |
| `src/scheduler.cpp` | Tier transition orchestration, K_Controller update loop | Working correctly |
| `src/dllmain.cpp` | Integration points, lifecycle management | Stable |
| `src/loadlib_hooks.cpp` | DLL detection (proxy vs model DLLs) | Stable |
| `src/streamline_hooks.cpp` | Streamline DLSS-G hooks + slDLSSSetOptions resolution | Stable |
| `shaders/lanczos3_horizontal.hlsl` | Horizontal Lanczos-3 compute shader | Compiled, working |
| `shaders/lanczos3_vertical.hlsl` | Vertical Lanczos-3 compute shader | Compiled, working |

---

## Git History (adaptive-dlss branch)

```
5deec24 Revert to stable: all interception disabled
fb097e0 v6c: override OutWidth/OutHeight via NGX vtable SetUI
5c3882a v6b: add dimension override to match resource swap
f71a858 v6: pre-call resource swap in slSetTag — trick Streamline
e931817 Probe alternate param names + GetUI for Width/Height diagnostic
78e1935 v5: hook _nvngx.dll EvaluateFeature with correct vtable indices
0eb3aa0 Disable interception: all resource swap approaches cause DEVICE_REMOVED
42cfada v4: atomic resource swap in slEvaluateFeature, no dim override
b6ca037 Test: dimension-only override, no resource swap
685c539 Fix: guard slDLSSSetOptions on intermediate buffer + add swap diag
38e989c Replace local tag injection with in-place resource swap in slSetTag
b27d4e9 Fix: preserve g_device across swapchain destroy/recreate
1f11c84 Fix sl::Resource native offset: 40 not 8 (inherits BaseStructure)
091fa9d Fix: validate captured resource pointer + dump sl::Resource layout
0425f5e Fix ResourceTag stride: 64 bytes not 56
214a23c Add stride probing to find correct ResourceTag struct size
51963dd Improve slSetTag diagnostic: dump bufType and resource ptr per tag
c1297ec Fix: guard slDLSSSetOptions override until output resource captured
c5c32f2 NGX interceptor v3: Streamline-level slDLSSSetOptions + slSetTag hooks
```

---

## Environment

- GPU: NVIDIA RTX (with MWAITX spin support)
- Display: 3440×1440, 164.9Hz G-Sync
- Game: Crimson Desert (DX12 + Streamline + DLSS-FG)
- Target FPS: 157
- DLSS scale factor: 0.330 (Ultra Performance)
- Build: Visual Studio 2022, CMake, MinHook
- Addon type: ReShade addon (.addon64)
