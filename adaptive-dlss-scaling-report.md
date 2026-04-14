# FPS-Adaptive DLSS Scaling — Technical Feasibility Report

## Concept Summary

Dynamically adjust the DLSS internal render resolution based on real-time FPS, creating a feedback loop where the GPU workload self-regulates. When FPS drops, the render resolution drops (DLSS upscales more aggressively). When FPS is high, the render resolution rises (DLSS upscales less, better IQ). The final output is always downscaled to the real display resolution via a Lanczos pass, so the user sees a consistent native-res image.

The game thinks it's rendering at a fake higher resolution. DLSS upscales from its internal res to that fake res. Then you downscale from the fake res to the real display res. The DLSS preset stays fixed — the dynamic variable is the output multiplier `k`, which controls the fake output resolution relative to the real display.

---

## Resolution Math

Let:
- `D` = real display resolution
- `O` = fake output resolution the game sees (`O = k × D`)
- `s` = DLSS internal scale factor (fixed, e.g. 0.33 for Ultra Performance)
- `k` = output multiplier (the dynamic variable)

The DLSS internal render resolution is always: `internal = s × O = s × k × D`

The effective scale relative to the real display is: `effective = s × k`

### Example at 1080p display (D = 1920×1080), DLSS Ultra Performance (s = 0.33):

| k (output multiplier) | Fake output res | DLSS internal res | Effective vs display |
|------------------------|-----------------|-------------------|---------------------|
| 1.00 | 1920×1080 | 634×356 | 0.33x (Ultra Perf) |
| 1.25 | 2400×1350 | 792×446 | 0.42x |
| 1.50 | 2880×1620 | 950×535 | 0.50x (Performance) |
| 1.75 | 3360×1890 | 1109×624 | 0.58x (Balanced) |
| 2.00 | 3840×2160 | 1267×713 | 0.67x (Quality) |

So by varying `k` from 1.0 to 2.0 with a fixed Ultra Performance preset, you sweep the entire DLSS quality range from Ultra Performance to Quality — without ever changing the DLSS ratio or recreating the DLSS feature.

### Why this matters

Changing the DLSS ratio requires DLSS feature recreation on every tier change, which causes a brief hitch. By instead keeping the DLSS ratio fixed and varying `k`, you only need to resize the proxy backbuffer. DLSS just sees a different output resolution, which it can handle through its native dynamic resolution support.

### Diminishing returns: DLSS inference cost

DLSS inference cost scales with output resolution, not input resolution. From the Streamline docs:

| GPU | 1080p output | 1440p output | 4K output |
|-----|-------------|-------------|-----------|
| RTX 4090 | 1.35ms | 1.78ms | 2.77ms |
| RTX 5090 | 1.07ms | 1.46ms | 1.72ms |

At k=2.0 on a 1080p display, DLSS is outputting to 4K — costing ~2.77ms on a 4090 instead of ~1.35ms at k=1.0. Plus the Lanczos downscale from 4K→1080p. At some point the DLSS + downscale overhead eats into the fps gains from the lower internal render resolution.

The sweet spot depends on the GPU and the game's render cost, but roughly:
- k > 2.0 is almost certainly not worth it (DLSS cost dominates)
- k = 1.5–1.75 is likely the best IQ-per-cost range for most setups
- k = 1.0 is the emergency fallback when GPU-bound

### One-line version

If DLSS is at scale factor `s` and you run the game at `k×` your display resolution before downscaling back: `effective quality = s × k`

---

## Architecture Overview

```
D = 1920x1080 (real display)
k = 1.5 (current tier, dynamic)
O = 2880x1620 (fake output = D × k)
s = 0.33 (DLSS Ultra Performance, fixed)

Game renders at: O × s = 950x535
DLSS upscales:   950x535 → 2880x1620
Lanczos downscale: 2880x1620 → 1920x1080 (real display)
Effective quality: 0.33 × 1.5 = 0.50x (Performance equivalent)
```

The dynamic variable is `k`. When FPS drops, `k` drops (less DLSS work, less downscale work, lower IQ). When FPS is healthy, `k` rises (more DLSS work, better IQ). The DLSS preset never changes.

---

## Layer 1: Swapchain Interception

### What to hook

You need to intercept DXGI at the factory level:

- `IDXGIFactory::CreateSwapChain`
- `IDXGIFactory2::CreateSwapChainForHwnd`
- `IDXGIFactory2::CreateSwapChainForCoreWindow`
- `IDXGISwapChain::GetDesc` (lie about dimensions)
- `IDXGISwapChain::ResizeBuffers` (intercept resize events)
- `IDXGISwapChain::GetBuffer` (return your proxy backbuffer)
- `IDXGISwapChain::Present` / `Present1` (intercept for downscale)

### How it works

1. Game calls `CreateSwapChainForHwnd` requesting 2880x1620
2. You intercept, create the real swapchain at 1920x1080
3. You also create an intermediate render target at 2880x1620 (the "fake backbuffer")
4. When the game calls `GetBuffer(0, ...)`, return the fake backbuffer
5. When the game calls `GetDesc`, return the fake dimensions
6. When the game calls `Present`:
   - The fake backbuffer has the DLSS-upscaled frame at 2880x1620
   - Run Lanczos downscale compute shader: 2880x1620 → 1920x1080
   - Copy result to real swapchain backbuffer
   - Call real `Present`

### Existing precedent

- The swapchain proxy pattern is well-established in the modding community — multiple open-source projects intercept swapchain creation to insert intermediate render targets
- ReShade itself hooks swapchain creation for its overlay
- ReLimiter already hooks `CreateSwapChainForHwnd` and `Present` for its pacing logic
- All DXGI hooking is based on public Microsoft API documentation

### Complexity: Medium
You already have the DXGI hooking infrastructure. The new part is the proxy backbuffer and the downscale pass.

---

## Layer 2: DLSS Resolution Override

### The NGX Parameter System

DLSS uses NVIDIA's NGX framework. The game interacts with DLSS through `NVSDK_NGX_Parameter` objects — key-value pairs that control the feature. The critical flow:

1. Game calls `NGX_DLSS_GET_OPTIMAL_SETTINGS` with a quality mode (Quality/Balanced/Performance/Ultra Performance)
2. NGX returns `OutRenderOptimalWidth`, `OutRenderOptimalHeight` — the internal render resolution
3. Game allocates GBuffers at that resolution
4. Game calls `NGX_DLSS_CREATE_FEATURE` with Width/Height (output res) and the render dimensions
5. Each frame, game calls `NGX_DLSS_EVALUATE` passing the low-res color, depth, motion vectors → gets back the upscaled output

### Where to intercept

DLSSTweaks proved this works. It acts as a proxy `nvngx.dll` and intercepts the NGX parameter system:

- Hook `NVSDK_NGX_Parameter::Get` for `OutRenderOptimalWidth` / `OutRenderOptimalHeight`
- Override the returned values to force a custom render resolution ratio
- The game then allocates its GBuffers at your chosen resolution
- DLSS receives the smaller input and upscales to the game's "display" resolution (which is your fake resolution)

### The ratio

DLSS quality modes map to fixed ratios:
- DLAA: 1.0 (native)
- Quality: 0.667
- Balanced: 0.58
- Performance: 0.5
- Ultra Performance: 0.33

DLSSTweaks showed DLSS works at arbitrary ratios, not just the preset ones. You can set 0.4, 0.75, whatever. The NVIDIA App now even has a "DLSS Override - Super Resolution" slider that lets users set custom render resolution percentages from 33% to 100%.

### For dynamic scaling

The key realization: you don't change the DLSS ratio at all. You keep it fixed (e.g. Ultra Performance at 0.33x) and vary the output multiplier `k` instead. This is cleaner because:

1. No DLSS feature recreation needed (no hitch on tier change)
2. The DLSS ratio stays constant, so GBuffers don't need resizing
3. Only the proxy backbuffer and Lanczos target change size
4. DLSS handles varying output resolution natively via dynamic resolution support

**Recommended approach — Tiered output multiplier with hysteresis:**
- Fix DLSS at Ultra Performance (0.33x) or Performance (0.5x)
- Define tiers as values of `k` (output multiplier):

With s = 0.33 (Ultra Performance):
```
Tier 0 (emergency):  k=1.00 → effective 0.33x (Ultra Perf quality)
Tier 1:              k=1.25 → effective 0.42x
Tier 2:              k=1.50 → effective 0.50x (Performance quality)
Tier 3:              k=1.75 → effective 0.58x (Balanced quality)
Tier 4 (max IQ):     k=2.00 → effective 0.67x (Quality quality)
```

With s = 0.5 (Performance):
```
Tier 0 (emergency):  k=1.00 → effective 0.50x (Performance quality)
Tier 1:              k=1.25 → effective 0.63x
Tier 2:              k=1.34 → effective 0.67x (Quality quality)
Tier 3:              k=1.50 → effective 0.75x
Tier 4 (max IQ):     k=1.75 → effective 0.88x (near-DLAA quality)
```

- Monitor FPS with EMA smoothing (existing infrastructure)
- When FPS drops below target for N frames, drop one tier (lower k)
- When FPS exceeds target+headroom for N frames, raise one tier (higher k)
- Asymmetric hysteresis: fast to drop, slow to raise

On tier change, only the proxy backbuffer is resized. DLSS sees a new output resolution but the internal ratio is unchanged — no feature recreation, no GBuffer reallocation.

**Fallback — Changing the DLSS ratio (if needed):**
If a game doesn't handle varying output resolution well, you can fall back to changing the DLSS ratio with a fixed `k`. This requires DLSS feature recreation on tier change (~1-2 frame hitch) and GBuffer reallocation, but is more compatible with games that validate output dimensions.

### Complexity: Medium-High
The NGX interception is well-understood (DLSSTweaks is open source, frozen at 0.200.8). The dynamic part adds the feedback loop and feature recreation logic.

---

## Layer 3: GBuffer Consistency

### Why varying `k` mostly avoids this problem

When you vary the output multiplier `k` instead of the DLSS ratio, the DLSS internal render resolution changes proportionally — but so does the output resolution. The ratio between them stays constant. This means:

- The game's GBuffers (color, depth, motion vectors) are allocated at `O × s` where `O` is the fake output res
- When `k` changes, `O` changes, and the game needs to reallocate GBuffers at the new `O × s`
- BUT: the game already thinks the "display" changed resolution, so it goes through its normal resolution-change path
- Most games handle resolution changes gracefully (they do it when you change settings)

### Buffers that must match the internal render resolution:
- Color input (the game's render target)
- Depth buffer
- Motion vectors
- Exposure texture
- Any game-specific auxiliary buffers

### Buffers that must match the output resolution:
- DLSS output (upscaled color)
- HUD-less color (for Frame Generation)
- UI Color and Alpha (for Frame Generation)

### The remaining problem

The game thinks the display resolution changed. Some games handle this cleanly (just reallocate and continue). Others:
- Show a loading screen or brief black flash
- Reset graphics settings
- Require a restart

To minimize disruption:
- Keep tier changes infrequent (hysteresis)
- Pre-allocate the proxy backbuffer at the maximum `k` and use a viewport/scissor rect for lower tiers (avoids actual reallocation)
- For games that don't handle resolution changes well, fall back to the fixed-`k`-vary-DLSS-ratio approach

### For the fallback approach (varying DLSS ratio with fixed k):

On tier change, you need to:
1. Signal DLSS to release the current feature
2. Update the NGX parameters with new render dimensions
3. Let the game's next `GetOptimalSettings` call return the new render dimensions
4. The game reallocates its GBuffers (if it queries every frame — many do for DRS support)
5. Recreate the DLSS feature

Games that support Dynamic Resolution Scaling already handle buffer reallocation gracefully. Games that don't will need the buffers intercepted and resized externally via `ID3D12Device::CreateCommittedResource` / `CreatePlacedResource` hooks.

### Complexity: Medium (down from High — varying `k` sidesteps most of the hard cases)

---

## Layer 4: Mip Bias Correction

### The formula

DLSS requires a negative mip bias to compensate for rendering at lower resolution:

```
mip_bias = log2(render_resolution / display_resolution)
```

Examples:
- Quality (0.667x): log2(0.667) = -0.585
- Performance (0.5x): log2(0.5) = -1.0
- Ultra Performance (0.33x): log2(0.33) = -1.585

### The problem

Games typically set mip bias once based on the selected DLSS quality mode. If you're secretly running at a different ratio, the mip bias is wrong:
- Too high (not negative enough): textures look blurry/smudgy because DLSS gets low-detail input
- Too low (too negative): textures shimmer/alias because mip levels are too sharp for the actual sample density

### How to fix

**Option A — Hook CreateSampler (DX12):**
- In DX12, samplers are created via `ID3D12Device::CreateSampler` with a `D3D12_SAMPLER_DESC` that has a `MipLODBias` field
- Hook this call, detect samplers with DLSS-related bias values, override with your calculated bias
- Also need to handle static samplers defined in root signatures (these are baked at PSO creation time and harder to override)

**Option B — NvAPI mip bias override:**
- NVIDIA Profile Inspector has a global mip bias override per-application
- Could potentially set this programmatically via NvAPI at runtime
- Less invasive but coarser control

**Option C — Hook the game's mip bias setter:**
- Many engines set mip bias through a single code path
- UE4/5: `r.ViewTextureMipBias.Offset`
- Unity: `QualitySettings.lodBias`
- Intercept and override

### Recommended approach

Hook `CreateSampler` and apply a delta correction:
```
corrected_bias = original_bias + log2(actual_ratio / game_thinks_ratio)
```

For static samplers in root signatures, you'd need to hook `CreateRootSignature` and patch the serialized blob — doable but ugly.

### Complexity: Medium
The math is trivial. The hooking is straightforward for dynamic samplers. Static samplers in root signatures are the pain point.

---

## Layer 5: The Downscale Pass

### Lanczos Compute Shader

A separable Lanczos-3 downscale in a DX12 compute shader. Two passes (horizontal then vertical) for efficiency:

```
Pass 1: 2880x1620 → 1920x1620 (horizontal)
Pass 2: 1920x1620 → 1920x1080 (vertical)
```

Each pass samples 6 texels (Lanczos-3 kernel) per output pixel. At 1080p output this is roughly 12M texels per pass — trivially fast on any modern GPU, well under 0.1ms.

### Implementation

- Create two intermediate UAV textures (one for each pass)
- Dispatch compute shader with thread groups covering the output dimensions
- Use `SampleLevel` with calculated UV offsets for the Lanczos kernel weights
- The kernel weights are: `sinc(x) * sinc(x/3)` for |x| < 3, 0 otherwise

### Alternative: Hardware bilinear with RCAS

Even simpler — use a bilinear downscale (basically free via texture sampling) followed by an RCAS (Robust Contrast-Adaptive Sharpening) pass. This looks good enough for most cases and is trivial to implement.

### Complexity: Low
This is the easiest part of the whole system.

---

## Layer 6: The Dynamic Feedback Loop

### FPS monitoring

You already have all the infrastructure for this in ReLimiter:
- EMA-smoothed frame time measurement
- Percentile-based analysis (from adaptive smoothing)
- Hysteresis logic (from overload detection)

### Resolution controller

```
target_fps = user_setting (e.g. 144)
current_fps = ema_smoothed_fps
s = 0.33  // fixed DLSS Ultra Performance

// Tier definitions (output multiplier k)
tiers[] = { 1.0, 1.25, 1.5, 1.75, 2.0 }
// effective quality = s × k
// Tier 0: 0.33x (Ultra Perf) — emergency fallback
// Tier 2: 0.50x (Performance) — default starting point
// Tier 4: 0.67x (Quality)    — max IQ

if (current_fps < target_fps * 0.95 for 30+ frames)
    tier = max(tier - 1, 0)           // drop k → less work
    
if (current_fps > target_fps * 1.05 for 60+ frames)
    tier = min(tier + 1, max_tier)    // raise k → better IQ

// Asymmetric hysteresis: fast to drop, slow to raise
// Prevents oscillation at the boundary
```

### On tier change

1. Calculate new fake output resolution: `O = D * tiers[tier]`
2. Resize proxy backbuffer to new O (or adjust viewport if pre-allocated at max)
3. Resize Lanczos intermediate/output targets
4. Recalculate and apply mip bias correction: `bias = log2(s * tiers[tier])`
5. Log the transition for telemetry
6. No DLSS feature recreation needed — ratio unchanged

### Complexity: Low-Medium
The feedback loop is straightforward. The tricky part is making tier transitions smooth (no visible pop or hitch).

---

## Layer 7: Ray Reconstruction Compatibility

DLSS Ray Reconstruction (DLSS-RR) replaces traditional denoisers and operates on the same input buffers as DLSS-SR:
- Low-res color, depth, motion vectors, specular motion vectors
- Linear depth (additional requirement)
- Same quality modes and resolution ratios

Since DLSS-RR uses the same NGX parameter system and the same resolution pipeline, the override approach works identically. The only additional consideration:
- DLSS-RR is more sensitive to input quality at very low resolutions
- At 0.33x, RR may produce more artifacts than standard denoising
- Recommend a minimum ratio floor of 0.5x when RR is active

### Complexity: Low (additive to existing DLSS-SR interception)

---

## Layer 8: Frame Generation Interaction

If DLSS Frame Generation is also active, the FG pipeline operates on the DLSS-SR output (the fake display resolution). Your downscale pass runs after FG, on the final presented frame.

The flow becomes:
```
Game renders at internal res (e.g. 640x360)
DLSS-SR upscales to fake res (e.g. 2880x1620)
DLSS-FG generates interpolated frames at fake res
Present intercepted → Lanczos downscale to real res (1920x1080)
```

FG's motion vectors and depth are at the fake resolution, which is fine — FG doesn't care about the downstream downscale.

### Complexity: Low (transparent to FG)

---

## Risk Assessment

| Risk | Severity | Likelihood | Mitigation |
|------|----------|------------|------------|
| Game validates swapchain dimensions | High | Medium | Fallback to non-intercepted mode |
| Game doesn't handle resolution changes cleanly | Medium | Medium | Pre-allocate at max k, use viewport; keep tier changes infrequent |
| Static samplers in root signatures | Medium | High | Accept wrong mip bias for these; most games use dynamic samplers for scene rendering |
| DLSS inference cost at high k | Medium | High | Cap k at 2.0; profile per-GPU to find diminishing returns threshold |
| Anti-cheat detection | High | Medium | Same risk as any DLL injection mod |
| DLSS minimum resolution limits | Medium | Low | Respect GetOptimalSettings min/max bounds |
| HDR format handling in downscale | Low | Medium | Support R10G10B10A2 and R16G16B16A16F in Lanczos shader |

---

## Implementation Phases

### Phase 1: Static proof of concept (1-2 weeks)
- Hook CreateSwapChain, create proxy backbuffer at fixed k=1.5 (1.5× real res)
- Hook GetDesc to lie about dimensions
- Implement Lanczos downscale compute shader
- Hook Present to downscale and copy
- Test with 3-4 DLSS games at fixed output multiplier
- **Goal: prove the swapchain lie + downscale pipeline works**

### Phase 2: DLSS ratio override + mip bias (1-2 weeks)
- Integrate NGX parameter interception (reference DLSSTweaks source)
- Fix DLSS at Ultra Performance (0.33x), verify it works with the faked output res
- Implement mip bias correction via CreateSampler hook: `bias = log2(s × k)`
- Test DLSS-SR quality at various k values
- **Goal: prove DLSS works correctly at fixed ratio with varying output multiplier + corrected mip bias**

### Phase 3: Dynamic feedback loop (1 week)
- Implement tiered k controller with EMA + hysteresis
- Handle proxy backbuffer resize on tier change (or viewport adjustment if pre-allocated)
- Add OSD display for current tier/k/effective resolution
- Profile DLSS inference cost at each tier to validate diminishing returns threshold
- **Goal: FPS-adaptive resolution working end-to-end**

### Phase 4: Compatibility and polish (2-4 weeks)
- Test across 20+ DLSS titles
- Handle edge cases (DRS games, games that validate buffers, HDR)
- Add Ray Reconstruction support
- Verify Frame Generation compatibility
- Per-game config overrides for known problematic titles
- **Goal: ship-ready for broad game compatibility**

---

## Key References

- [DLSSTweaks](https://github.com/emoose/DLSSTweaks) — NGX parameter interception, resolution ratio override, mip bias. Frozen at 0.200.8 but source available (MIT license)
- [NVIDIA DLSS SDK](https://github.com/NVIDIA/DLSS) — Official headers, NGX parameter definitions, integration samples
- [Streamline SDK](https://github.com/NVIDIA-RTX/Streamline) — DLSS-FG/SR/RR programming guides, Reflex integration
- [NVIDIA NGX Programming Guide](https://docs.nvidia.com/rtx/ngx/programming-guide/index.html) — NGX parameter system, feature creation/evaluation API
- [Microsoft DXGI Documentation](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/dx-graphics-dxgi) — Swapchain creation, Present, ResizeBuffers API reference
- Lossless Scaling — Adaptive Frame Generation concept (dynamic multiplier based on FPS target)

---

## Verdict

This is buildable. The corrected approach — fixing the DLSS ratio and varying the output multiplier `k` — is significantly cleaner than the original proposal of changing the DLSS ratio dynamically. It avoids DLSS feature recreation, avoids GBuffer reallocation in most cases, and reduces the problem to swapchain proxy management + a downscale pass.

The swapchain proxy pattern is well-established in the modding community, the DLSS ratio override is proven (DLSSTweaks + NVIDIA App), the downscale is trivial, and the feedback loop is just your existing adaptive smoothing infrastructure repurposed.

The main constraint is DLSS inference cost scaling with output resolution — at k=2.0 on 1080p you're asking DLSS to upscale to 4K, which costs real GPU time. The sweet spot is k=1.25–1.75 for most setups. Beyond k=2.0 you're almost certainly losing more to DLSS overhead than you're gaining from the lower render resolution.

The real engineering cost is in Phase 4 — per-game compatibility. Budget 60% of total effort there.

Estimated total: 5-8 weeks for a solid v1.0 covering the majority of DLSS DX12 titles (reduced from 6-10 due to the simpler architecture).
