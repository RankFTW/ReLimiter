#include "feedback_device_identity.h"

#include <array>

namespace {
// deps/reshade/source/com_utils.hpp: IID_UnwrappedObject.
// d3d12/d3d12_device.cpp (also d3d11/d3d11_device.cpp) explicitly AddRefs
// _orig before returning it. Keep this first-party adapter independent of
// ReShade's internal template/header dependencies. This is NOT a Streamline IID.
constexpr GUID kReShadeUnwrappedObject = {
    0x7f2c9a11, 0x3b4e, 0x4d6a,
    { 0x81, 0x2f, 0x5e, 0x9c, 0xd3, 0x7a, 0x1b, 0x42 }
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

struct IdentityChain {
    // Retaining each canonical identity prevents address reuse while checking
    // cycles. Every successful QI reference has exactly one owner here or below.
    std::array<IUnknown*, kMaxIdentityLayers> identities{};
    ~IdentityChain() noexcept {
        for (IUnknown* identity : identities) ReleaseIdentity(identity);
    }
};

IUnknown* AcquireNormalizedIdentity(IUnknown* device) noexcept {
    IdentityChain chain;
    IUnknown* current = device; // First layer is borrowed from the caller.
    bool owns_current = false;
    for (size_t layer = 0; layer < kMaxIdentityLayers; ++layer) {
        IUnknown* identity = nullptr;
        const HRESULT identity_status = QueryIdentity(
            current, __uuidof(IUnknown), &identity);
        if (FAILED(identity_status) || !identity) {
            if (owns_current) ReleaseIdentity(current);
            return nullptr;
        }
        for (size_t previous = 0; previous < layer; ++previous) {
            if (chain.identities[previous] != identity) continue;
            if (owns_current) ReleaseIdentity(current);
            // A self-returning unwrap is a terminal identity, not a new layer.
            // Longer cycles are malformed and must not broaden admission.
            if (previous + 1 == layer) return identity;
            ReleaseIdentity(identity);
            return nullptr;
        }
        chain.identities[layer] = identity;

        IUnknown* unwrapped = nullptr;
        const HRESULT unwrap_status = QueryIdentity(
            current, kReShadeUnwrappedObject, &unwrapped);
        if (owns_current) ReleaseIdentity(current);
        if (unwrap_status == E_NOINTERFACE) {
            chain.identities[layer] = nullptr;
            return identity;
        }
        if (FAILED(unwrap_status) || !unwrapped) return nullptr;
        current = unwrapped;
        owns_current = true;
    }
    ReleaseIdentity(current);
    return nullptr; // Incomplete normalization: fail closed at the depth bound.
}
} // namespace

bool FeedbackDeviceIdentity_IsSame(IUnknown* left, IUnknown* right) noexcept {
    if (!left || !right) return false;
    if (left == right) return true;
    IUnknown* left_identity = AcquireNormalizedIdentity(left);
    IUnknown* right_identity = AcquireNormalizedIdentity(right);
    const bool matches = left_identity && left_identity == right_identity;
    ReleaseIdentity(left_identity);
    ReleaseIdentity(right_identity);
    return matches;
}