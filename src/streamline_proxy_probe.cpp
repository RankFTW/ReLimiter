#include "streamline_proxy_probe.h"
#include "streamline_proxy_search.h"
#include "streamline_queue_hooks.h"
#include "streamline_gpu_handoff.h"
#include "streamline_base_present.h"
#include "hooks.h"
#include "logger.h"
#include "runtime_lifecycle.h"
#include <dxgi1_2.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwchar>

namespace streamline_proxy_probe {
using Present = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1 = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
std::atomic<Present> original_present{nullptr};
std::atomic<Present1> original_present1{nullptr};
std::atomic<uint64_t> expected_vtable{0};
// VSync vtable copies and the class vtable each one replaced (StreamlineProxy_NoteVtableCopy): append-only, one writer.
constexpr size_t kCopyCapacity = 16;
std::atomic<uint64_t> copy_vptrs[kCopyCapacity]{};
std::atomic<uint64_t> copy_originals[kCopyCapacity]{};
std::atomic<size_t> copy_count{0};
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
// Diagnostics, logged once each: whose vtable ReShade's native swapchain and queue have, and what Streamline's base
// interface leads to from them and from Streamline's own swapchain proxy (StreamlineRetrieveBaseInterface: the IID a
// Streamline proxy answers with the object it wraps; another module's proxy that forwards unknown IIDs passes it on).
// A frame generation mod can put its own proxy and Streamline's BELOW ReShade (Kingdom Come: Deliverance II with
// KCD2FrameGeneration): ReShade's native objects are then proxies, DXGI's and Direct3D's own lie further down, and
// the native Present entry and the standard queue hooks do not reach them.
constexpr GUID kStreamlineBase{0xadec44e2, 0x61f0, 0x45c3, {0xad, 0x9f, 0x1b, 0x37, 0x37, 0x92, 0x84, 0xff}};
void OwnerName(uint64_t address, char* out, size_t size) noexcept {
    HMODULE owner = nullptr;
    wchar_t path[MAX_PATH]{};
    if (!address || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(address), &owner) || !owner || !GetModuleFileNameW(owner, path, MAX_PATH)) {
        std::snprintf(out, size, "no module");
        return;
    }
    path[MAX_PATH - 1] = L'\0';
    // "(system)": ReShade's own module is often named dxgi.dll too, in the game's folder.
    wchar_t system[MAX_PATH]{};
    const UINT system_length = GetSystemDirectoryW(system, MAX_PATH);
    const bool in_system = system_length && system_length < MAX_PATH && !_wcsnicmp(path, system, system_length);
    const wchar_t* name = std::wcsrchr(path, L'\\');
    std::snprintf(out, size, "%ls%s", name ? name + 1 : path, in_system ? " (system)" : "");
}
uint64_t StreamlineBaseOf(uint64_t object) noexcept {
    __try {
        IUnknown* base = nullptr;
        if (reinterpret_cast<IUnknown*>(object)->QueryInterface(kStreamlineBase, reinterpret_cast<void**>(&base)) != S_OK || !base)
            return 0;
        base->Release(); // the layer above keeps it alive; only its address and vtable are read
        return reinterpret_cast<uint64_t>(base);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
void DescribeLayers(uint64_t object, char* out, size_t size) noexcept {
    size_t used = 0;
    out[0] = '\0';
    for (int layer = 0; layer < 4 && object; ++layer) {
        char owner[96];
        OwnerName(ReadVtable(reinterpret_cast<const void*>(object)), owner, sizeof(owner));
        const int written = std::snprintf(out + used, size - used, "%s0x%llX (vtable in %s)", layer ? " -> " : "",
            static_cast<unsigned long long>(object), owner);
        if (written <= 0 || static_cast<size_t>(written) >= size - used) break;
        used += static_cast<size_t>(written);
        const uint64_t base = StreamlineBaseOf(object);
        if (!base || base == object) break;
        object = base;
    }
}
std::atomic<bool> proxy_layers_logged{false};
// The proxy's vptr names Streamline's proxy class: the class vtable itself, or one of our VSync copies of it.
bool ExpectedClass(uint64_t table) noexcept {
    const uint64_t expected = expected_vtable.load(std::memory_order_acquire);
    if (!table || !expected) return false;
    if (table == expected) return true;
    const size_t count = copy_count.load(std::memory_order_acquire);
    for (size_t i = 0; i < count; ++i)
        if (copy_vptrs[i].load(std::memory_order_relaxed) == table)
            return copy_originals[i].load(std::memory_order_relaxed) == expected;
    return false;
}
template<class Forward> HRESULT Observe(void* proxy, UINT sync, UINT flags, Forward forward) {
    const DWORD incoming = GetLastError();
    RuntimeCallbackGuard callback;
    if (callback.IsStopping()) { SetLastError(incoming); return forward(); }
    const auto table = ReadVtable(proxy);
    const bool valid = ExpectedClass(table);
    if (valid && !proxy_layers_logged.load(std::memory_order_relaxed) &&
        !proxy_layers_logged.exchange(true, std::memory_order_relaxed)) {
        char layers[400];
        DescribeLayers(reinterpret_cast<uint64_t>(proxy), layers, sizeof(layers));
        LOG_INFO("Streamline GPU: layers from Streamline's swap chain proxy: %s", layers);
    }
    // The proxy the game presents under the swap chain of ReShade's present event on this thread
    // (streamline_base_present.h): one load and a compare once it is known.
    if (valid) StreamlineBase_NoteProxyPresent(reinterpret_cast<uint64_t>(proxy));
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
void StreamlineProxy_NoteVtableCopy(uint64_t original, uint64_t copy) noexcept {
    using namespace streamline_proxy_probe;
    if (!original || !copy) return;
    const size_t count = copy_count.load(std::memory_order_acquire);
    for (size_t i = 0; i < count; ++i)
        if (copy_vptrs[i].load(std::memory_order_relaxed) == copy) return; // a copy replaces one class for good
    if (count >= kCopyCapacity) return;
    copy_vptrs[count].store(copy, std::memory_order_relaxed);
    copy_originals[count].store(original, std::memory_order_relaxed);
    copy_count.store(count + 1, std::memory_order_release);
}
bool StreamlineProxy_IsProxyClass(uint64_t vptr) noexcept { return streamline_proxy_probe::ExpectedClass(vptr); }
void StreamlineProxy_RecordInner(uint64_t swapchain, uint64_t queue, uint64_t) noexcept {
    using namespace streamline_proxy_probe;
    if (!queue || !expected_vtable.load(std::memory_order_acquire)) return;
    unsigned expected = 0;
    if (!queue_offer_state.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) return;
    const DWORD error = GetLastError();
    const bool copied = CopyQueue(queue, &queue_methods);
    queue_offer_state.store(copied ? 2u : 0u, std::memory_order_release);
    char swapchain_layers[400], queue_layers[400], method[96];
    DescribeLayers(swapchain, swapchain_layers, sizeof(swapchain_layers));
    DescribeLayers(queue, queue_layers, sizeof(queue_layers));
    OwnerName(queue_methods.entries[6], method, sizeof(method));
    LOG_INFO("Streamline GPU: layers under ReShade: swap chain %s; queue %s, its ExecuteCommandLists 0x%llX in %s",
        swapchain_layers, queue_layers, static_cast<unsigned long long>(queue_methods.entries[6]), method);
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