// GPU output transport helpers (gpu_present_types.h).
#include "gpu_present_types.h"
#include <cstring>

namespace {
// Retired ready-present gate settings, fixed at their defaults: mode bits 0-1
// off, slack bits 2-12 = 500 us, hold bits 13-26 = 8000 us, generation 0.
constexpr uint64_t kSettings=(500ull<<2) | (8000ull<<13);
}
bool ReadyPresent_SameIdentity(const ReadyPresentIdentity& a,const ReadyPresentIdentity& b) noexcept {
    return a.config==b.config && a.observer==b.observer && a.prediction==b.prediction && a.swapchain==b.swapchain &&
        a.queue==b.queue && a.producer==b.producer && a.controller==b.controller && a.capture==b.capture && a.thread==b.thread && a.runtime==b.runtime;
}
ReadyPresentConfig ReadyPresent_GetConfig() noexcept {
    return {static_cast<ReadyPresentMode>(kSettings&3),static_cast<double>((kSettings>>2)&2047),
        static_cast<double>((kSettings>>13)&16383),kSettings};
}
ReadyPresentMode ReadyPresent_ModeFromText(const char* text) noexcept {
    if(text && std::strcmp(text,"active")==0)return ReadyPresentMode::Active;
    if(text && std::strcmp(text,"shadow")==0)return ReadyPresentMode::Shadow;
    return ReadyPresentMode::Off;
}
const char* ReadyPresent_ModeText(ReadyPresentMode mode) noexcept {
    return mode==ReadyPresentMode::Active?"active":mode==ReadyPresentMode::Shadow?"shadow":"off";
}
