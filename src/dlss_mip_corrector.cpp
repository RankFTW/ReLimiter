/**
 * Mip_Corrector implementation — ID3D12Device::CreateSampler vtable hook.
 *
 * Intercepts CreateSampler calls to apply corrected mip LOD bias log2(s × k)
 * to samplers that already have a non-zero MipLODBias (game-set DLSS bias).
 * Zero-bias samplers are passed through unchanged.
 *
 * Also detects static samplers in root signatures and logs a warning,
 * since those cannot be corrected at runtime.
 *
 * Feature: adaptive-dlss-scaling
 */

#include "dlss_mip_corrector.h"
#include "dlss_resolution_math.h"
#include "logger.h"

#include <atomic>
#include <cstring>
#include <cmath>

// ── Module state ──

static std::atomic<double> g_current_bias{0.0};
static ID3D12Device*       g_device = nullptr;
static bool                g_hooked = false;

// ── Vtable hook state ──

// CreateSampler is index 22 in the ID3D12Device vtable.
// CreateRootSignature is index 12 in the ID3D12Device vtable.
static constexpr int VTABLE_INDEX_CREATE_SAMPLER         = 22;
static constexpr int VTABLE_INDEX_CREATE_ROOT_SIGNATURE  = 12;

using CreateSampler_t = void (STDMETHODCALLTYPE*)(
    ID3D12Device* self,
    const D3D12_SAMPLER_DESC* pDesc,
    D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor);

using CreateRootSignature_t = HRESULT (STDMETHODCALLTYPE*)(
    ID3D12Device* self,
    UINT nodeMask,
    const void* pBlobWithRootSignature,
    SIZE_T blobLengthInBytes,
    REFIID riid,
    void** ppvRootSignature);

static CreateSampler_t        g_original_create_sampler        = nullptr;
static CreateRootSignature_t  g_original_create_root_signature = nullptr;

// ── Hooked CreateSampler ──

static void STDMETHODCALLTYPE Hooked_CreateSampler(
    ID3D12Device* self,
    const D3D12_SAMPLER_DESC* pDesc,
    D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor)
{
    if (!pDesc || !g_original_create_sampler) {
        if (g_original_create_sampler)
            g_original_create_sampler(self, pDesc, DestDescriptor);
        return;
    }

    // Only modify samplers with non-zero MipLODBias (game already set a DLSS bias).
    if (pDesc->MipLODBias != 0.0f) {
        double bias = g_current_bias.load(std::memory_order_relaxed);
        D3D12_SAMPLER_DESC modified = *pDesc;
        modified.MipLODBias = static_cast<float>(bias);
        g_original_create_sampler(self, &modified, DestDescriptor);
        return;
    }

    // Zero-bias sampler — pass through unchanged.
    g_original_create_sampler(self, pDesc, DestDescriptor);
}

// ── Hooked CreateRootSignature (Task 5.3: static sampler detection) ──

static HRESULT STDMETHODCALLTYPE Hooked_CreateRootSignature(
    ID3D12Device* self,
    UINT nodeMask,
    const void* pBlobWithRootSignature,
    SIZE_T blobLengthInBytes,
    REFIID riid,
    void** ppvRootSignature)
{
    // Attempt to deserialize the root signature to inspect static samplers.
    // We use D3D12CreateRootSignatureDeserializer to peek at the description.
    ID3D12RootSignatureDeserializer* deserializer = nullptr;
    HRESULT hr_deser = D3D12CreateRootSignatureDeserializer(
        pBlobWithRootSignature, blobLengthInBytes,
        IID_PPV_ARGS(&deserializer));

    if (SUCCEEDED(hr_deser) && deserializer) {
        const D3D12_ROOT_SIGNATURE_DESC* desc = deserializer->GetRootSignatureDesc();
        if (desc && desc->NumStaticSamplers > 0) {
            for (UINT i = 0; i < desc->NumStaticSamplers; ++i) {
                const D3D12_STATIC_SAMPLER_DESC& ss = desc->pStaticSamplers[i];
                if (ss.MipLODBias != 0.0f) {
                    LOG_WARN("MipCorrector: root signature has static sampler "
                             "(register s%u, space %u) with MipLODBias=%.3f — "
                             "cannot correct at runtime",
                             ss.ShaderRegister, ss.RegisterSpace, ss.MipLODBias);
                }
            }
        }
        deserializer->Release();
    }

    // Always call the original to actually create the root signature.
    return g_original_create_root_signature(self, nodeMask,
        pBlobWithRootSignature, blobLengthInBytes, riid, ppvRootSignature);
}

// ── Vtable patching helpers ──

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

// ── Public API ──

void MipCorrector_Init(ID3D12Device* device) {
    if (!device) {
        LOG_ERROR("MipCorrector: Init called with null device");
        return;
    }

    if (g_hooked) {
        LOG_WARN("MipCorrector: already initialized, skipping");
        return;
    }

    g_device = device;

    // Get vtable pointer from the device COM object.
    void** vtable = *reinterpret_cast<void***>(device);

    PatchVtable(vtable, VTABLE_INDEX_CREATE_SAMPLER,
                reinterpret_cast<void*>(&Hooked_CreateSampler),
                reinterpret_cast<void**>(&g_original_create_sampler));

    PatchVtable(vtable, VTABLE_INDEX_CREATE_ROOT_SIGNATURE,
                reinterpret_cast<void*>(&Hooked_CreateRootSignature),
                reinterpret_cast<void**>(&g_original_create_root_signature));

    g_hooked = true;
    LOG_INFO("MipCorrector: initialized, CreateSampler + CreateRootSignature hooks installed");
}

void MipCorrector_Shutdown() {
    if (!g_hooked || !g_device) {
        return;
    }

    void** vtable = *reinterpret_cast<void***>(g_device);

    RestoreVtable(vtable, VTABLE_INDEX_CREATE_SAMPLER,
                  reinterpret_cast<void*>(g_original_create_sampler));
    RestoreVtable(vtable, VTABLE_INDEX_CREATE_ROOT_SIGNATURE,
                  reinterpret_cast<void*>(g_original_create_root_signature));

    g_original_create_sampler        = nullptr;
    g_original_create_root_signature = nullptr;
    g_device = nullptr;
    g_hooked = false;
    g_current_bias.store(0.0, std::memory_order_relaxed);

    LOG_INFO("MipCorrector: shutdown, hooks removed");
}

void MipCorrector_Update(double new_bias) {
    g_current_bias.store(new_bias, std::memory_order_relaxed);
    LOG_INFO("MipCorrector: bias updated to %.4f", new_bias);
}

double MipCorrector_GetBias() {
    return g_current_bias.load(std::memory_order_relaxed);
}
