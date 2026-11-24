#include "streamline_base_present.h"
#ifdef _WIN64
#include "streamline_proxy_probe.h"
#include "streamline_gpu_handoff.h"
#include "vsync_control.h"
#include "logger.h"
#include <Windows.h>
#include <dxgi1_2.h>
#include <d3d12.h>
#include <atomic>
#include <array>

std::atomic<uint64_t> g_streamline_base_publication{0};

namespace streamline_base_present {
// Even address reuse cannot give a reader a new queue with an old alias: each publication has a unique revision.
std::atomic<uint64_t> base{0}, alias{0}, proxy_id{0}, device{0}, queue{0};
using GetNativeInterface = int(__cdecl*)(void*, void**);
std::atomic<ULONGLONG> next_try{0}; // class probe at the ReShade event; proxy identity is checked under the vtable lock
struct OpenEvent {
    uint64_t native, device, queue;
    bool carried, inside_proxy;
    uint64_t pending;
};
// A missing or migrated finish must not grow the Present thread's storage. The stack contains
// distinct native identities; a new event for an existing identity abandons that event and its children.
struct OpenEvents {
    std::array<OpenEvent, 4> entries{};
    size_t depth = 0;
    size_t Find(uint64_t native) const noexcept {
        for (size_t i = depth; i; --i) if (entries[i - 1].native == native) return i - 1;
        return depth;
    }
    void Truncate(size_t keep) noexcept {
        while (depth > keep) {
            const auto pending = entries[--depth].pending;
            if (pending && VSync_AbandonOutputAlias(pending)) next_try.store(0, std::memory_order_relaxed);
        }
    }
    void Push(const OpenEvent& e) noexcept {
        const auto index = Find(e.native);
        if (index != depth) Truncate(index);
        else if (depth == entries.size()) Truncate(depth - 1);
        entries[depth++] = e;
    }
};
thread_local OpenEvents events{};
uintptr_t ReadWord(uintptr_t address) noexcept {
    __try { return *reinterpret_cast<const uintptr_t*>(address); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
// The queue the base swap chain presents on. DXGI keeps it in the swap chain object (GetDevice answers with the device
// only): the pointers in the object's first 4 KB whose vptr is the class vtable of a known D3D12 command queue and that
// answer QueryInterface for ID3D12CommandQueue. One distinct queue is taken; with several, the first that is not the
// game's (ReShade's) queue, logged.
uint64_t FindQueue(uintptr_t object, uint64_t game_queue) noexcept {
    const uintptr_t queue_vptr = ReadWord(static_cast<uintptr_t>(game_queue));
    if (!queue_vptr) return 0;
    uintptr_t found[4]{};
    size_t count = 0;
    for (size_t offset = 0; offset < 0x1000 && count < 4; offset += sizeof(uintptr_t)) {
        const uintptr_t word = ReadWord(object + offset);
        if (!word || (word & 7) || word == object) continue;
        if (ReadWord(word) != queue_vptr) continue;
        bool seen = false;
        for (size_t i = 0; i < count; ++i) seen |= found[i] == word;
        if (seen) continue;
        ID3D12CommandQueue* candidate = nullptr;
        if (FAILED(reinterpret_cast<IUnknown*>(word)->QueryInterface(__uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&candidate))) || !candidate) continue;
        const bool same = reinterpret_cast<uintptr_t>(candidate) == word;
        candidate->Release();
        if (same) found[count++] = word;
    }
    uint64_t chosen = 0;
    for (size_t i = 0; i < count && !chosen; ++i) if (found[i] != game_queue) chosen = found[i];
    if (!chosen && count) chosen = found[0];
    LOG_INFO("Streamline base present: queues in base swap chain 0x%llX: %zu (0x%llX 0x%llX 0x%llX 0x%llX), game queue 0x%llX, taken 0x%llX",
        static_cast<unsigned long long>(object), count, static_cast<unsigned long long>(found[0]), static_cast<unsigned long long>(found[1]),
        static_cast<unsigned long long>(found[2]), static_cast<unsigned long long>(found[3]), static_cast<unsigned long long>(game_queue),
        static_cast<unsigned long long>(chosen));
    return chosen;
}
}

void StreamlineBase_PublishLocked(const StreamlineBaseBinding& b) noexcept {
    using namespace streamline_base_present;
    g_streamline_base_publication.store(0, std::memory_order_relaxed);
    if (!b.base) return;
    // A reader that sees ANY new field must also see the invalidation when it rechecks the revision.
    std::atomic_thread_fence(std::memory_order_release);
    base.store(b.base, std::memory_order_relaxed);
    alias.store(b.alias, std::memory_order_relaxed);
    proxy_id.store(b.proxy, std::memory_order_relaxed);
    queue.store(b.queue, std::memory_order_relaxed);
    device.store(b.device, std::memory_order_relaxed);
    g_streamline_base_publication.store(b.revision, std::memory_order_release);
}

StreamlineBaseBinding StreamlineBase_Read(uint64_t publication) noexcept {
    using namespace streamline_base_present;
    std::atomic_thread_fence(std::memory_order_acquire); // pairs with publication of all five fields
    const StreamlineBaseBinding b{base.load(std::memory_order_relaxed), alias.load(std::memory_order_relaxed),
        proxy_id.load(std::memory_order_relaxed), queue.load(std::memory_order_relaxed), device.load(std::memory_order_relaxed), publication};
    std::atomic_thread_fence(std::memory_order_acquire);
    return publication == g_streamline_base_publication.load(std::memory_order_relaxed) ? b : StreamlineBaseBinding{};
}

static uint64_t ReserveProxyForEvent(uint64_t proxy, uint64_t* refused_base = nullptr) noexcept {
    using namespace streamline_base_present;
    if (!events.depth || !proxy) return 0;
    auto& event = events.entries[events.depth - 1];
    if (event.inside_proxy) return 0;
    const auto b = StreamlineBase_Current();
    if (b.alias == event.native && b.proxy == proxy) return 0; // steady state needs no lock or clock
    const uint64_t ticket = VSync_BeginOutputAlias(event.native, proxy, GetTickCount64(), refused_base);
    // Attach the ticket before calling any external code: an intervening event can abandon this resolver too.
    if (ticket) event.pending = ticket;
    return ticket;
}

// The base swap chain of Streamline's proxy `proxy`, which the native swap chain `native` is or wraps: its queue is
// found and its Present entries are put on it. The game's thread, with `proxy` alive (ReShade's present event of a
// native that is the proxy, or the proxy's own Present).
static void Resolve(uint64_t proxy) noexcept {
    using namespace streamline_base_present;
    uint64_t refused_base = 0;
    const uint64_t ticket = ReserveProxyForEvent(proxy, &refused_base);
    if (!ticket) return;
    const auto& event = events.entries[events.depth - 1];
    const uint64_t native = event.native, native_device = event.device, game_queue = event.queue;
    const HMODULE interposer = GetModuleHandleW(L"sl.interposer.dll");
    const auto get_native = interposer ? reinterpret_cast<GetNativeInterface>(GetProcAddress(interposer, "slGetNativeInterface")) : nullptr;
    void* base = nullptr;
    const int result = get_native ? get_native(reinterpret_cast<void*>(static_cast<uintptr_t>(proxy)), &base) : -1;
    if (result != 0 || !base) {
        // Complete this resolution even on failure. Never cancel a newer proxy's ticket.
        VSync_BindOutputAlias(ticket, {0, native, proxy});
        if (get_native) LOG_WARN("Streamline base present: slGetNativeInterface(0x%llX) = %d", static_cast<unsigned long long>(proxy), result);
        return;
    }
    auto* const swapchain = static_cast<IDXGISwapChain*>(base);
    if (reinterpret_cast<uint64_t>(base) == refused_base) {
        VSync_BindOutputAlias(ticket, {0, native, proxy});
        swapchain->Release();
        return;
    }
    const uint64_t present_queue = game_queue ? FindQueue(reinterpret_cast<uintptr_t>(base), game_queue) : 0;
    const bool installed = VSync_BindOutputAlias(ticket, {reinterpret_cast<uint64_t>(base), native, proxy, present_queue, native_device});
    if (native == proxy)
        LOG_INFO("Streamline base present: native 0x%llX is Streamline's proxy; base swap chain 0x%llX queue 0x%llX: %s",
            static_cast<unsigned long long>(native), static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(base)),
            static_cast<unsigned long long>(present_queue),
            installed ? "base entry pending event finish" : "not installed");
    else
        LOG_INFO("Streamline base present: native 0x%llX wraps Streamline's proxy 0x%llX (another module's swap chain "
                 "between ReShade and Streamline); base swap chain 0x%llX queue 0x%llX: %s",
            static_cast<unsigned long long>(native), static_cast<unsigned long long>(proxy),
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(base)), static_cast<unsigned long long>(present_queue),
            installed ? "base entry pending event finish" : "not installed");
    swapchain->Release();
}

bool StreamlineBase_NoteGamePresent(uint64_t native, uint64_t native_device, uint64_t game_queue) {
    using namespace streamline_base_present;
    if (!native) return false;
    // A ReShade event already INSIDE Streamline is never evidence that its caller is below it,
    // including when ReShade delays finish_present across a DO_NOT_WAIT retry.
    const bool inside_proxy = StreamlineInputScope::Current() != nullptr;
    // No output binding: one relaxed load and a compare, reused for the event's routing decision.
    const bool carried = StreamlineBase_Carries(native);
    events.Push({native, native_device, game_queue, carried, inside_proxy, 0});
    if (!carried && !inside_proxy) {
        const ULONGLONG now = GetTickCount64();
        if (now >= next_try.load(std::memory_order_relaxed)) {
            next_try.store(now + 1000, std::memory_order_relaxed);
            if (StreamlineProxy_IsProxyClass(ReadWord(static_cast<uintptr_t>(native)))) {
                Resolve(native);
            }
        }
    }
    return carried;
}

bool StreamlineBase_EventCarried(uint64_t native) noexcept {
    using namespace streamline_base_present;
    const auto index = events.Find(native);
    return index != events.depth ? events.entries[index].carried : StreamlineBase_Carries(native);
}

bool StreamlineBase_NoteGamePresentDone(uint64_t native) noexcept {
    using namespace streamline_base_present;
    const auto index = events.Find(native);
    if (index == events.depth) return false;
    events.Truncate(index + 1); // a finish out of order abandons all children, never publishes them
    const auto& finished = events.entries[--events.depth];
    const bool carried = finished.carried;
    if (finished.pending) VSync_ActivateOutputAlias(finished.pending, native);
    return carried;
}

// A Present of Streamline's proxy (streamline_proxy_probe.cpp, in front of the forward). Inside ReShade's present
// event of a native swap chain that is not the proxy, that native is another module's swap chain wrapping the proxy
// (Kingdom Come: Deliverance II with KCD2FrameGeneration: ReShade, the mod's swap chain, Streamline's proxy, DXGI),
// and the proxy's base swap chain is where DLSS-G's worker presents. Resolved here because the proxy is alive here.
void StreamlineBase_NoteProxyPresent(uint64_t proxy) noexcept {
    Resolve(proxy); // pending and published replacements supersede before throttling
}

void StreamlineBase_OnDestroyed(uint64_t native) noexcept {
    using namespace streamline_base_present;
    // Also cancels a resolution that has not published yet. No external COM call holds the vtable lock.
    if (!VSync_UnbindOutputAlias(native)) return;
    next_try.store(0, std::memory_order_relaxed);
}
#endif
