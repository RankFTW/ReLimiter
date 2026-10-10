#include "streamline_discovery.h"
#include "streamline_queue_hooks.h"
#include "streamline_gpu_handoff.h"
#include "hooks.h"
#include "logger.h"
#include "runtime_lifecycle.h"
#include <atomic>

namespace {
constexpr uint32_t kDLSSGFeature = 1000;
struct DiscoveryIdentity {
    HMODULE interposer = nullptr;
    StreamlineResolvedFeatures functions{};
};
DiscoveryIdentity last_identity; // installer-worker owned
bool identity_observed = false;
std::atomic<uint64_t> discovery_revision{0};

int ResolveFeature(StreamlineFeatureResolver resolve, const char* name, void** output) noexcept {
    *output = nullptr;
    if (!resolve) return -1;
    __try { return resolve(kDLSSGFeature, name, output); }
    __except (EXCEPTION_EXECUTE_HANDLER) { *output = nullptr; return -1; }
}

StreamlineResolvedFeatures ResolveFeatures(StreamlineFeatureResolver resolve) noexcept {
    StreamlineResolvedFeatures functions;
    functions.options_result = ResolveFeature(resolve, "slDLSSGSetOptions", &functions.set_options);
    functions.state_result = ResolveFeature(resolve, "slDLSSGGetState", &functions.get_state);
    if (functions.options_result != 0) functions.set_options = nullptr;
    if (functions.state_result != 0) functions.get_state = nullptr;
    return functions;
}

bool SameFunctions(const StreamlineResolvedFeatures& a, const StreamlineResolvedFeatures& b) noexcept {
    return a.set_options == b.set_options && a.get_state == b.get_state &&
        a.options_result == b.options_result && a.state_result == b.state_result;
}

void LogFunctionOwner(const char* name, void* address) noexcept {
    if (!address) return;
    HMODULE owner = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(address), &owner)) return;
    wchar_t path[1024]{};
    const DWORD length = GetModuleFileNameW(owner, path, 1024);
    LOG_INFO("Streamline discovery: function=%s address=%p owner=%p path=%ls",
        name, address, owner, length && length < 1024 ? path : L"unavailable/truncated");
    FreeLibrary(owner);
}

}

StreamlineResolvedFeatures StreamlineDiscovery_Resolve(
    HMODULE interposer, StreamlineFeatureResolver resolve) noexcept {
    if (RuntimeLifecycle_IsStopping()) return {};
    auto functions = ResolveFeatures(resolve);
    if (identity_observed && interposer == last_identity.interposer &&
        SameFunctions(functions, last_identity.functions)) return functions;

    // Pinning only preserves the image, not feature activity. Re-resolve after
    // pinning so an implementation switch cannot publish mixed entry points.
    const bool pinned = (!functions.set_options || Hooks_PinTargetModule(functions.set_options)) &&
        (!functions.get_state || Hooks_PinTargetModule(functions.get_state));
    if (!pinned || !SameFunctions(functions, ResolveFeatures(resolve))) {
        functions = {};
        functions.options_result = functions.state_result = -2; // local: unstable/unpinned
    }
    if (identity_observed && interposer == last_identity.interposer &&
        SameFunctions(functions, last_identity.functions)) return functions;

    const bool changed = identity_observed;
    last_identity = {interposer, functions};
    identity_observed = true;
    const auto revision = discovery_revision.fetch_add(1, std::memory_order_acq_rel) + 1;
    StreamlineQueue_SetFeatureModule(functions.set_options, functions.get_state);
    const char* capability = functions.set_options && functions.get_state ? "resolved" :
        functions.set_options || functions.get_state ? "partial" : "unavailable";
    LOG_INFO("Streamline discovery: revision=%llu interposer=%p options_result=%d state_result=%d "
        "changed=%d functions=%s (queue qualification independent)",
        revision, interposer, functions.options_result, functions.state_result, changed, capability);
    LogFunctionOwner("slDLSSGSetOptions", functions.set_options);
    LogFunctionOwner("slDLSSGGetState", functions.get_state);
    return functions;
}

void StreamlineDiscovery_RecordBoundary(const StreamlineBoundaryEvidence& evidence) noexcept {
    if (RuntimeLifecycle_IsStopping()) return;
    StreamlineHandoff_Boundary(evidence);
}
