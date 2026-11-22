#include "streamline_factory_probe.h"
#include "streamline_com_evidence.h"
#include "streamline_proxy_probe.h"
#include "hooks.h"
#include "logger.h"
#include "runtime_lifecycle.h"
#include <dxgi1_2.h>
#include <atomic>
#include <cstdio>
#include <cstring>

namespace streamline_factory_probe {
using CreateFactory = HRESULT(WINAPI*)(REFIID, void**);
struct Anchor {
    HMODULE module = nullptr;
    uint64_t table = 0;
    StreamlineComEvidence methods{};
    const char* outcome = "not_started";
    bool absent = false, valid = false;
    // The image is selected by Streamline's own initialization, not by the
    // factory methods (streamline_proxy_search.h).
    bool streamline_initialized = false;
    // The first factory slot another module owned, and that module's file
    // name ("unknown" when no module owns it).
    uint32_t foreign_slot = 0;
    char foreign_owner[64] = {};
    // The interposer's CreateDXGIFactory1 resolved outside its image (another
    // add-on's GetProcAddress hook or export table entry); foreign_owner names
    // the module that answered.
    bool foreign_export = false;
};
// Attempts: early (init_device), worker (the early attempt found no
// interposer) and ready (once, after Streamline resolved its DLSS-G
// functions, when no attempt before it produced an anchor). The factory
// slots are the interposer's only while nothing hooked them after it: in
// CONTROL Resonant RenoDX DLSS owned slot 10 at init_device and the SCSKiller
// d3d12.dll proxy once Streamline was ready. Likewise the factory export is
// the interposer's only while no other add-on answers for it (Cyberpunk 2077
// with RenoDX DLSS: export_rejected at both attempts). An attempt made while
// Streamline is ready then takes Streamline's initialization as the image's
// provenance.
Anchor early, fallback, ready;
std::atomic<bool> attempted{false}, finished{false};
bool fallback_attempted = false, ready_attempted = false;
// Whether Streamline was ready when the current anchor was attempted; the
// early attempt at init_device can precede slInit.
bool attempted_ready = false;
const Anchor& Current() noexcept {
    if (ready_attempted) return ready;
    return early.absent ? fallback : early;
}
void RecordOwner(void* address, Anchor* anchor) noexcept {
    HMODULE owner = nullptr;
    std::snprintf(anchor->foreign_owner, sizeof(anchor->foreign_owner), "unknown");
    if (!address || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(address), &owner)) {
        return;
    }
    char path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameA(owner, path, MAX_PATH);
    if (!length || length >= MAX_PATH) return;
    const char* name = std::strrchr(path, '\\');
    std::snprintf(anchor->foreign_owner, sizeof(anchor->foreign_owner), "%s", name ? name + 1 : path);
}
bool OwnedBy(void* address, HMODULE expected) noexcept {
    HMODULE owner = nullptr;
    if (!address || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(address), &owner)) return false;
    const bool matched = owner == expected;
    FreeLibrary(owner); return matched;
}
HRESULT MakeFactory(CreateFactory create, IDXGIFactory2** factory) noexcept {
    __try { return create(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(factory)); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return E_UNEXPECTED; }
}
bool CopyAnchor(IDXGIFactory2* factory, Anchor* anchor) noexcept {
    __try {
        const auto* table = *reinterpret_cast<const uint64_t* const*>(factory);
        anchor->table = reinterpret_cast<uint64_t>(table);
        anchor->methods.entries[2] = table[10];
        anchor->methods.entries[3] = table[15];
        anchor->methods.entries[4] = table[16];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool ReleaseFactory(IDXGIFactory2* factory) noexcept {
    __try { factory->Release(); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// streamline_ready: Streamline resolved its DLSS-G functions before this
// attempt, so its plugins need the swapchain proxy in this image.
Anchor Bootstrap(HMODULE module, CreateFactory create, bool streamline_ready) noexcept {
    Anchor anchor; anchor.module = module;
    if (!create) { anchor.outcome = "export_rejected"; return anchor; }
    if (!OwnedBy(reinterpret_cast<void*>(create), module)) {
        // Another module answers for the interposer's factory export. Its factory
        // says nothing about the interposer and calling it runs that module's
        // code, so the export is never called. Once Streamline resolved its DLSS-G
        // functions, its initialization selects the image, as when other hooks own
        // the factory slots; before that the anchor waits for it.
        anchor.foreign_export = true;
        RecordOwner(reinterpret_cast<void*>(create), &anchor);
        anchor.streamline_initialized = anchor.valid = streamline_ready;
        anchor.outcome = streamline_ready ? "streamline_initialized" : "export_rejected";
        return anchor;
    }
    if (!Hooks_PinTargetModule(reinterpret_cast<void*>(create))) { anchor.outcome = "pin_failed"; return anchor; }
    IDXGIFactory2* factory = nullptr;
    const auto result = MakeFactory(create, &factory);
    if (FAILED(result) || !factory) { anchor.outcome = "factory_failed"; return anchor; }
    bool readable = CopyAnchor(factory, &anchor);
    // The interposer patches these slots once its plugins need the DXGI
    // proxy; before slInit they are dxgi.dll's, and hooks installed after it
    // own them instead.
    const uint32_t slots[] = {10, 15, 16};
    bool owned = readable;
    for (unsigned i = 2; owned && i < 5; ++i) {
        if (OwnedBy(reinterpret_cast<void*>(anchor.methods.entries[i]), module)) continue;
        owned = false;
        anchor.foreign_slot = slots[i - 2];
        RecordOwner(reinterpret_cast<void*>(anchor.methods.entries[i]), &anchor);
    }
    const auto& entries = anchor.methods.entries;
    const bool distinct = entries[2] != entries[3] && entries[2] != entries[4] && entries[3] != entries[4];
    // No factory creation hooks, QI observer, temporary swapchain or GPU work.
    const bool released = ReleaseFactory(factory);
    anchor.streamline_initialized = readable && released && !owned && streamline_ready;
    anchor.valid = readable && released && ((owned && distinct) || anchor.streamline_initialized);
    anchor.outcome = !released ? "release_fault" : !readable ? "table_unreadable" :
        anchor.streamline_initialized ? "streamline_initialized" :
        !owned ? "methods_not_interposer" : !distinct ? "methods_aliased" : "anchor_ready";
    return anchor;
}
void Report(const Anchor& anchor, const char* stage) noexcept {
    if (anchor.foreign_export) {
        LOG_INFO("Streamline GPU: factory anchor=%s stage=%s export_owner=%s", anchor.outcome, stage,
            anchor.foreign_owner);
    } else if (anchor.foreign_slot) {
        LOG_INFO("Streamline GPU: factory anchor=%s stage=%s slot=%u owner=%s", anchor.outcome, stage,
            anchor.foreign_slot, anchor.foreign_owner);
    } else {
        LOG_INFO("Streamline GPU: factory anchor=%s stage=%s", anchor.outcome, stage);
    }
}
// After the last attempt failed: the FG input route has no Streamline hooks in this process.
void MarkUnavailable(const Anchor& anchor) noexcept {
    char cause[160];
    if ((anchor.foreign_export || anchor.foreign_slot) && anchor.foreign_owner[0] &&
        std::strcmp(anchor.foreign_owner, "unknown") != 0) {
        std::snprintf(cause, sizeof(cause), "ReLimiter could not hook Streamline's DXGI factory (%s, hooked by %s)",
            anchor.outcome, anchor.foreign_owner);
    } else {
        std::snprintf(cause, sizeof(cause), "ReLimiter could not hook Streamline's DXGI factory (%s)", anchor.outcome);
    }
    StreamlineProxy_MarkUnavailable(cause);
}
void TryWorker(HMODULE module, CreateFactory create, bool streamline_ready) noexcept {
    if (early.absent && !fallback_attempted) {
        fallback_attempted = true;
        fallback = Bootstrap(module, create, streamline_ready);
        attempted_ready = streamline_ready;
    }
    if (!ready_attempted && streamline_ready && !attempted_ready && !Current().valid) {
        ready_attempted = true;
        ready = Bootstrap(module, create, true);
    }
    const auto& anchor = Current();
    if (anchor.valid && anchor.module == module) {
        StreamlineProxy_Install(module, anchor.table, anchor.methods, anchor.streamline_initialized);
    } else if (!anchor.valid && (ready_attempted || attempted_ready)) {
        // No attempt follows one made while Streamline is ready.
        MarkUnavailable(anchor);
    }
}
}
void StreamlineFactoryProbe_TryEarly() noexcept {
    using namespace streamline_factory_probe;
    const DWORD error = GetLastError();
    if (!RuntimeLifecycle_IsStopping() && !attempted.exchange(true, std::memory_order_acq_rel)) {
        HMODULE module = nullptr;
        if (GetModuleHandleExW(0, L"sl.interposer.dll", &module)) {
            early = Bootstrap(module, reinterpret_cast<CreateFactory>(GetProcAddress(module, "CreateDXGIFactory1")), false);
            FreeLibrary(module);
        } else { early.absent = true; early.outcome = "interposer_absent"; }
        finished.store(true, std::memory_order_release);
    }
    SetLastError(error);
}
void StreamlineFactoryProbe_TryWorker(HMODULE module, bool streamline_ready) noexcept {
    using namespace streamline_factory_probe;
    if (!module || RuntimeLifecycle_IsStopping() || !finished.load(std::memory_order_acquire)) return;
    TryWorker(module, reinterpret_cast<CreateFactory>(GetProcAddress(module, "CreateDXGIFactory1")), streamline_ready);
}
void StreamlineFactoryProbe_Drain(bool) noexcept {
    using namespace streamline_factory_probe;
    static bool reported = false, ready_reported = false;
    if (!finished.load(std::memory_order_acquire)) return;
    if (!reported && !(early.absent && !fallback_attempted)) {
        reported = true;
        Report(early.absent ? fallback : early, early.absent ? "worker" : "init_device");
    }
    if (ready_attempted && !ready_reported) {
        ready_reported = true;
        Report(ready, "streamline_ready");
    }
}