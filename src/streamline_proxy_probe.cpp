#include "streamline_proxy_probe.h"
#include "streamline_proxy_search.h"
#include "streamline_queue_hooks.h"
#include "streamline_gpu_handoff.h"
#include "hooks.h"
#include "logger.h"
#include "runtime_lifecycle.h"
#include <dxgi1_2.h>
#include <atomic>
#include <cstdio>
#include <cstring>

namespace streamline_proxy_probe {
using Present = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1 = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
std::atomic<Present> original_present{nullptr};
std::atomic<Present1> original_present1{nullptr};
std::atomic<uint64_t> expected_vtable{0};
std::atomic<unsigned> queue_offer_state{0};
StreamlineComEvidence queue_methods; // single producer after CAS, release published
// The installer worker writes the cause once, then publishes the flag.
char unavailable_cause[160] = {};
std::atomic<bool> unavailable{false};
uint64_t ReadVtable(const void* object) noexcept {
    if (!object) return 0;
    __try { return *static_cast<const uint64_t*>(object); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
bool CopyQueue(uint64_t queue, StreamlineComEvidence* methods) noexcept {
    __try {
        const auto* table = *reinterpret_cast<const uint64_t* const*>(queue);
        methods->entries[6] = table[10]; methods->entries[7] = table[14]; methods->entries[8] = table[15];
        return methods->entries[6] && methods->entries[7] && methods->entries[8];
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
template<class Forward> HRESULT Observe(void* proxy, UINT sync, UINT flags, Forward forward) {
    const DWORD incoming = GetLastError();
    RuntimeCallbackGuard callback;
    if (callback.IsStopping()) { SetLastError(incoming); return forward(); }
    const auto table = ReadVtable(proxy);
    const bool valid = table && table == expected_vtable.load(std::memory_order_acquire);
    StreamlineInputScope input(valid ? reinterpret_cast<uint64_t>(proxy) : 0, sync, flags);
    SetLastError(incoming);
    const HRESULT result = forward();
    input.Complete(result);
    return result; // scope preserves the original's LastError, including unwinding
}
HRESULT STDMETHODCALLTYPE ObservePresent(IDXGISwapChain* proxy, UINT sync, UINT flags) {
    return Observe(proxy, sync, flags, [&] { return original_present.load(std::memory_order_acquire)(proxy, sync, flags); });
}
HRESULT STDMETHODCALLTYPE ObservePresent1(IDXGISwapChain1* proxy, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* parameters) {
    return Observe(proxy, sync, flags, [&] { return original_present1.load(std::memory_order_acquire)(proxy, sync, flags, parameters); });
}
bool attempted = false;
}
void StreamlineProxy_Install(HMODULE module, uint64_t table, const StreamlineComEvidence& factory,
    bool streamline_initialized) noexcept {
    using namespace streamline_proxy_probe;
    if (attempted || !module || (!table && !streamline_initialized) || RuntimeLifecycle_IsStopping()) return;
    attempted = true;
    streamline_proxy_search::Result result;
    int present = INT32_MIN, present1 = INT32_MIN;
    try {
        streamline_proxy_search::Image image;
        const streamline_proxy_search::Anchor anchor{table, {factory.entries[2], factory.entries[3], factory.entries[4]},
            streamline_initialized};
        // The factory methods may belong to other hooks on that route; pin the interposer itself.
        void* const pin = streamline_initialized ? reinterpret_cast<void*>(module) : reinterpret_cast<void*>(factory.entries[2]);
        if (!Hooks_PinTargetModule(pin)) result.outcome = "module_pin_failed";
        else if (!streamline_proxy_search::Snapshot(module, &image)) result.outcome = "image_snapshot_failed";
        else {
            result = streamline_proxy_search::Find(image, anchor);
            if (result.vtable) {
                HookInstallGuard install;
                if (!install || RuntimeLifecycle_IsStopping()) result.outcome = "stopping";
                else if (!streamline_proxy_search::Unchanged(image, anchor, result)) result.outcome = "image_changed";
                else {
                    // A partial install can forward but cannot qualify any input.
                    present = InstallHook(reinterpret_cast<void*>(result.present), reinterpret_cast<void*>(&ObservePresent), &original_present);
                    present1 = InstallHook(reinterpret_cast<void*>(result.present1), reinterpret_cast<void*>(&ObservePresent1), &original_present1);
                    // Inside a batch the two hooks are only queued: enable them now, the result decides the outcome.
                    Hooks_FlushBatch();
                    const bool enabled = present == MH_OK && present1 == MH_OK &&
                        Hooks_TargetEnabled(reinterpret_cast<void*>(result.present)) &&
                        Hooks_TargetEnabled(reinterpret_cast<void*>(result.present1));
                    if (enabled) expected_vtable.store(result.vtable, std::memory_order_release);
                    result.outcome = enabled ? "installed" : "install_incomplete";
                }
            }
        }
    } catch (...) { result.outcome = "search_exception"; }
    // A shutdown is not a cause; every other outcome but an install leaves the route without its hooks for good.
    if (std::strcmp(result.outcome, "installed") != 0 && std::strcmp(result.outcome, "stopping") != 0) {
        char cause[160];
        std::snprintf(cause, sizeof(cause), "ReLimiter could not hook Streamline's swap chain (%s)", result.outcome);
        StreamlineProxy_MarkUnavailable(cause);
    }
    LOG_INFO("Streamline GPU: proxy=%s statuses=%d,%d anchor=%s route=%s slot=%u address=%llu expected=%llu "
             "candidates=%u rejected=%u candidate_check=%s candidate_slot=%u hooked_slots=0x%llx",
        result.outcome, present, present1, result.anchor.check, result.anchor.route, result.anchor.index,
        result.anchor.address, result.anchor.expected, result.candidates, result.rejected, result.candidate_check,
        result.candidate_index, static_cast<unsigned long long>(result.hooked_slots));
}
void StreamlineProxy_RecordInner(uint64_t, uint64_t queue, uint64_t) noexcept {
    using namespace streamline_proxy_probe;
    if (!queue || !expected_vtable.load(std::memory_order_acquire)) return;
    unsigned expected = 0;
    if (!queue_offer_state.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) return;
    const DWORD error = GetLastError();
    const bool copied = CopyQueue(queue, &queue_methods);
    queue_offer_state.store(copied ? 2u : 0u, std::memory_order_release);
    SetLastError(error);
}
void StreamlineProxy_Drain(bool) noexcept {
    using namespace streamline_proxy_probe;
    if (queue_offer_state.load(std::memory_order_acquire) != 2) return;
    StreamlineQueue_Offer(queue_methods);
    queue_offer_state.store(3, std::memory_order_release);
}
bool StreamlineProxy_Unavailable(char* cause, size_t size) noexcept {
    using namespace streamline_proxy_probe;
    if (!unavailable.load(std::memory_order_acquire)) return false;
    if (cause && size) std::snprintf(cause, size, "%s", unavailable_cause);
    return true;
}
void StreamlineProxy_MarkUnavailable(const char* cause) noexcept {
    using namespace streamline_proxy_probe;
    if (unavailable.load(std::memory_order_relaxed)) return;
    std::snprintf(unavailable_cause, sizeof(unavailable_cause), "%s", cause ? cause : "unknown");
    unavailable.store(true, std::memory_order_release);
    LOG_INFO("Streamline GPU: FG input route unavailable: %s", unavailable_cause);
}