#include "feedback_device_identity.h"

#include <d3d11.h>
#include <d3d12.h>

namespace {
// deps/reshade/source/com_utils.hpp: IID_UnwrappedObject.
// d3d12/d3d12_device.cpp (also d3d11/d3d11_device.cpp) explicitly AddRefs
// _orig before returning it, and answers it without ReShade's device lock.
// Keep this first-party adapter independent of
// ReShade's internal template/header dependencies. This is NOT a Streamline IID.
constexpr GUID kReShadeUnwrappedObject = {
    0x7f2c9a11, 0x3b4e, 0x4d6a,
    { 0x81, 0x2f, 0x5e, 0x9c, 0xd3, 0x7a, 0x1b, 0x42 }
};
// The class GUIDs under which ReShade's device proxies register themselves in the native device's private data
// (d3d12/d3d12_device.hpp: D3D12Device, d3d11/d3d11_device.hpp: D3D11Device; SetPrivateData in their constructors).
constexpr GUID kReShadeD3D12DeviceProxy = {
    0x2523AFF4, 0x978B, 0x4939, { 0xBA, 0x16, 0x8E, 0xE8, 0x76, 0xA4, 0xCB, 0x2A }
};
constexpr GUID kReShadeD3D11DeviceProxy = {
    0x72299288, 0x2C68, 0x4AD8, { 0x94, 0x5D, 0x2B, 0xFB, 0x5A, 0xA5, 0xC6, 0x09 }
};
constexpr size_t kMaxIdentityLayers = 8;

void ReleaseIdentity(IUnknown* device) noexcept {
    if (!device) return;
    __try { device->Release(); }
    __except(EXCEPTION_EXECUTE_HANDLER) {}
}

HRESULT QueryIdentity(IUnknown* device, REFIID iid, IUnknown** result) noexcept {
    *result = nullptr;
    HRESULT status = E_FAIL;
    __try {
        status = device->QueryInterface(iid, reinterpret_cast<void**>(result));
    }
    __except(EXCEPTION_EXECUTE_HANDLER) { status = E_FAIL; }
    if (FAILED(status)) {
        ReleaseIdentity(*result);
        *result = nullptr;
    }
    return status;
}

// GetPrivateData of the ReShade proxy pointer on one interface of a native device. False: absent or not that shape.
bool ReadProxyPointer12(IUnknown* native, IUnknown** proxy) noexcept {
    ID3D12Object* object = nullptr;
    bool found = false;
    __try {
        if (SUCCEEDED(native->QueryInterface(__uuidof(ID3D12Object), reinterpret_cast<void**>(&object))) && object) {
            void* data = nullptr;
            UINT size = sizeof(data);
            if (SUCCEEDED(object->GetPrivateData(kReShadeD3D12DeviceProxy, &size, &data)) && size == sizeof(data) && data) {
                *proxy = reinterpret_cast<IUnknown*>(data);
                found = true;
            }
            object->Release();
        }
    }
    __except(EXCEPTION_EXECUTE_HANDLER) { found = false; }
    return found;
}

bool ReadProxyPointer11(IUnknown* native, IUnknown** proxy) noexcept {
    ID3D11Device* device = nullptr;
    bool found = false;
    __try {
        if (SUCCEEDED(native->QueryInterface(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device))) && device) {
            void* data = nullptr;
            UINT size = sizeof(data);
            if (SUCCEEDED(device->GetPrivateData(kReShadeD3D11DeviceProxy, &size, &data)) && size == sizeof(data) && data) {
                *proxy = reinterpret_cast<IUnknown*>(data);
                found = true;
            }
            device->Release();
        }
    }
    __except(EXCEPTION_EXECUTE_HANDLER) { found = false; }
    return found;
}
} // namespace

IUnknown* DeviceIdentity_AcquireNative(IUnknown* incoming) noexcept {
    if (!incoming) return nullptr;
    IUnknown* layers[kMaxIdentityLayers] = {};   // the raw pointers of the layers seen, for cycle detection only
    IUnknown* current = incoming; // First layer is borrowed from the caller.
    bool owns_current = false;
    for (size_t layer = 0; layer < kMaxIdentityLayers; ++layer) {
        layers[layer] = current;
        IUnknown* unwrapped = nullptr;
        const HRESULT status = QueryIdentity(current, kReShadeUnwrappedObject, &unwrapped);
        if (status == E_NOINTERFACE) {
            // Not a layer over another object: terminal.
        } else if (FAILED(status) || !unwrapped) {
            if (owns_current) ReleaseIdentity(current);
            return nullptr;
        } else if (unwrapped == current) {
            // A self-returning unwrap is a terminal identity, not a new layer: drop its extra reference.
            ReleaseIdentity(unwrapped);
        } else {
            for (size_t previous = 0; previous <= layer; ++previous) {
                if (layers[previous] != unwrapped) continue;
                // Longer cycles are malformed and must not broaden admission.
                ReleaseIdentity(unwrapped);
                if (owns_current) ReleaseIdentity(current);
                return nullptr;
            }
            if (owns_current) ReleaseIdentity(current);
            current = unwrapped;
            owns_current = true;
            continue;
        }
        // The terminal object: its canonical identity, the only IUnknown query of the whole chain.
        IUnknown* identity = nullptr;
        QueryIdentity(current, __uuidof(IUnknown), &identity);
        if (owns_current) ReleaseIdentity(current);
        return identity;
    }
    if (owns_current) ReleaseIdentity(current);
    return nullptr; // Incomplete normalization: fail closed at the depth bound.
}

IUnknown* DeviceIdentity_AcquireCanonicalOfNative(IUnknown* native) noexcept {
    if (!native) return nullptr;
    IUnknown* identity = nullptr;
    QueryIdentity(native, __uuidof(IUnknown), &identity);
    return identity;
}

uint64_t DeviceIdentity_OfNative(uint64_t native) noexcept {
    IUnknown* identity = DeviceIdentity_AcquireCanonicalOfNative(reinterpret_cast<IUnknown*>(native));
    const uint64_t value = reinterpret_cast<uint64_t>(identity);
    ReleaseIdentity(identity);
    return value;
}

IUnknown* DeviceIdentity_ReShadeProxyOf(IUnknown* native) noexcept {
    if (!native) return nullptr;
    IUnknown* proxy = nullptr;
    if (ReadProxyPointer12(native, &proxy)) return proxy;
    if (ReadProxyPointer11(native, &proxy)) return proxy;
    return nullptr;
}

bool FeedbackDeviceIdentity_IsSame(IUnknown* left, IUnknown* right) noexcept {
    if (!left || !right) return false;
    if (left == right) return true;
    IUnknown* left_identity = DeviceIdentity_AcquireNative(left);
    IUnknown* right_identity = DeviceIdentity_AcquireNative(right);
    const bool matches = left_identity && left_identity == right_identity;
    ReleaseIdentity(left_identity);
    ReleaseIdentity(right_identity);
    return matches;
}
