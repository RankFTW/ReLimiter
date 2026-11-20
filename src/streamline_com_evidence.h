#pragma once
#include <array>
#include <cstdint>

// Copied implementation addresses, not object ownership. Slots 2..4 are
// factory CreateSwapChain/Hwnd/CoreWindow; 6..8 are queue Execute/Signal/Wait.
struct StreamlineComEvidence {
    std::array<uint64_t, 9> entries{};
};
