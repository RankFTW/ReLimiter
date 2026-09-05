// White-box production regression: compile this file with src/hooks.cpp.
// Do NOT also compile vsync_control.cpp/loadlib_hooks.cpp or link MinHook:
// implementations are included below, with deterministic boundary fakes.
// Windows x64/Win32, C++17. This validates hook ownership/routing, not real
// instruction patching, loader-lock behavior, driver calls or frame pacing.
#include "../src/vsync_control.cpp"
#include "../src/loadlib_hooks.cpp"

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace {
void Check(bool condition, const char* message) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::abort();
}

struct FakeHook {
    void* target = nullptr;
    bool enabled = false;
};
std::array<FakeHook, 64> s_fake_hooks{};
std::mutex s_fake_mutex;
std::atomic<unsigned> s_create_calls{0};
std::atomic<unsigned> s_remove_calls{0};
std::atomic<MH_STATUS> s_next_enable_status{MH_OK};
void* s_blocked_target = nullptr;
HANDLE s_create_entered = nullptr;
HANDLE s_release_create = nullptr;
HANDLE s_enable_entered = nullptr;
bool s_test_stopping = false;
void* s_expected_self = nullptr;
const DXGI_PRESENT_PARAMETERS* s_expected_parameters = nullptr;
unsigned s_background_starts = 0;
unsigned s_synchronous_installs = 0;
bool s_load_succeeds = true;

HMODULE WINAPI FakeLoadW(LPCWSTR) {
      SetLastError(ERROR_MOD_NOT_FOUND);
      return s_load_succeeds ? reinterpret_cast<HMODULE>(0x1000) : nullptr;
}
HMODULE WINAPI FakeLoadA(LPCSTR) { return FakeLoadW(nullptr); }
HMODULE WINAPI FakeLoadExW(LPCWSTR name, HANDLE, DWORD) { return FakeLoadW(name); }
HMODULE WINAPI FakeLoadExA(LPCSTR name, HANDLE, DWORD) { return FakeLoadA(name); }

template <int Index>
__declspec(noinline) int WINAPI Target(int value) { return value + Index; }
using TestFunction = int(WINAPI*)(int);

HRESULT STDMETHODCALLTYPE PresentA(IDXGISwapChain* self, UINT interval, UINT flags) {
    Check(self == s_expected_self && interval == 3 && flags == 4,
          "Present preserves self, interval and flags");
    return 101;
}
HRESULT STDMETHODCALLTYPE PresentB(IDXGISwapChain* self, UINT interval, UINT flags) {
    // Wrapper-to-native call with the SAME self, whose vtable still names B.
    return s_present_thunks[0](self, interval, flags) + 1;
}
HRESULT STDMETHODCALLTYPE Present1A(
    IDXGISwapChain1* self, UINT interval, UINT flags,
    const DXGI_PRESENT_PARAMETERS* parameters) {
    Check(self == s_expected_self && interval == 3 && flags == 4 &&
          parameters == s_expected_parameters, "Present1 preserves typed arguments");
    return 201;
}
HRESULT STDMETHODCALLTYPE Present1B(
    IDXGISwapChain1* self, UINT interval, UINT flags,
    const DXGI_PRESENT_PARAMETERS* parameters) {
    return s_present1_thunks[0](self, interval, flags, parameters) + 1;
}

template <size_t Index>
HRESULT STDMETHODCALLTYPE ExtraPresent(IDXGISwapChain*, UINT, UINT) {
    return static_cast<HRESULT>(300 + Index);
}
template <size_t... Indices>
constexpr auto ExtraTargets(std::index_sequence<Indices...>) {
    return std::array<PFN_Present, sizeof...(Indices)>{ &ExtraPresent<Indices>... };
}

void TestSerializedPublication() {
    static std::atomic<TestFunction> original{nullptr};
    s_create_entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    s_release_create = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    s_enable_entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE second_started = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Check(s_create_entered && s_release_create && s_enable_entered && second_started,
          "create test synchronization events");
    s_blocked_target = reinterpret_cast<void*>(&Target<1>);
    MH_STATUS first_status = MH_UNKNOWN;
    MH_STATUS second_status = MH_UNKNOWN;
    std::thread first([&] {
        first_status = InstallHook(s_blocked_target,
            reinterpret_cast<void*>(&Target<2>), &original);
    });
    Check(WaitForSingleObject(s_create_entered, 5000) == WAIT_OBJECT_0,
          "first create entered");
    std::thread second([&] {
        SetEvent(second_started);
        second_status = InstallHook(s_blocked_target,
            reinterpret_cast<void*>(&Target<2>), &original);
    });
    Check(WaitForSingleObject(second_started, 5000) == WAIT_OBJECT_0,
          "competing installer started");
    Check(WaitForSingleObject(s_enable_entered, 100) == WAIT_TIMEOUT,
          "competing installer cannot enable an unfinished publication");
    Check(original.load(std::memory_order_acquire) == nullptr,
          "original remains unpublished until create completes");
    SetEvent(s_release_create);
    first.join();
    second.join();
    Check(first_status == MH_OK && second_status == MH_OK,
          "both identical requests succeed");
    Check(s_create_calls.load() == 1 && s_remove_calls.load() == 0,
          "one create, no exposed-trampoline rollback");
    Check(original.load(std::memory_order_acquire)(5) == 6,
          "published typed original remains callable");
    s_blocked_target = nullptr;
    CloseHandle(second_started);
    CloseHandle(s_create_entered);
    CloseHandle(s_release_create);
    CloseHandle(s_enable_entered);
    s_create_entered = s_release_create = s_enable_entered = nullptr;

    static std::atomic<TestFunction> foreign_original{nullptr};
    Check(InstallHook(reinterpret_cast<void*>(&Target<1>),
          reinterpret_cast<void*>(&Target<2>), &foreign_original) == MH_ERROR_ALREADY_CREATED,
          "another slot cannot adopt an existing target");
    Check(InstallHook(reinterpret_cast<void*>(&Target<3>),
          reinterpret_cast<void*>(&Target<2>), &original) == MH_ERROR_ALREADY_CREATED,
          "another target cannot overwrite a published original");
    Check(original.load()(5) == 6 && foreign_original.load() == nullptr,
          "rejected requests preserve original identity");
}

void TestRetainedEnableFailure() {
    static std::atomic<TestFunction> original{nullptr};
    s_next_enable_status.store(MH_ERROR_MEMORY_PROTECT);
    {
        HookInstallGuard outer;
        Check(static_cast<bool>(outer), "outer install admitted");
        Check(InstallHook(reinterpret_cast<void*>(&Target<4>),
              reinterpret_cast<void*>(&Target<5>), &original) == MH_ERROR_MEMORY_PROTECT,
              "nested install reports enable failure");
    }
    Check(original.load()(5) == 9 && s_remove_calls.load() == 0,
          "enable failure retains original and MinHook record");
    const unsigned creates = s_create_calls.load();
    s_next_enable_status.store(MH_ERROR_ENABLED);
    Check(InstallHook(reinterpret_cast<void*>(&Target<4>),
          reinterpret_cast<void*>(&Target<5>), &original) == MH_OK,
          "already-enabled retry succeeds without rollback");
    Check(s_create_calls.load() == creates && s_remove_calls.load() == 0,
          "retry neither recreates nor removes trampoline");
}

void TestExactTargetThunks() {
    bool already_installed = false;
    Check(InstallTypedHook(s_present_targets, reinterpret_cast<void*>(&PresentA),
          s_present_thunks, &already_installed) == MH_OK, "install Present A");
    Check(InstallTypedHook(s_present_targets, reinterpret_cast<void*>(&PresentB),
          s_present_thunks, &already_installed) == MH_OK, "install Present B");
    Check(InstallTypedHook(s_present1_targets, reinterpret_cast<void*>(&Present1A),
          s_present1_thunks, &already_installed) == MH_OK, "install Present1 A");
    Check(InstallTypedHook(s_present1_targets, reinterpret_cast<void*>(&Present1B),
          s_present1_thunks, &already_installed) == MH_OK, "install Present1 B");

    std::array<void*, 23> vtable{};
    vtable[8] = reinterpret_cast<void*>(&PresentB);
    vtable[22] = reinterpret_cast<void*>(&Present1B);
    struct FakeSwapchain { void** vtable; } swapchain{vtable.data()};
    s_expected_self = &swapchain;
    DXGI_PRESENT_PARAMETERS parameters{};
    s_expected_parameters = &parameters;
    for (bool stopping : {false, true}) {
        s_test_stopping = stopping;
        auto* self = reinterpret_cast<IDXGISwapChain*>(&swapchain);
        auto* self1 = reinterpret_cast<IDXGISwapChain1*>(&swapchain);
        Check(s_present_thunks[0](self, 3, 4) == 101,
              "entered Present A ignores self vtable B");
        Check(s_present_thunks[1](self, 3, 4) == 102,
              "Present wrapper B chains to A without recursion");
        Check(s_present1_thunks[0](self1, 3, 4, &parameters) == 201,
              "entered Present1 A ignores self vtable B");
        Check(s_present1_thunks[1](self1, 3, 4, &parameters) == 202,
              "Present1 wrapper B chains to A without recursion");
        swapchain.vtable = nullptr;
        Check(s_present_thunks[0](self, 3, 4) == 101 &&
              s_present1_thunks[0](self1, 3, 4, &parameters) == 201,
              "forwarding never dereferences a replaced/null vtable");
        swapchain.vtable = vtable.data();
    }
    s_test_stopping = false;

    constexpr auto extra = ExtraTargets(std::make_index_sequence<15>{});
    for (size_t index = 0; index < 14; ++index) {
        Check(InstallTypedHook(s_present_targets, reinterpret_cast<void*>(extra[index]),
              s_present_thunks, &already_installed) == MH_OK, "fill bounded thunk slots");
    }
    Check(InstallTypedHook(s_present_targets, reinterpret_cast<void*>(extra[14]),
          s_present_thunks, &already_installed) == MH_ERROR_MEMORY_ALLOC,
          "capacity exhaustion does not recycle a retained slot");
    Check(s_present_thunks[0](reinterpret_cast<IDXGISwapChain*>(&swapchain), 3, 4) == 101,
          "old original survives capacity exhaustion");
}

void TestLoaderNotifications() {
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Check(event != nullptr, "create notification event");
    s_install_event.store(event, std::memory_order_release);
    s_orig_LoadLibraryA = &FakeLoadA;
    s_orig_LoadLibraryW = &FakeLoadW;
    s_orig_LoadLibraryExA = &FakeLoadExA;
    s_orig_LoadLibraryExW = &FakeLoadExW;
    Hook_LoadLibraryA("sl.interposer.dll");
    Hook_LoadLibraryW(L"NVNGX.DLL");
    Check(GetLastError() == ERROR_MOD_NOT_FOUND, "preserve loader last-error result");
    Check(WaitForSingleObject(event, 0) == WAIT_OBJECT_0 &&
          WaitForSingleObject(event, 0) == WAIT_TIMEOUT,
          "successful executable-load notifications coalesce");
    for (DWORD flags : {LOAD_LIBRARY_AS_DATAFILE, LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE,
                       LOAD_LIBRARY_AS_IMAGE_RESOURCE}) {
        Hook_LoadLibraryExA("sl.interposer.dll", nullptr, flags);
        Hook_LoadLibraryExW(L"nvngx.dll", nullptr, flags);
        Check(WaitForSingleObject(event, 0) == WAIT_TIMEOUT,
              "data-only loads do not notify");
    }
    Hook_LoadLibraryExA("sl.interposer.dll", nullptr, 0);
    Hook_LoadLibraryExW(L"nvngx.dll", nullptr, 0);
    Check(WaitForSingleObject(event, 0) == WAIT_OBJECT_0,
          "executable LoadLibraryEx loads notify");
    s_load_succeeds = false;
    Hook_LoadLibraryW(L"nvngx.dll");
    Check(WaitForSingleObject(event, 0) == WAIT_TIMEOUT, "failed loads do not notify");
    s_load_succeeds = true;
    s_test_stopping = true;
    Hook_LoadLibraryW(L"nvngx.dll");
    Check(WaitForSingleObject(event, 0) == WAIT_TIMEOUT, "stopping callbacks only forward");
    s_test_stopping = false;
    s_on_install_worker = true;
    Hook_LoadLibraryW(L"nvngx.dll");
    Check(WaitForSingleObject(event, 0) == WAIT_TIMEOUT, "worker cannot self-notify a retry loop");
    s_on_install_worker = false;
    Check(s_background_starts == 0 && s_synchronous_installs == 0,
          "loader callbacks neither install nor create/join workers");
    s_install_event.store(nullptr, std::memory_order_release);
    CloseHandle(event);
}
} // namespace

// Boundary fakes, not copied installation/forwarding implementations.
Config g_config{};
void Log_Write(LogLevel, const char*, ...) {}
RuntimeCallbackGuard::RuntimeCallbackGuard() noexcept : stopping_(s_test_stopping) {}
RuntimeCallbackGuard::~RuntimeCallbackGuard() = default;
bool RuntimeLifecycle_IsStopping() noexcept { return s_test_stopping; }
HANDLE RuntimeLifecycle_GetStopEvent() noexcept { return nullptr; }
bool BackgroundTasks_Start(BackgroundTaskProc, void*) noexcept {
      ++s_background_starts;
      return false;
}
#ifdef _WIN64
void HookStreamlinePCL(HMODULE) { ++s_synchronous_installs; }
void NGXHooks_TryInstall() { ++s_synchronous_installs; }
#endif
bool FrameTimeline_BeginPresentCall(
    uint64_t, PresentTimelineEntryPoint, PresentTimelineCall*) noexcept { return false; }
bool FrameTimeline_BeginPresentAttempt(
    const PresentTimelineCall&, uint32_t, PresentTimelineSpan*) noexcept { return false; }
bool FrameTimeline_PublishPresentSpan(PresentTimelineSpan) noexcept { return false; }

extern "C" MH_STATUS WINAPI MH_CreateHook(void* target, void*, void** original) {
    {
        std::lock_guard<std::mutex> lock(s_fake_mutex);
        for (const FakeHook& hook : s_fake_hooks)
            if (hook.target == target) return MH_ERROR_ALREADY_CREATED;
        FakeHook* available = nullptr;
        for (FakeHook& hook : s_fake_hooks) {
            if (!hook.target) { available = &hook; break; }
        }
        if (!available) return MH_ERROR_MEMORY_ALLOC;
        *available = {target, false};
        *original = target;
        s_create_calls.fetch_add(1);
    }
    if (target == s_blocked_target) {
        SetEvent(s_create_entered);
        Check(WaitForSingleObject(s_release_create, 5000) == WAIT_OBJECT_0,
              "release blocked create");
    }
    return MH_OK;
}
extern "C" MH_STATUS WINAPI MH_EnableHook(void* target) {
    if (s_enable_entered) SetEvent(s_enable_entered);
    std::lock_guard<std::mutex> lock(s_fake_mutex);
    const MH_STATUS forced = s_next_enable_status.exchange(MH_OK);
    for (FakeHook& hook : s_fake_hooks) {
        if (hook.target != target) continue;
        if (forced == MH_ERROR_ENABLED) hook.enabled = true;
        if (forced != MH_OK) return forced;
        if (hook.enabled) return MH_ERROR_ENABLED;
        hook.enabled = true;
        return MH_OK;
    }
    return MH_ERROR_NOT_CREATED;
}
extern "C" MH_STATUS WINAPI MH_DisableHook(void* target) {
    std::lock_guard<std::mutex> lock(s_fake_mutex);
    for (FakeHook& hook : s_fake_hooks)
        if (!target || hook.target == target) hook.enabled = false;
    return MH_OK;
}
extern "C" MH_STATUS WINAPI MH_RemoveHook(void*) {
    s_remove_calls.fetch_add(1);
    return MH_OK;
}

int main() {
    Hooks_ResetInstallAdmission();
    TestSerializedPublication();
    TestRetainedEnableFailure();
    TestExactTargetThunks();
      TestLoaderNotifications();
    Hooks_BeginShutdown();
    static std::atomic<TestFunction> original{nullptr};
    Check(InstallHook(reinterpret_cast<void*>(&Target<6>),
          reinterpret_cast<void*>(&Target<7>), &original) == MH_ERROR_DISABLED,
          "shutdown closes install admission");
    Check(s_remove_calls.load() == 0, "no exposed trampoline was removed");
    std::puts("Hook installation and exact-target thunk regressions passed.");
    return 0;
}