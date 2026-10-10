// White-box production regression: compile this file with src/hooks.cpp.
// Do NOT also compile vsync_control.cpp/loadlib_hooks.cpp or link MinHook:
// implementations are included below, with deterministic boundary fakes.
// Windows x64/Win32, C++17. This validates hook ownership/routing, not real
// instruction patching, loader-lock behavior, driver calls or frame pacing.
#include "../src/vsync_control.cpp"
#include "../src/loadlib_hooks.cpp"
#include "presentation_gate.h" // PresentGate_MakeAttribution (header-only)
#include "pacing_resolver.h"   // H3 pipeline publication for the GPU gate route

#ifdef _WIN64
void FgInput_BeginWorkerLifetime() noexcept {}
void FgInput_InstallAndDrain() noexcept {}
void FgInput_Drain() noexcept {}
void StreamlineFactoryProbe_Drain(bool) noexcept {}
void StreamlineFactoryProbe_TryWorker(HMODULE, bool) noexcept {}
void StreamlineProxy_Drain(bool) noexcept {}
void StreamlineQueue_Drain(bool) noexcept {}
#endif

#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cstring>
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
NativeGateContext s_native_context{};
bool s_capture_native = false;
unsigned s_native_holds = 0;
unsigned s_native_calls = 0;
PresentTimelineSpan s_captured_span{};
// Attempt span handed to the open readiness scope; the hook still owns it.
const PresentTimelineSpan* s_readiness_span = nullptr;
int64_t s_native_release_qpc = 0;
int64_t s_readiness_qpc = 0;
unsigned s_readiness_entries = 0, s_readiness_completions = 0;
bool s_early_readiness=false;
GpuPresentExecution s_gpu_execution=GpuPresentExecution::WaitCommitted;
// Pacing telemetry sink: Present records pushed by the production hooks.
unsigned s_pacing_present_records = 0;
PacingPresentRecord s_last_pacing_present{};
constexpr uint64_t kFakeGpuTicketBase = 5000;

HRESULT STDMETHODCALLTYPE NativeProbe(IDXGISwapChain*, UINT, UINT) {
      ++s_native_calls;
      Check(CapturePresentQpc() >= s_native_release_qpc, "native hold precedes original call");
      if (s_capture_native) Check(s_readiness_entries == s_readiness_completions+1,
            "readiness signal boundary precedes native call");
      return S_OK;
}
HRESULT STDMETHODCALLTYPE NativeProbe1(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*) {
      return NativeProbe(nullptr, 0, 0);
}

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

    // These fake objects are deliberately not the selected swapchain (0x1000).
    // Shared implementation discovery must not override their arguments or
    // enter the selected swapchain's observer/gate, even with forced VSync.
    const auto old_mode = s_vsync_mode.load();
    const auto old_interval = s_game_sync_interval.load();
    const auto old_readiness = s_readiness_entries;
    for (int forced_mode : {1, 2}) {
        s_vsync_mode.store(forced_mode);
        Check(s_present_thunks[0](reinterpret_cast<IDXGISwapChain*>(&swapchain), 3, 4) == 101,
              "unselected Present bypasses forced VSync");
        Check(s_present1_thunks[0](reinterpret_cast<IDXGISwapChain1*>(&swapchain), 3, 4, &parameters) == 201,
              "unselected Present1 preserves arguments");
    }
    Check(s_game_sync_interval.load() == old_interval && s_readiness_entries == old_readiness,
          "unselected instances cannot update selected state or enter readiness");
    s_vsync_mode.store(old_mode);

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

void TestNativeGateHookPlacement() {
      const auto present_original = s_present_targets[0].original.load();
      const auto present1_original = s_present1_targets[0].original.load();
      s_present_targets[0].original = NativeProbe;
      s_present1_targets[0].original = NativeProbe1;
      NativeGate_SetMode(NativeGateMode::Native);
      s_native_context = {0x1000, 1, 2, 3, 0, 10'000'000, true, true};
      const unsigned pacing_records_before = s_pacing_present_records;
      for (uint64_t frame = 1; frame <= 34; ++frame) {
            s_native_context.now_qpc = CapturePresentQpc();
            const PacingPlanKey key{PacingIdentityDomain::NvApiFrame, 2, 3, frame};
            const bool transferred = NativeGate_OnMarker(s_native_context, key);
            Check(transferred == (frame > 32), "native hook requires 32 mappings");
            NativeGate_OnMarkerReleased(s_native_context.now_qpc);
            // PRESENT_START executor outcome for this frame (same thread).
            PresentationGateResult marker_gate{};
            marker_gate.status = PacingPlanStatus::SourceOnly;
            PacingTelemetry_SetPresentAttribution(PresentGate_MakeAttribution(
                PacingPlanKey_Frame(key), s_native_context.now_qpc, marker_gate));
            NativeGate_BeginScope(0x1000, 1);
            s_capture_native = frame == 34; // qualification and first active frame with CSV off
            HRESULT result = frame == 34
                  ? s_present1_thunks[0](reinterpret_cast<IDXGISwapChain1*>(0x1000), 0, 0, nullptr)
                  : s_present_thunks[0](reinterpret_cast<IDXGISwapChain*>(0x1000), 0, 0);
            Check(result == S_OK, "native prototype preserves HRESULT");
            NativeGate_EndScope(0x1000, 1);
            // plan_status precedence: H2 when it ran for this call, else the
            // PRESENT_START executor's.
            Check(s_last_pacing_present.plan_status == static_cast<uint8_t>(frame > 32
                      ? PacingPlanStatus::Held : PacingPlanStatus::SourceOnly),
                  "Present record carries the H2 status, else the PRESENT_START status");
      }
      Check(s_native_holds == 2 && s_native_calls == 34, "CSV off/on native holds exactly once");
      Check(s_captured_span.native_gate.source_frame_id == 34, "candidate key attached to captured attempt");
      Check(s_captured_span.p_qpc >= s_captured_span.native_gate.native_release_qpc &&
            s_captured_span.e_qpc >= s_captured_span.p_qpc, "P/E excludes gate hold");
      Check(s_pacing_present_records - pacing_records_before == 34,
            "one pacing Present record per selected native call");
      Check(s_last_pacing_present.native_swapchain == 0x1000 &&
            s_last_pacing_present.thread_id == GetCurrentThreadId() &&
            s_last_pacing_present.result == static_cast<uint32_t>(S_OK),
            "pacing Present record identifies the selected call");
      Check(s_last_pacing_present.p_qpc >= s_captured_span.native_gate.native_release_qpc &&
            s_last_pacing_present.e_qpc >= s_last_pacing_present.p_qpc,
            "pacing P/E bracket only the native call");
      // The fake native gate releases before its target: not a completed hold.
      Check(s_last_pacing_present.output_kind == PacingOutputKind::None &&
            s_last_pacing_present.output_arrival_qpc == 0,
            "a native gate that did not reach its target reports no CPU hold");
#ifdef _WIN64
        // The GPU gate requires readiness while the resolved rung is H3.
        PacingPipelineSnapshot h3{};
        h3.mode=PacingTimingMode::Vrr;h3.route=PacingRoute::NvApiMarkers;
        h3.placement=PacingSourcePlacement::Marker;h3.rung=PacingRung::GpuQueueWait;
        h3.reason=PacingFallbackReason::None;
        PacingPipeline_Publish(h3);s_early_readiness=true;
      s_capture_native=false;
        // Source IDs continue from the native qualification loop above.
        for(uint64_t frame=35;frame<=40;++frame) {
              s_gpu_execution=frame<37?GpuPresentExecution::WaitCommitted:
                    frame<39?GpuPresentExecution::Handled:GpuPresentExecution::Fallback;
              s_native_context.now_qpc=CapturePresentQpc();
              const PacingPlanKey key{PacingIdentityDomain::NvApiFrame,2,3,frame};
              Check(NativeGate_OnMarker(s_native_context,key),"GPU gate native scope qualifies");
              NativeGate_OnMarkerReleased(s_native_context.now_qpc);NativeGate_BeginScope(0x1000,1);
              const auto holds=s_native_holds;
              const auto result=(frame&1)?s_present_thunks[0](reinterpret_cast<IDXGISwapChain*>(0x1000),0,0):
                    s_present1_thunks[0](reinterpret_cast<IDXGISwapChain1*>(0x1000),0,0,nullptr);
              Check(result==S_OK && s_native_holds==holds+(s_gpu_execution==GpuPresentExecution::Fallback?1u:0u),
                    "only rejected GPU admission executes exactly one native CPU hold");NativeGate_EndScope(0x1000,1);
              // Output precedence: committed GPU attribution, then an
              // uninstrumented replacement (Handled), then no hold (Fallback).
              if(s_gpu_execution==GpuPresentExecution::WaitCommitted)
                    Check(s_last_pacing_present.output_kind==PacingOutputKind::GpuQueueWait &&
                          s_last_pacing_present.gpu_ticket==kFakeGpuTicketBase+frame,
                          "committed GPU wait attributes its ticket to the Present record");
              else if(s_gpu_execution==GpuPresentExecution::Handled)
                    Check(s_last_pacing_present.output_kind==PacingOutputKind::Unknown &&
                          s_last_pacing_present.gpu_ticket==0,
                          "a replaced call without GPU attribution reports Unknown");
              else
                    Check(s_last_pacing_present.output_kind==PacingOutputKind::None &&
                          s_last_pacing_present.gpu_ticket==0,
                          "a rejected GPU admission leaves no stale GPU attribution");
              // plan_status precedence: GPU attribution, then H2 (ran only on
              // Fallback), else the PRESENT_START status (none here).
              const PacingPlanStatus expected_status=
                    s_gpu_execution==GpuPresentExecution::WaitCommitted?PacingPlanStatus::Late:
                    s_gpu_execution==GpuPresentExecution::Fallback?PacingPlanStatus::Held:
                    PacingPlanStatus::Unknown;
              Check(s_last_pacing_present.plan_status==static_cast<uint8_t>(expected_status),
                    "Present record plan_status follows GPU, H2, PRESENT_START precedence");
        }
        s_capture_native=true;
        PacingPipeline_Publish(PacingPipelineSnapshot{});s_early_readiness=false;
#endif
      NativeGate_SetMode(NativeGateMode::Marker);
      // The observer is capture-scoped, not dependent on native gate selection.
      Check(s_present_thunks[0](reinterpret_cast<IDXGISwapChain*>(0x1000),0,0)==S_OK,
            "Marker Present preserves result with observer boundary");
      Check(s_present1_thunks[0](reinterpret_cast<IDXGISwapChain1*>(0x1000),0,0,nullptr)==S_OK,
            "Marker Present1 preserves result with observer boundary");
      Check(s_readiness_entries==(sizeof(void*)==8?9u:3u) && s_readiness_completions==s_readiness_entries,
            "readiness wraps each captured or runtime native invocation exactly once");
      s_capture_native = false;
      s_present_targets[0].original = present_original;
      s_present1_targets[0].original = present1_original;
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
static thread_local char s_last_test_log[2048]{};
void Log_Write(LogLevel, const char* format, ...) {
      va_list arguments;
      va_start(arguments, format);
      vsnprintf(s_last_test_log, sizeof(s_last_test_log), format, arguments);
      va_end(arguments);
}
RuntimeCallbackGuard::RuntimeCallbackGuard() noexcept : stopping_(s_test_stopping) {}
RuntimeCallbackGuard::~RuntimeCallbackGuard() = default;
bool RuntimeLifecycle_IsStopping() noexcept { return s_test_stopping; }
HANDLE RuntimeLifecycle_GetStopEvent() noexcept { return nullptr; }
bool BackgroundTasks_Start(BackgroundTaskProc, void*) noexcept {
      ++s_background_starts;
      return false;
}
#ifdef _WIN64
bool HookStreamlinePCL(HMODULE) { ++s_synchronous_installs; return false; }
void NGXHooks_TryInstall() { ++s_synchronous_installs; }
#endif
bool FrameTimeline_BeginPresentCall(
      uint64_t, PresentTimelineEntryPoint, PresentTimelineCall*, bool runtime) noexcept { return s_capture_native || runtime; }
bool FrameTimeline_BeginPresentAttempt(
      const PresentTimelineCall&, uint32_t, PresentTimelineSpan* span, bool runtime) noexcept {
      span->capture_epoch=s_capture_native?1:0;return s_capture_native || runtime;
}
uint64_t FrameTimeline_GetPresentSwapchainGeneration(uint64_t) noexcept { return 1; }
uint64_t SwapMgr_GetNativeHandle() { return 0x1000; }
NativeGateContext NativeGate_GetContext(int64_t now) noexcept { auto c = s_native_context; c.now_qpc = now; return c; }
void NativeGate_Execute(NativeGateCall* call, uint64_t) noexcept {
      if (call->trace.status != NativeGateStatus::Native) return;
      ++s_native_holds;
      s_native_release_qpc = CapturePresentQpc();
      call->trace.native_release_qpc = s_native_release_qpc;
      call->trace.plan_status = static_cast<uint8_t>(PacingPlanStatus::Held);
}

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
      NativeGate_SetMode(NativeGateMode::Shadow);
      TracePresentImplementation("Present", reinterpret_cast<void*>(1), reinterpret_cast<void*>(2),
                                 MH_ERROR_NOT_EXECUTABLE, false);
      Check(std::strstr(s_last_test_log, "module=unavailable") != nullptr &&
            std::strstr(s_last_test_log, "method=Present") != nullptr,
            "failed target is reported without module lookup or object dereference");
      NativeGate_SetMode(NativeGateMode::Marker);
      s_last_test_log[0] = '\0';
      TracePresentImplementation("Present", nullptr, nullptr, MH_OK, false);
      Check(s_last_test_log[0] == '\0', "implementation diagnostics require Shadow mode");
    Hooks_ResetInstallAdmission();
    TestSerializedPublication();
    TestRetainedEnableFailure();
    TestExactTargetThunks();
      TestNativeGateHookPlacement();
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
GpuReadinessCall::GpuReadinessCall(const PresentTimelineSpan* span, bool before_gate) noexcept {
      s_readiness_span = span;
      if (!span) return;
      Check(span->p_qpc==0 && span->e_qpc==0, "signal instrumentation outside native P/E");
      s_readiness_qpc=CapturePresentQpc();
      Check(before_gate==s_early_readiness,"pre-hold submission only on requested route");
      Check(s_readiness_qpc>=span->native_gate.native_release_qpc, "signal follows existing gate hold");
      ++s_readiness_entries;
}
GpuReadinessCall::~GpuReadinessCall() = default;
// Completion runs after the native call, when the attempt span handed to the
// scope holds its final gate trace and P/E; capture it for the placement checks.
void GpuReadinessCall::Complete(bool success, bool retried) noexcept {
      const PresentTimelineSpan* span = s_readiness_span;
      s_readiness_span = nullptr;
      if (!span) return;
      Check(success && !retried && span->p_qpc>=s_readiness_qpc && span->e_qpc>=span->p_qpc,
              "completion retains actual original result and P/E chronology");
      s_captured_span = *span;
      ++s_readiness_completions;
}
#ifdef _WIN64
GpuPresentExecution GpuPresent_Execute(NativeGateCall* native,GpuReadinessCall*,PresentTimelineSpan* span) noexcept {
      if(s_gpu_execution==GpuPresentExecution::Fallback)return s_gpu_execution;
      Check(span!=nullptr,"owned GPU call has a runtime Present span");
      Check(s_readiness_entries==s_readiness_completions+1 && span->p_qpc==0,"fence boundary precedes GPU hold and P");
      native->trace.native_release_qpc=CapturePresentQpc();
      // Boundary contract of the real gate: only committed waits own the output.
      if(s_gpu_execution==GpuPresentExecution::WaitCommitted) {
            PacingGpuOutputAttribution output;
            output.kind=PacingOutputKind::GpuQueueWait;
            output.ticket=kFakeGpuTicketBase+native->trace.source_frame_id;
            output.plan_status=static_cast<uint8_t>(PacingPlanStatus::Late);
            PacingTelemetry_SetGpuOutputAttribution(output);
      }
      return s_gpu_execution;
}
#endif
void PacingTelemetry_PushPresent(const PacingPresentRecord& record) noexcept {
      ++s_pacing_present_records;
      s_last_pacing_present = record;
}
// DXGI display source intake (pacing_telemetry.cpp); the hook tests do not
// exercise frame statistics.
void PacingTelemetry_RecordDxgiPresent(IDXGISwapChain*, const PacingPresentRecord&) noexcept {}
int64_t GetQPCFrequency() { return 10'000'000; }