#pragma once

#include <Windows.h>
#include <Unknwn.h>

// COM devices only. Normalizes IUnknown identity through the explicit ReShade
// IID_UnwrappedObject contract; never infers equivalence from API/adapter LUID.
// Inputs are borrowed and must remain alive for the call. No heap allocation.
bool FeedbackDeviceIdentity_IsSame(IUnknown* left, IUnknown* right) noexcept;