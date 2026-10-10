#include "streamline_queue_hooks.h"
#include "streamline_gpu_handoff.h"
#include "streamline_com_evidence.h"
#include "hooks.h"
#include "logger.h"
#include "runtime_lifecycle.h"
#include <d3d12.h>
#include <atomic>
#include <array>
#include <cwchar>
#include <intrin.h>

namespace streamline_queue {
using Execute = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
using FenceOperation = HRESULT(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, ID3D12Fence*, UINT64);
std::atomic<Execute> original_execute{nullptr};
std::atomic<FenceOperation> original_signal{nullptr}, original_wait{nullptr};
std::array<uint64_t, 3> targets{};
std::atomic<bool> ready{false};
std::atomic<uint64_t> plugin_begin{0}, plugin_end{0};
bool feature_blocked = false; // installer worker only; public hooks do not rebind
thread_local unsigned depth = 0;
struct Depth { Depth() noexcept { ++depth; } ~Depth() { --depth; } };
bool MatchesQueue(ID3D12CommandQueue* queue) noexcept {
    __try {
        const auto* table = *reinterpret_cast<const uint64_t* const*>(queue);
        return table[10] == targets[0] && table[14] == targets[1] && table[15] == targets[2];
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
uint64_t PluginSite() noexcept {
    const auto begin = plugin_begin.load(std::memory_order_acquire);
    const auto end = plugin_end.load(std::memory_order_acquire);
    if (!begin || end <= begin) return 0;
    void* stack[12]{};
    const auto count = CaptureStackBackTrace(1, 12, stack, nullptr);
    for (unsigned i = 0; i < count; ++i) {
        const auto pc = reinterpret_cast<uint64_t>(stack[i]);
        if (pc >= begin && pc < end) return pc;
    }
    return 0; // missing/truncated stack is never an authorization
}
void STDMETHODCALLTYPE ObserveExecute(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists) {
    const DWORD incoming = GetLastError();
    RuntimeCallbackGuard callback;
    Depth nested;
    SetLastError(incoming);
    original_execute.load(std::memory_order_acquire)(queue, count, lists);
    const DWORD outgoing = GetLastError();
    if (!callback.IsStopping() && depth == 1 && ready.load(std::memory_order_acquire))
        StreamlineHandoff_Execute(queue);
    SetLastError(outgoing);
}
template<class Forward> HRESULT ObserveFence(ID3D12CommandQueue* queue, ID3D12Fence* fence,
    uint64_t value, bool signal, Forward forward) {
    const DWORD incoming = GetLastError();
    RuntimeCallbackGuard callback;
    Depth nested;
    StreamlineFenceCall call;
    if (!callback.IsStopping() && depth == 1 && ready.load(std::memory_order_acquire) &&
        StreamlineHandoff_Interested(fence)) {
        if (MatchesQueue(queue)) call = StreamlineHandoff_BeforeFence(queue, fence, value, PluginSite(), signal);
        else StreamlineHandoff_RejectQueueMethods();
    }
    SetLastError(incoming);
    const HRESULT result = forward();
    const DWORD outgoing = GetLastError();
    StreamlineHandoff_AfterFence(call, result);
    SetLastError(outgoing);
    return result;
}
HRESULT STDMETHODCALLTYPE ObserveSignal(ID3D12CommandQueue* queue, ID3D12Fence* fence, UINT64 value) {
    return ObserveFence(queue, fence, value, true, [&] { return original_signal.load(std::memory_order_acquire)(queue, fence, value); });
}
HRESULT STDMETHODCALLTYPE ObserveWait(ID3D12CommandQueue* queue, ID3D12Fence* fence, UINT64 value) {
    return ObserveFence(queue, fence, value, false, [&] { return original_wait.load(std::memory_order_acquire)(queue, fence, value); });
}
bool RuntimeMethods(const std::array<uint64_t, 3>& methods) noexcept {
    if (!methods[0] || !methods[1] || !methods[2] || methods[0] == methods[1] ||
        methods[0] == methods[2] || methods[1] == methods[2]) return false;
    HMODULE owner = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(methods[0]), &owner)) return false;
    wchar_t path[1024]{};
    const auto length = GetModuleFileNameW(owner, path, 1024);
    const wchar_t* name = std::wcsrchr(path, L'\\');
    bool valid = length && length < 1024 && name &&
        (!_wcsicmp(name + 1, L"D3D12Core.dll") || !_wcsicmp(name + 1, L"d3d12.dll"));
    for (const auto method : methods) {
        MEMORY_BASIC_INFORMATION info{};
        valid = valid && VirtualQuery(reinterpret_cast<void*>(method), &info, sizeof(info)) &&
            info.AllocationBase == owner && info.State == MEM_COMMIT && info.Type == MEM_IMAGE &&
            !(info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) && (info.Protect & 0xff) == PAGE_EXECUTE_READ;
    }
    FreeLibrary(owner);
    return valid;
}
uint32_t ImageSize(HMODULE module) noexcept {
    __try {
        const auto* base = reinterpret_cast<const BYTE*>(module);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 4096) return 0;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        return nt->Signature == IMAGE_NT_SIGNATURE && nt->FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 &&
            nt->OptionalHeader.SizeOfImage <= 32 * 1024 * 1024 ? nt->OptionalHeader.SizeOfImage : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
}
void StreamlineQueue_Offer(const StreamlineComEvidence& evidence) noexcept {
    using namespace streamline_queue;
    static bool attempted = false;
    if (attempted || !evidence.entries[6] || !evidence.entries[7] || !evidence.entries[8] || RuntimeLifecycle_IsStopping()) return;
    attempted = true;
    const std::array<uint64_t, 3> methods{evidence.entries[6], evidence.entries[7], evidence.entries[8]};
    if (!RuntimeMethods(methods)) { LOG_WARN("Streamline GPU: unsupported standard queue methods"); return; }
    HookInstallGuard install;
    if (!install || RuntimeLifecycle_IsStopping()) return;
    for (auto method : methods) if (!Hooks_PinTargetModule(reinterpret_cast<void*>(method))) return;
    if (!RuntimeMethods(methods)) return;
    targets = methods;
    const auto execute = InstallHook(reinterpret_cast<void*>(targets[0]), reinterpret_cast<void*>(&ObserveExecute), &original_execute);
    const auto signal = InstallHook(reinterpret_cast<void*>(targets[1]), reinterpret_cast<void*>(&ObserveSignal), &original_signal);
    const auto wait = InstallHook(reinterpret_cast<void*>(targets[2]), reinterpret_cast<void*>(&ObserveWait), &original_wait);
    ready.store(execute == MH_OK && signal == MH_OK && wait == MH_OK, std::memory_order_release);
    LOG_INFO("Streamline GPU: standard queue hooks ready=%d statuses=%d,%d,%d", ready.load(), execute, signal, wait);
}
void StreamlineQueue_SetFeatureModule(void* options, void* state) noexcept {
    using namespace streamline_queue;
    // Existing public resolver pins/rechecks these entry points before this call.
    // A changed selected implementation invalidates every learned site/binding.
    plugin_begin.store(0, std::memory_order_release);
    StreamlineHandoff_Invalidate("feature_implementation_changed");
    if (feature_blocked) return;
    HMODULE a = nullptr, b = nullptr;
    const bool found = options && state && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCWSTR>(options), &a) && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCWSTR>(state), &b);
    const auto size = found && a == b ? ImageSize(a) : 0;
    plugin_end.store(reinterpret_cast<uint64_t>(a) + size, std::memory_order_release);
    plugin_begin.store(size ? reinterpret_cast<uint64_t>(a) : 0, std::memory_order_release);
    if (a) FreeLibrary(a);
    if (b) FreeLibrary(b);
}
void StreamlineQueue_Drain(bool final) noexcept { StreamlineHandoff_Service(final); }
void StreamlineQueue_BlockFeature() noexcept {
    streamline_queue::feature_blocked = true;
    streamline_queue::plugin_begin.store(0, std::memory_order_release);
    StreamlineHandoff_Invalidate("feature_implementation_blocked");
}