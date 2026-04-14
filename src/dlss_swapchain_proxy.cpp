/**
 * Swapchain_Proxy implementation — DXGI factory and swapchain vtable hooks.
 *
 * Intercepts CreateSwapChainForHwnd, CreateSwapChainForCoreWindow, CreateSwapChain
 * on the DXGI factory to create a proxy backbuffer at the fake output resolution.
 *
 * Overrides GetDesc/GetDesc1 to report fake dimensions, GetBuffer to return the
 * proxy backbuffer, and ResizeBuffers to resize the proxy while keeping the real
 * swapchain at the display resolution.
 *
 * Pre-allocation: allocates at k_max×D, uses viewport/scissor for lower tiers.
 * Passthrough: on error, reverts all overrides within one frame.
 *
 * Feature: adaptive-dlss-scaling
 */

#include "dlss_swapchain_proxy.h"
#include "dlss_resolution_math.h"
#include "config.h"
#include "logger.h"

#include <atomic>
#include <mutex>
#include <cmath>
#include <cstring>

// ── Supported format check (Task 6.8) ──

// Forward declaration — defined later, used in ResizeBuffers and SwapProxy_Resize
static void ForcePassthroughInternal(const char* reason);

static bool IsSupportedFormat(DXGI_FORMAT fmt) {
    switch (fmt) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
            return true;
        default:
            return false;
    }
}

// ── Module state ──

static ProxyState       g_proxy_state;
static std::mutex       g_proxy_mutex;
static bool             g_hooks_installed       = false;
static bool             g_swapchain_hooks_installed = false;
static ID3D12Device*    g_device                = nullptr;
static IDXGIFactory2*   g_hooked_factory        = nullptr;

// Allocation dimensions (k_max×D) — the actual texture size when pre-allocated.
static uint32_t         g_alloc_width           = 0;
static uint32_t         g_alloc_height          = 0;

// ── DXGI Factory vtable hook typedefs and state (Task 6.2) ──

static constexpr int VTABLE_INDEX_CREATE_SWAPCHAIN                = 10;
static constexpr int VTABLE_INDEX_CREATE_SWAPCHAIN_FOR_HWND       = 15;
static constexpr int VTABLE_INDEX_CREATE_SWAPCHAIN_FOR_COREWINDOW = 16;

using CreateSwapChain_t = HRESULT (STDMETHODCALLTYPE*)(
    IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);

using CreateSwapChainForHwnd_t = HRESULT (STDMETHODCALLTYPE*)(
    IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);

using CreateSwapChainForCoreWindow_t = HRESULT (STDMETHODCALLTYPE*)(
    IDXGIFactory2*, IUnknown*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*,
    IDXGIOutput*, IDXGISwapChain1**);

static CreateSwapChain_t              g_orig_CreateSwapChain              = nullptr;
static CreateSwapChainForHwnd_t       g_orig_CreateSwapChainForHwnd      = nullptr;
static CreateSwapChainForCoreWindow_t g_orig_CreateSwapChainForCoreWindow = nullptr;

// ── IDXGISwapChain vtable hook typedefs and state (Tasks 6.3–6.5) ──

static constexpr int VTABLE_INDEX_GETBUFFER      = 9;
static constexpr int VTABLE_INDEX_GETDESC         = 12;
static constexpr int VTABLE_INDEX_RESIZEBUFFERS   = 13;
static constexpr int VTABLE_INDEX_GETDESC1        = 18;

using GetDesc_t = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*, DXGI_SWAP_CHAIN_DESC*);
using GetDesc1_t = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain1*, DXGI_SWAP_CHAIN_DESC1*);
using GetBuffer_t = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, REFIID, void**);
using ResizeBuffers_t = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

static GetDesc_t        g_orig_GetDesc        = nullptr;
static GetDesc1_t       g_orig_GetDesc1       = nullptr;
static GetBuffer_t      g_orig_GetBuffer      = nullptr;
static ResizeBuffers_t  g_orig_ResizeBuffers  = nullptr;

// ── Vtable patching helpers (same pattern as dlss_mip_corrector.cpp) ──

static void PatchVtable(void** vtable, int index, void* hook, void** original) {
    *original = vtable[index];
    DWORD old_protect = 0;
    VirtualProtect(&vtable[index], sizeof(void*), PAGE_READWRITE, &old_protect);
    vtable[index] = hook;
    VirtualProtect(&vtable[index], sizeof(void*), old_protect, &old_protect);
}

static void RestoreVtable(void** vtable, int index, void* original) {
    if (!original) return;
    DWORD old_protect = 0;
    VirtualProtect(&vtable[index], sizeof(void*), PAGE_READWRITE, &old_protect);
    vtable[index] = original;
    VirtualProtect(&vtable[index], sizeof(void*), old_protect, &old_protect);
}

// ── Proxy backbuffer allocation ──

static bool AllocateProxyBackbuffer(ID3D12Device* device, uint32_t width, uint32_t height,
                                    DXGI_FORMAT format) {
    if (!device || width == 0 || height == 0) return false;

    if (g_proxy_state.proxy_backbuffer) {
        g_proxy_state.proxy_backbuffer->Release();
        g_proxy_state.proxy_backbuffer = nullptr;
    }

    D3D12_HEAP_PROPERTIES heap_props = {};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width              = width;
    desc.Height             = height;
    desc.DepthOrArraySize   = 1;
    desc.MipLevels          = 1;
    desc.Format             = format;
    desc.SampleDesc.Count   = 1;
    desc.Layout             = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags              = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                              D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    D3D12_CLEAR_VALUE clear_value = {};
    clear_value.Format = format;

    HRESULT hr = device->CreateCommittedResource(
        &heap_props, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_RENDER_TARGET, &clear_value,
        IID_PPV_ARGS(&g_proxy_state.proxy_backbuffer));

    if (FAILED(hr)) {
        LOG_ERROR("SwapProxy: proxy backbuffer alloc failed %ux%u fmt=%d (hr=0x%08X)",
                  width, height, static_cast<int>(format), hr);
        return false;
    }

    LOG_INFO("SwapProxy: allocated proxy backbuffer %ux%u fmt=%d",
             width, height, static_cast<int>(format));
    return true;
}

static void ReleaseProxyBackbuffer() {
    if (g_proxy_state.proxy_backbuffer) {
        g_proxy_state.proxy_backbuffer->Release();
        g_proxy_state.proxy_backbuffer = nullptr;
    }
}

// ══════════════════════════════════════════════════════════════════════
// Hooked swapchain methods (Tasks 6.3, 6.4, 6.5, 6.7)
// ══════════════════════════════════════════════════════════════════════

// ── Task 6.3: GetDesc override — return fake output resolution ──

static HRESULT STDMETHODCALLTYPE Hooked_GetDesc(IDXGISwapChain* self,
                                                 DXGI_SWAP_CHAIN_DESC* pDesc) {
    if (!g_orig_GetDesc) return E_FAIL;
    HRESULT hr = g_orig_GetDesc(self, pDesc);
    if (FAILED(hr) || !pDesc) return hr;

    // Task 6.7: passthrough — return real dimensions.
    if (g_proxy_state.passthrough) return hr;

    // Override with fake dimensions (viewport size, not allocation size).
    std::lock_guard<std::mutex> lock(g_proxy_mutex);
    pDesc->BufferDesc.Width  = g_proxy_state.fake_width;
    pDesc->BufferDesc.Height = g_proxy_state.fake_height;
    return hr;
}

// ── Task 6.3: GetDesc1 override — return fake output resolution ──

static HRESULT STDMETHODCALLTYPE Hooked_GetDesc1(IDXGISwapChain1* self,
                                                  DXGI_SWAP_CHAIN_DESC1* pDesc) {
    if (!g_orig_GetDesc1) return E_FAIL;
    HRESULT hr = g_orig_GetDesc1(self, pDesc);
    if (FAILED(hr) || !pDesc) return hr;

    if (g_proxy_state.passthrough) return hr;

    std::lock_guard<std::mutex> lock(g_proxy_mutex);
    pDesc->Width  = g_proxy_state.fake_width;
    pDesc->Height = g_proxy_state.fake_height;
    return hr;
}

// ── Task 6.4: GetBuffer override — return proxy backbuffer ──

static HRESULT STDMETHODCALLTYPE Hooked_GetBuffer(IDXGISwapChain* self,
                                                   UINT Buffer,
                                                   REFIID riid,
                                                   void** ppSurface) {
    if (!g_orig_GetBuffer) return E_FAIL;

    // Task 6.7: passthrough — return real backbuffer.
    if (g_proxy_state.passthrough) {
        return g_orig_GetBuffer(self, Buffer, riid, ppSurface);
    }

    // For buffer index 0, return the proxy backbuffer instead.
    // Games typically only request buffer 0 for the current back buffer.
    if (Buffer == 0 && g_proxy_state.proxy_backbuffer && ppSurface) {
        HRESULT hr = g_proxy_state.proxy_backbuffer->QueryInterface(riid, ppSurface);
        if (SUCCEEDED(hr)) return hr;
        // Fall through to real buffer on QI failure.
        LOG_WARN("SwapProxy: proxy backbuffer QI failed for buffer 0, using real");
    }

    return g_orig_GetBuffer(self, Buffer, riid, ppSurface);
}

// ── Task 6.5: ResizeBuffers interception ──
// Resize proxy backbuffer to new k×D while keeping real swapchain at D.

static HRESULT STDMETHODCALLTYPE Hooked_ResizeBuffers(IDXGISwapChain* self,
                                                       UINT BufferCount,
                                                       UINT Width,
                                                       UINT Height,
                                                       DXGI_FORMAT NewFormat,
                                                       UINT SwapChainFlags) {
    if (!g_orig_ResizeBuffers) return E_FAIL;

    // Task 6.7: passthrough — forward directly.
    if (g_proxy_state.passthrough) {
        return g_orig_ResizeBuffers(self, BufferCount, Width, Height,
                                    NewFormat, SwapChainFlags);
    }

    // Release real backbuffer reference before resize.
    if (g_proxy_state.real_backbuffer) {
        g_proxy_state.real_backbuffer->Release();
        g_proxy_state.real_backbuffer = nullptr;
    }

    // The game passes the fake resolution (k×D) as Width/Height.
    // We resize the real swapchain to the display resolution (D) instead.
    uint32_t display_w, display_h;
    {
        std::lock_guard<std::mutex> lock(g_proxy_mutex);
        display_w = g_proxy_state.display_width;
        display_h = g_proxy_state.display_height;
    }

    // If game passes 0,0 it means "use window size" — let it through as-is.
    UINT real_w = (Width == 0)  ? 0 : display_w;
    UINT real_h = (Height == 0) ? 0 : display_h;

    DXGI_FORMAT fmt = NewFormat;
    if (fmt != DXGI_FORMAT_UNKNOWN && !IsSupportedFormat(fmt)) {
        LOG_WARN("SwapProxy: ResizeBuffers unsupported format %d, passthrough",
                 static_cast<int>(fmt));
        SwapProxy_ForcePassthrough("ResizeBuffers unsupported format");
        return g_orig_ResizeBuffers(self, BufferCount, Width, Height,
                                    NewFormat, SwapChainFlags);
    }

    HRESULT hr = g_orig_ResizeBuffers(self, BufferCount, real_w, real_h,
                                       NewFormat, SwapChainFlags);
    if (FAILED(hr)) {
        LOG_ERROR("SwapProxy: real ResizeBuffers failed (hr=0x%08X)", hr);
        return hr;
    }

    // Update display dimensions if the real resize changed them.
    if (real_w > 0 && real_h > 0) {
        std::lock_guard<std::mutex> lock(g_proxy_mutex);
        g_proxy_state.display_width  = real_w;
        g_proxy_state.display_height = real_h;

        // Update format if changed.
        if (fmt != DXGI_FORMAT_UNKNOWN) {
            g_proxy_state.format = fmt;
        }
    }

    // Re-acquire real backbuffer.
    if (g_proxy_state.real_swapchain) {
        ID3D12Resource* real_bb = nullptr;
        HRESULT hr2 = g_proxy_state.real_swapchain->GetBuffer(0, IID_PPV_ARGS(&real_bb));
        if (SUCCEEDED(hr2) && real_bb) {
            g_proxy_state.real_backbuffer = real_bb;
        }
    }

    // Task 6.6: Pre-allocation — if pre-allocated, just update fake dims
    // (the proxy backbuffer stays at k_max×D, viewport adjusts).
    // If not pre-allocated, reallocate the proxy backbuffer.
    {
        std::lock_guard<std::mutex> lock(g_proxy_mutex);
        if (g_proxy_state.pre_allocated) {
            // Recompute allocation at k_max for new display dims.
            double k_max = g_config.dlss_k_max;
            auto [new_alloc_w, new_alloc_h] = ComputeFakeResolution(
                k_max, g_proxy_state.display_width, g_proxy_state.display_height);

            // Only reallocate if display resolution actually changed.
            if (new_alloc_w != g_alloc_width || new_alloc_h != g_alloc_height) {
                g_alloc_width  = new_alloc_w;
                g_alloc_height = new_alloc_h;

                DXGI_FORMAT alloc_fmt = (fmt != DXGI_FORMAT_UNKNOWN)
                    ? fmt : g_proxy_state.format;

                if (!AllocateProxyBackbuffer(g_device, new_alloc_w, new_alloc_h, alloc_fmt)) {
                    LOG_ERROR("SwapProxy: re-alloc failed on ResizeBuffers, passthrough");
                    ForcePassthroughInternal("re-alloc failed on ResizeBuffers");
                }
            }
            // fake_width/fake_height stay as they are — updated by SwapProxy_Resize.
        } else {
            // Non-pre-allocated: reallocate at current fake dims.
            DXGI_FORMAT alloc_fmt = (fmt != DXGI_FORMAT_UNKNOWN)
                ? fmt : g_proxy_state.format;

            if (!AllocateProxyBackbuffer(g_device,
                    g_proxy_state.fake_width, g_proxy_state.fake_height, alloc_fmt)) {
                LOG_ERROR("SwapProxy: re-alloc failed on ResizeBuffers, passthrough");
                ForcePassthroughInternal("re-alloc failed on ResizeBuffers");
            }
        }
    }

    LOG_INFO("SwapProxy: ResizeBuffers complete — real=%ux%u, fake=%ux%u",
             display_w, display_h, g_proxy_state.fake_width, g_proxy_state.fake_height);
    return hr;
}

// ══════════════════════════════════════════════════════════════════════
// Post-creation setup: hooks swapchain vtable and allocates proxy
// ══════════════════════════════════════════════════════════════════════

static void SetupProxyForSwapchain(IDXGISwapChain* swapchain, IUnknown* pDevice) {
    if (!swapchain || !pDevice || g_swapchain_hooks_installed) return;

    // Get ID3D12Device from the device/command queue passed to CreateSwapChain.
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* cmdq = nullptr;
    if (SUCCEEDED(pDevice->QueryInterface(IID_PPV_ARGS(&device)))) {
        // pDevice was the device directly.
    } else if (SUCCEEDED(pDevice->QueryInterface(IID_PPV_ARGS(&cmdq)))) {
        cmdq->GetDevice(IID_PPV_ARGS(&device));
        cmdq->Release();
    }

    if (!device) {
        LOG_WARN("SwapProxy: could not obtain ID3D12Device, passthrough");
        SwapProxy_ForcePassthrough("no ID3D12Device");
        return;
    }

    // Query IDXGISwapChain3.
    IDXGISwapChain3* sc3 = nullptr;
    HRESULT hr = swapchain->QueryInterface(IID_PPV_ARGS(&sc3));
    if (FAILED(hr) || !sc3) {
        LOG_WARN("SwapProxy: IDXGISwapChain3 not supported, passthrough");
        device->Release();
        SwapProxy_ForcePassthrough("IDXGISwapChain3 not supported");
        return;
    }

    // Get real desc — use the original (unhooked) GetDesc since we haven't
    // hooked this swapchain yet.
    DXGI_SWAP_CHAIN_DESC real_desc = {};
    hr = swapchain->GetDesc(&real_desc);
    if (FAILED(hr)) {
        LOG_ERROR("SwapProxy: GetDesc failed (hr=0x%08X), passthrough", hr);
        sc3->Release();
        device->Release();
        SwapProxy_ForcePassthrough("GetDesc failed");
        return;
    }

    DXGI_FORMAT fmt = real_desc.BufferDesc.Format;
    if (!IsSupportedFormat(fmt)) {
        LOG_WARN("SwapProxy: unsupported format %d, passthrough", static_cast<int>(fmt));
        sc3->Release();
        device->Release();
        SwapProxy_ForcePassthrough("unsupported format");
        return;
    }

    uint32_t display_w = real_desc.BufferDesc.Width;
    uint32_t display_h = real_desc.BufferDesc.Height;

    // Compute default k from config.
    double k_max = g_config.dlss_k_max;
    int num_tiers = static_cast<int>(std::floor((k_max - 1.0) / 0.25)) + 1;
    int tier = g_config.dlss_default_tier;
    if (tier < 0) tier = 0;
    if (tier >= num_tiers) tier = num_tiers - 1;
    double k_default = 1.0 + tier * 0.25;
    if (k_default > k_max) k_default = k_max;

    auto [fake_w, fake_h]   = ComputeFakeResolution(k_default, display_w, display_h);
    auto [alloc_w, alloc_h] = ComputeFakeResolution(k_max, display_w, display_h);

    g_device = device;  // Stored for later reallocation; caller holds ref.

    {
        std::lock_guard<std::mutex> lock(g_proxy_mutex);
        g_proxy_state.real_swapchain  = sc3;
        g_proxy_state.format          = fmt;
        g_proxy_state.display_width   = display_w;
        g_proxy_state.display_height  = display_h;
        g_proxy_state.fake_width      = fake_w;
        g_proxy_state.fake_height     = fake_h;
        g_proxy_state.passthrough     = false;
        g_proxy_state.pre_allocated   = true;
        g_alloc_width  = alloc_w;
        g_alloc_height = alloc_h;
    }

    // Task 6.6: Pre-allocate at k_max×D.
    if (!AllocateProxyBackbuffer(device, alloc_w, alloc_h, fmt)) {
        sc3->Release();
        g_proxy_state.real_swapchain = nullptr;
        SwapProxy_ForcePassthrough("proxy backbuffer allocation failed");
        return;
    }

    // Acquire real backbuffer reference.
    ID3D12Resource* real_bb = nullptr;
    // Use the original GetBuffer (not yet hooked on this swapchain).
    hr = sc3->GetBuffer(0, IID_PPV_ARGS(&real_bb));
    if (SUCCEEDED(hr) && real_bb) {
        g_proxy_state.real_backbuffer = real_bb;
    }

    // Install swapchain vtable hooks.
    void** sc_vtable = *reinterpret_cast<void***>(swapchain);

    PatchVtable(sc_vtable, VTABLE_INDEX_GETDESC,
                reinterpret_cast<void*>(&Hooked_GetDesc),
                reinterpret_cast<void**>(&g_orig_GetDesc));

    PatchVtable(sc_vtable, VTABLE_INDEX_GETBUFFER,
                reinterpret_cast<void*>(&Hooked_GetBuffer),
                reinterpret_cast<void**>(&g_orig_GetBuffer));

    PatchVtable(sc_vtable, VTABLE_INDEX_RESIZEBUFFERS,
                reinterpret_cast<void*>(&Hooked_ResizeBuffers),
                reinterpret_cast<void**>(&g_orig_ResizeBuffers));

    // Hook GetDesc1 via IDXGISwapChain1 interface.
    IDXGISwapChain1* sc1 = nullptr;
    if (SUCCEEDED(swapchain->QueryInterface(IID_PPV_ARGS(&sc1))) && sc1) {
        void** sc1_vtable = *reinterpret_cast<void***>(sc1);
        PatchVtable(sc1_vtable, VTABLE_INDEX_GETDESC1,
                    reinterpret_cast<void*>(&Hooked_GetDesc1),
                    reinterpret_cast<void**>(&g_orig_GetDesc1));
        sc1->Release();
    }

    g_swapchain_hooks_installed = true;

    LOG_INFO("SwapProxy: active — display=%ux%u, fake=%ux%u, alloc=%ux%u, fmt=%d",
             display_w, display_h, fake_w, fake_h, alloc_w, alloc_h,
             static_cast<int>(fmt));
}

// ══════════════════════════════════════════════════════════════════════
// Hooked DXGI factory methods (Task 6.2)
// ══════════════════════════════════════════════════════════════════════

static HRESULT STDMETHODCALLTYPE Hooked_CreateSwapChain(
    IDXGIFactory* self, IUnknown* pDevice,
    DXGI_SWAP_CHAIN_DESC* pDesc, IDXGISwapChain** ppSwapChain)
{
    if (!g_orig_CreateSwapChain) return E_FAIL;

    // Let the real swapchain be created at the display resolution.
    HRESULT hr = g_orig_CreateSwapChain(self, pDevice, pDesc, ppSwapChain);
    if (FAILED(hr) || !ppSwapChain || !*ppSwapChain) return hr;

    if (g_config.adaptive_dlss_scaling && pDevice) {
        LOG_INFO("SwapProxy: CreateSwapChain intercepted (%ux%u)",
                 pDesc ? pDesc->BufferDesc.Width : 0,
                 pDesc ? pDesc->BufferDesc.Height : 0);
        SetupProxyForSwapchain(*ppSwapChain, pDevice);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hooked_CreateSwapChainForHwnd(
    IDXGIFactory2* self, IUnknown* pDevice, HWND hWnd,
    const DXGI_SWAP_CHAIN_DESC1* pDesc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc,
    IDXGIOutput* pRestrictToOutput, IDXGISwapChain1** ppSwapChain)
{
    if (!g_orig_CreateSwapChainForHwnd) return E_FAIL;

    HRESULT hr = g_orig_CreateSwapChainForHwnd(
        self, pDevice, hWnd, pDesc, pFullscreenDesc, pRestrictToOutput, ppSwapChain);
    if (FAILED(hr) || !ppSwapChain || !*ppSwapChain) return hr;

    if (g_config.adaptive_dlss_scaling && pDevice) {
        LOG_INFO("SwapProxy: CreateSwapChainForHwnd intercepted (%ux%u)",
                 pDesc ? pDesc->Width : 0, pDesc ? pDesc->Height : 0);
        SetupProxyForSwapchain(reinterpret_cast<IDXGISwapChain*>(*ppSwapChain), pDevice);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hooked_CreateSwapChainForCoreWindow(
    IDXGIFactory2* self, IUnknown* pDevice, IUnknown* pWindow,
    const DXGI_SWAP_CHAIN_DESC1* pDesc,
    IDXGIOutput* pRestrictToOutput, IDXGISwapChain1** ppSwapChain)
{
    if (!g_orig_CreateSwapChainForCoreWindow) return E_FAIL;

    HRESULT hr = g_orig_CreateSwapChainForCoreWindow(
        self, pDevice, pWindow, pDesc, pRestrictToOutput, ppSwapChain);
    if (FAILED(hr) || !ppSwapChain || !*ppSwapChain) return hr;

    if (g_config.adaptive_dlss_scaling && pDevice) {
        LOG_INFO("SwapProxy: CreateSwapChainForCoreWindow intercepted (%ux%u)",
                 pDesc ? pDesc->Width : 0, pDesc ? pDesc->Height : 0);
        SetupProxyForSwapchain(reinterpret_cast<IDXGISwapChain*>(*ppSwapChain), pDevice);
    }
    return hr;
}

// ══════════════════════════════════════════════════════════════════════
// Public API
// ══════════════════════════════════════════════════════════════════════

void SwapProxy_Init() {
    if (g_hooks_installed) {
        LOG_WARN("SwapProxy: already initialized, skipping");
        return;
    }

    if (!g_config.adaptive_dlss_scaling) {
        LOG_INFO("SwapProxy: adaptive DLSS scaling disabled, skipping init");
        return;
    }

    // Create a temporary DXGI factory to hook its vtable.
    IDXGIFactory2* factory = nullptr;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr) || !factory) {
        LOG_ERROR("SwapProxy: CreateDXGIFactory1 failed (hr=0x%08X)", hr);
        return;
    }

    void** factory_vtable = *reinterpret_cast<void***>(factory);

    PatchVtable(factory_vtable, VTABLE_INDEX_CREATE_SWAPCHAIN,
                reinterpret_cast<void*>(&Hooked_CreateSwapChain),
                reinterpret_cast<void**>(&g_orig_CreateSwapChain));

    PatchVtable(factory_vtable, VTABLE_INDEX_CREATE_SWAPCHAIN_FOR_HWND,
                reinterpret_cast<void*>(&Hooked_CreateSwapChainForHwnd),
                reinterpret_cast<void**>(&g_orig_CreateSwapChainForHwnd));

    PatchVtable(factory_vtable, VTABLE_INDEX_CREATE_SWAPCHAIN_FOR_COREWINDOW,
                reinterpret_cast<void*>(&Hooked_CreateSwapChainForCoreWindow),
                reinterpret_cast<void**>(&g_orig_CreateSwapChainForCoreWindow));

    g_hooked_factory = factory;  // Keep alive so vtable stays valid.
    g_hooks_installed = true;

    LOG_INFO("SwapProxy: DXGI factory hooks installed");
}

void SwapProxy_Shutdown() {
    // Restore swapchain vtable hooks.
    if (g_swapchain_hooks_installed && g_proxy_state.real_swapchain) {
        IDXGISwapChain* sc = reinterpret_cast<IDXGISwapChain*>(g_proxy_state.real_swapchain);
        void** sc_vtable = *reinterpret_cast<void***>(sc);

        RestoreVtable(sc_vtable, VTABLE_INDEX_GETDESC,
                      reinterpret_cast<void*>(g_orig_GetDesc));
        RestoreVtable(sc_vtable, VTABLE_INDEX_GETBUFFER,
                      reinterpret_cast<void*>(g_orig_GetBuffer));
        RestoreVtable(sc_vtable, VTABLE_INDEX_RESIZEBUFFERS,
                      reinterpret_cast<void*>(g_orig_ResizeBuffers));

        // Restore GetDesc1 if hooked.
        if (g_orig_GetDesc1) {
            IDXGISwapChain1* sc1 = nullptr;
            if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc1))) && sc1) {
                void** sc1_vtable = *reinterpret_cast<void***>(sc1);
                RestoreVtable(sc1_vtable, VTABLE_INDEX_GETDESC1,
                              reinterpret_cast<void*>(g_orig_GetDesc1));
                sc1->Release();
            }
        }

        g_swapchain_hooks_installed = false;
    }

    // Restore factory vtable hooks.
    if (g_hooks_installed && g_hooked_factory) {
        void** factory_vtable = *reinterpret_cast<void***>(g_hooked_factory);

        RestoreVtable(factory_vtable, VTABLE_INDEX_CREATE_SWAPCHAIN,
                      reinterpret_cast<void*>(g_orig_CreateSwapChain));
        RestoreVtable(factory_vtable, VTABLE_INDEX_CREATE_SWAPCHAIN_FOR_HWND,
                      reinterpret_cast<void*>(g_orig_CreateSwapChainForHwnd));
        RestoreVtable(factory_vtable, VTABLE_INDEX_CREATE_SWAPCHAIN_FOR_COREWINDOW,
                      reinterpret_cast<void*>(g_orig_CreateSwapChainForCoreWindow));

        g_hooked_factory->Release();
        g_hooked_factory = nullptr;
        g_hooks_installed = false;
    }

    // Release proxy resources.
    ReleaseProxyBackbuffer();

    if (g_proxy_state.real_backbuffer) {
        g_proxy_state.real_backbuffer->Release();
        g_proxy_state.real_backbuffer = nullptr;
    }

    if (g_proxy_state.real_swapchain) {
        g_proxy_state.real_swapchain->Release();
        g_proxy_state.real_swapchain = nullptr;
    }

    // Reset all state.
    g_orig_GetDesc       = nullptr;
    g_orig_GetDesc1      = nullptr;
    g_orig_GetBuffer     = nullptr;
    g_orig_ResizeBuffers = nullptr;
    g_orig_CreateSwapChain              = nullptr;
    g_orig_CreateSwapChainForHwnd       = nullptr;
    g_orig_CreateSwapChainForCoreWindow = nullptr;
    g_device = nullptr;
    g_alloc_width  = 0;
    g_alloc_height = 0;

    {
        std::lock_guard<std::mutex> lock(g_proxy_mutex);
        g_proxy_state = ProxyState{};
    }

    LOG_INFO("SwapProxy: shutdown complete");
}

// ── Task 6.5/6.6: SwapProxy_Resize — called by K_Controller on tier transition ──

void SwapProxy_Resize(uint32_t new_fake_w, uint32_t new_fake_h) {
    if (g_proxy_state.passthrough) return;

    std::lock_guard<std::mutex> lock(g_proxy_mutex);

    if (g_proxy_state.pre_allocated) {
        // Pre-allocation strategy (Task 6.6): just update viewport dimensions.
        // The proxy backbuffer stays at k_max×D; viewport/scissor adjusts.
        if (new_fake_w > g_alloc_width || new_fake_h > g_alloc_height) {
            LOG_ERROR("SwapProxy: resize %ux%u exceeds allocation %ux%u, clamping",
                      new_fake_w, new_fake_h, g_alloc_width, g_alloc_height);
            new_fake_w = (new_fake_w > g_alloc_width)  ? g_alloc_width  : new_fake_w;
            new_fake_h = (new_fake_h > g_alloc_height) ? g_alloc_height : new_fake_h;
        }

        g_proxy_state.fake_width  = new_fake_w;
        g_proxy_state.fake_height = new_fake_h;

        LOG_INFO("SwapProxy: viewport resized to %ux%u (alloc=%ux%u)",
                 new_fake_w, new_fake_h, g_alloc_width, g_alloc_height);
    } else {
        // Non-pre-allocated: reallocate the proxy backbuffer.
        g_proxy_state.fake_width  = new_fake_w;
        g_proxy_state.fake_height = new_fake_h;

        if (!AllocateProxyBackbuffer(g_device, new_fake_w, new_fake_h,
                                     g_proxy_state.format)) {
            LOG_ERROR("SwapProxy: resize realloc failed, passthrough");
            ForcePassthroughInternal("resize realloc failed");
            return;
        }

        LOG_INFO("SwapProxy: reallocated proxy backbuffer to %ux%u",
                 new_fake_w, new_fake_h);
    }
}

ProxyState SwapProxy_GetState() {
    std::lock_guard<std::mutex> lock(g_proxy_mutex);
    return g_proxy_state;
}

bool SwapProxy_IsActive() {
    return g_hooks_installed && g_swapchain_hooks_installed && !g_proxy_state.passthrough;
}

bool SwapProxy_IsInitialized() {
    return g_hooks_installed;
}

// ── Internal passthrough (no lock — caller must hold g_proxy_mutex or be safe) ──

static void ForcePassthroughInternal(const char* reason) {
    if (g_proxy_state.passthrough) return;
    g_proxy_state.passthrough = true;
    LOG_WARN("SwapProxy: entering passthrough mode — %s", reason ? reason : "unknown");
}

// ── Task 6.7: Passthrough mode — revert all overrides within one frame ──

void SwapProxy_ForcePassthrough(const char* reason) {
    std::lock_guard<std::mutex> lock(g_proxy_mutex);
    ForcePassthroughInternal(reason);

    // Next GetDesc returns real display dimensions (passthrough check in hook).
    // Next GetBuffer returns real swapchain backbuffer (passthrough check in hook).
    // Lanczos dispatch is skipped by caller checking SwapProxy_IsActive().
    // K_Controller stops tier evaluation by checking SwapProxy_IsActive().
    // All state changes complete within one frame — the passthrough flag is
    // checked at the top of each hooked function.
}
