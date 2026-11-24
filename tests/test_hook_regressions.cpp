// White-box production regression: compile this file with src/hooks.cpp.
// Do NOT also compile vsync_control.cpp/loadlib_hooks.cpp or link MinHook:
// implementations are included below, with deterministic boundary fakes.
// Windows x64/Win32, C++17. This validates hook ownership/routing, not real
// instruction patching, loader-lock behavior, driver calls or frame pacing.
#include "../src/vsync_control.cpp"
#include "../src/streamline_base_present.cpp"
#include "../src/loadlib_hooks.cpp"
#include "presentation_gate.h" // PresentGate_MakeAttribution (header-only)
#include "pacing_resolver.h"   // H3 pipeline publication for the GPU gate route
#include "native_queue.h"
#include <new>

#ifdef _WIN64
void FgInput_BeginWorkerLifetime() noexcept {}
void FgInput_InstallAndDrain() noexcept {}
void FgInput_Drain() noexcept {}
void StreamlineFactoryProbe_Drain(bool) noexcept {}
void StreamlineFactoryProbe_TryWorker(HMODULE, bool) noexcept {}
void StreamlineProxy_Drain(bool) noexcept {}
void StreamlineProxy_NoteVtableCopy(uint64_t, uint64_t) noexcept {}
bool StreamlineProxy_IsProxyClass(uint64_t) noexcept { return false; }
thread_local StreamlineInputScope* StreamlineInputScope::current_ = nullptr;
StreamlineInputScope::StreamlineInputScope(uint64_t, UINT sync, UINT) noexcept : sync_interval(sync), previous_(current_) { current_ = this; }
StreamlineInputScope::~StreamlineInputScope() { current_ = previous_; }
void StreamlineQueue_Drain(bool) noexcept {}
#endif

#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cstring>
#include <mutex>
#include <thread>

// The newest log line of the production code under test (Log_Write below keeps only the last).
static thread_local char s_last_test_log[2048]{};

namespace {
void Check(bool condition, const char* message) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::abort();
}

struct FakeHook {
    void* target = nullptr;
    bool enabled = false;
    bool queued_enable = false;
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
thread_local unsigned s_callback_depth = 0, s_readiness_depth = 0;
thread_local bool s_raise_at_selection = false;
uint64_t s_native_handle_override = 0; // the swap manager's native handle seen by the production detours (0: 0x1000)
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
uint64_t s_last_timeline_native = 0, s_last_gate_native = 0, s_last_dxgi_receiver = 0;
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

// ── The private vtable copy of the selected object: the route's Present entry ──
// A swapchain-like object whose vtable names code of this image for 43 slots, driven through the production
// install / reconcile / destroy / teardown and the production detours. The fake native handle (0x1000) is not
// these objects, so every body that runs is the identity rejection path: it is counted once per outermost
// entry, which is how "one body per call" is observed here.
unsigned s_vt_present_calls = 0, s_vt_present1_calls = 0;
bool s_vt_forward_same = false;
IDXGISwapChain* s_vt_forward_other = nullptr;
const DXGI_PRESENT_PARAMETERS* s_vt_expected_parameters = nullptr;
size_t s_vt_first_record = vtable_copy::kNoRecord;
constexpr size_t kVtSlots = 43;

HRESULT STDMETHODCALLTYPE VtSlot() { return 0; }
// Slot 0 of the test vtables (IUnknown::QueryInterface): the install reads the object through it. A real signature:
// on Win32 the callee pops its arguments.
HRESULT STDMETHODCALLTYPE VtQueryInterface(IDXGISwapChain*, REFIID, void** out) {
    if (out) *out = nullptr;
    return E_NOINTERFACE;
}
HRESULT STDMETHODCALLTYPE VtPresentAlt(IDXGISwapChain*, UINT, UINT) { return 9; }
bool RejectAllVptrs(uintptr_t) { return false; }
HRESULT STDMETHODCALLTYPE InlineProbeOriginal(IDXGISwapChain*, UINT, UINT) { return 7; }
HRESULT STDMETHODCALLTYPE VtPresent(IDXGISwapChain* self, UINT interval, UINT flags) {
    ++s_vt_present_calls;
    Check(interval == 3 && flags == 4, "the vtable original gets the caller's arguments");
    HRESULT result = 1;
    // The same object through the inline patch's detour: an inner entry.
    if (s_vt_forward_same) result += s_present_thunks[0](self, interval, flags);
    // Another object from inside the original: its own outermost entry.
    if (s_vt_forward_other) result += s_present_thunks[0](s_vt_forward_other, interval, flags);
    return result;
}
HRESULT STDMETHODCALLTYPE VtPresent1(IDXGISwapChain1*, UINT interval, UINT flags,
                                     const DXGI_PRESENT_PARAMETERS* parameters) {
    ++s_vt_present1_calls;
    Check(interval == 3 && flags == 4 && parameters == s_vt_expected_parameters,
          "the vtable Present1 original gets the caller's parameters unchanged");
    return 21;
}
// Another module's detour on its own vtable after ours: it calls the entry it saved (our detour).
HRESULT STDMETHODCALLTYPE ForeignPresent(IDXGISwapChain* self, UINT interval, UINT flags) {
    return s_vtable_present_thunks[s_vt_first_record](self, interval, flags) + 1000;
}

struct VtObject { void** vptr; };
VtObject s_nest[5];
bool s_nest_reentered = false;
// The inline original of the nesting test: each object's Present calls the next object's (through the inline
// detour); the deepest calls its own once more (an inner entry), then ends.
HRESULT STDMETHODCALLTYPE NestLevel(IDXGISwapChain* self_object, UINT interval, UINT flags) {
    const size_t index = static_cast<size_t>(reinterpret_cast<VtObject*>(self_object) - s_nest);
    if (index + 1 < 5)
        return s_present_thunks[0](reinterpret_cast<IDXGISwapChain*>(&s_nest[index + 1]), interval, flags);
    if (s_nest_reentered) return 7;
    s_nest_reentered = true;
    return s_present_thunks[0](self_object, interval, flags);
}

uint64_t EntryCount(NativeGateEntry entry) {
    return NativeGate_Diagnostics().entries[static_cast<size_t>(entry)];
}

// A vtable of `slots` entries (the locator at index -1) that ends exactly where readable memory ends: the page
// after it has `guard_protect` (PAGE_NOACCESS or PAGE_GUARD), so a verbatim copy stops there. Every slot names VtSlot.
char* s_last_guard_page = nullptr;
void** VtableBeforeGuardPage(size_t slots, DWORD guard_protect = PAGE_NOACCESS) {
    char* base = static_cast<char*>(VirtualAlloc(nullptr, 2 * 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    Check(base != nullptr, "allocate a vtable page");
    DWORD old = 0;
    Check(VirtualProtect(base + 4096, 4096, guard_protect, &old) != FALSE, "protect the guard page");
    s_last_guard_page = base + 4096;
    void** vptr = reinterpret_cast<void**>(base + 4096) - slots;
    vptr[-1] = base; // the locator
    for (size_t i = 0; i < slots; ++i) vptr[i] = reinterpret_cast<void*>(&VtSlot);
    vptr[0] = reinterpret_cast<void*>(&VtQueryInterface);
    return vptr;
}
// The test vtables above live in private pages; a class vtable lives in a module's image. This stands in for the
// production residency check where the page tests need it, so the check itself is tested with the real one.
bool AcceptAllVptrs(uintptr_t) { return true; }
bool LastLogHas(const char* text) { return std::strstr(s_last_test_log, text) != nullptr; }
// Executable memory that belongs to no module: a hook's private thunk. Never called.
void* NonModuleCode() {
    unsigned char* page = static_cast<unsigned char*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE,
                                                                   PAGE_EXECUTE_READWRITE));
    Check(page != nullptr, "allocate non-module code");
    page[0] = 0xC3;
    return page;
}

void TestVtableCopyEntry() {
    const auto initial_restores = s_vtable_restores.load();
    static void* vtable_storage[1 + kVtSlots];
    static void* foreign_storage[1 + kVtSlots];
    struct FakeObject { void** vptr; };
    static FakeObject object, other;
    vtable_storage[0] = foreign_storage[0] = reinterpret_cast<void*>(&object); // the locator: copied as it is
    for (size_t i = 0; i < kVtSlots; ++i) vtable_storage[1 + i] = foreign_storage[1 + i] = reinterpret_cast<void*>(&VtSlot);
    vtable_storage[1 + 8] = reinterpret_cast<void*>(&VtPresent);
    vtable_storage[1 + 22] = foreign_storage[1 + 22] = reinterpret_cast<void*>(&VtPresent1);
    vtable_storage[1 + 0] = reinterpret_cast<void*>(&VtQueryInterface);
    foreign_storage[1 + 8] = reinterpret_cast<void*>(&ForeignPresent);
    void** const original_vptr = vtable_storage + 1;
    void** const foreign_vptr = foreign_storage + 1;
    object.vptr = other.vptr = original_vptr;
    auto* self = reinterpret_cast<IDXGISwapChain*>(&object);
    auto* self1 = reinterpret_cast<IDXGISwapChain1*>(&object);
    auto* other_self = reinterpret_cast<IDXGISwapChain*>(&other);
    const uint64_t object_handle = reinterpret_cast<uint64_t>(&object);
    const auto present_original = s_present_targets[0].original.load();
    s_present_targets[0].original = InlineProbeOriginal;
    DXGI_PRESENT_PARAMETERS parameters{};
    s_vt_expected_parameters = &parameters;

    // Selection: the object, and only the object, gets our copy; the class vtable is not written.
    VSync_OnNativeSelected(object_handle);
    const size_t first = s_records.IndexOfCopy(reinterpret_cast<uintptr_t>(object.vptr));
    s_vt_first_record = first;
    Check(first != vtable_copy::kNoRecord && object.vptr != original_vptr, "selection points the object at our copy");
    Check(other.vptr == original_vptr && original_vptr[8] == reinterpret_cast<void*>(&VtPresent),
          "another object of the class and the class vtable are untouched");
    Check(object.vptr[8] == reinterpret_cast<void*>(s_vtable_present_thunks[first]) &&
          object.vptr[22] == reinterpret_cast<void*>(s_vtable_present1_thunks[first]),
          "slots 8 and 22 of the copy name our detours");
    Check(object.vptr[-1] == original_vptr[-1] && object.vptr[7] == original_vptr[7] &&
          object.vptr[kVtSlots - 1] == original_vptr[kVtSlots - 1], "the locator and every other entry are copied");
    Check(s_records.Get(first)->original_present == reinterpret_cast<uintptr_t>(&VtPresent),
          "the record keeps the slot value read at copy time");
    const NativeTargets inline_targets = ResolveNativeTargets(static_cast<uintptr_t>(object_handle), 0, 0);
    Check(inline_targets.carried && inline_targets.present == reinterpret_cast<void*>(&VtPresent) &&
          inline_targets.present1 == reinterpret_cast<void*>(&VtPresent1),
          "the inline patch is installed on the implementation's function the record forwards to, not on our detour");
    char text[300];
    char text_route[800];
    DescribeVtableSlot("Present", object_handle, 8, false, s_present_targets, text, sizeof(text));
    Check(std::strstr(text, "our vtable detour") != nullptr && std::strstr(text, "installed") != nullptr,
          "the diagnostics report our detour in the slot as installed");

    // One entry: the vtable detour runs the body (here the rejection) once and calls the record's original.
    s_vt_forward_same = false;
    s_vt_forward_other = nullptr;
    uint64_t vt_before = EntryCount(NativeGateEntry::VtablePresent), typed_before = EntryCount(NativeGateEntry::TypedPresent),
             inner_before = EntryCount(NativeGateEntry::InnerPassThrough), rejected_before = NativeGate_PresentsRejected(false);
    unsigned calls_before = s_vt_present_calls;
    Check(reinterpret_cast<PFN_Present>(object.vptr[8])(self, 3, 4) == 1, "the vtable detour returns the original's result");
    Check(s_vt_present_calls == calls_before + 1 && EntryCount(NativeGateEntry::VtablePresent) == vt_before + 1 &&
          EntryCount(NativeGateEntry::TypedPresent) == typed_before &&
          EntryCount(NativeGateEntry::InnerPassThrough) == inner_before &&
          NativeGate_PresentsRejected(false) == rejected_before + 1, "one vtable entry, one body");

    // Dedup: the same object reaching the inline detour from inside the original passes straight through.
    s_vt_forward_same = true;
    vt_before = EntryCount(NativeGateEntry::VtablePresent); typed_before = EntryCount(NativeGateEntry::TypedPresent);
    inner_before = EntryCount(NativeGateEntry::InnerPassThrough); rejected_before = NativeGate_PresentsRejected(false);
    Check(reinterpret_cast<PFN_Present>(object.vptr[8])(self, 3, 4) == 1 + 7, "the inner entry reaches the inline original");
    Check(EntryCount(NativeGateEntry::VtablePresent) == vt_before + 1 && EntryCount(NativeGateEntry::TypedPresent) == typed_before + 1 &&
          EntryCount(NativeGateEntry::InnerPassThrough) == inner_before + 1 &&
          NativeGate_PresentsRejected(false) == rejected_before + 1,
          "both entries counted per kind, the inner one separately, the body once");

    // Another object from inside the original is not suppressed: its own outermost entry, its own body.
    s_vt_forward_same = false;
    s_vt_forward_other = other_self;
    inner_before = EntryCount(NativeGateEntry::InnerPassThrough); rejected_before = NativeGate_PresentsRejected(false);
    Check(reinterpret_cast<PFN_Present>(object.vptr[8])(self, 3, 4) == 1 + 7, "the other object reaches its original");
    Check(EntryCount(NativeGateEntry::InnerPassThrough) == inner_before &&
          NativeGate_PresentsRejected(false) == rejected_before + 2, "a Present on another swapchain is not suppressed");
    s_vt_forward_other = nullptr;

    // Present1: parameters and result pass through unchanged.
    vt_before = EntryCount(NativeGateEntry::VtablePresent1);
    Check(reinterpret_cast<PFN_Present1>(object.vptr[22])(self1, 3, 4, &parameters) == 21 &&
          s_vt_present1_calls == 1 && EntryCount(NativeGateEntry::VtablePresent1) == vt_before + 1,
          "the vtable Present1 detour forwards its parameters and result");

    // Dedup beyond four objects: five distinct objects nested on one thread (each original presents the next one
    // through the inline detour); the fifth object and its inner entry still run one body: five bodies, one pass-through.
    for (VtObject& n : s_nest) n.vptr = original_vptr;
    s_nest_reentered = false;
    const auto saved_inline = s_present_targets[0].original.load();
    s_present_targets[0].original = NestLevel;
    inner_before = EntryCount(NativeGateEntry::InnerPassThrough); rejected_before = NativeGate_PresentsRejected(false);
    Check(s_present_thunks[0](reinterpret_cast<IDXGISwapChain*>(&s_nest[0]), 3, 4) == 7, "five nested objects reach the original");
    Check(NativeGate_PresentsRejected(false) == rejected_before + 5 && EntryCount(NativeGateEntry::InnerPassThrough) == inner_before + 1,
          "the fifth nested object runs its body once and its inner entry passes through");
    s_present_targets[0].original = saved_inline;

    // Another module replaces the vptr AFTER our copy: left as it is. Counted, no new record, no chain, no
    // vptr write against it.
    const size_t records_before = s_records.Size();
    Check(!vtable_copy::ShouldRestoreVptr(reinterpret_cast<uintptr_t>(foreign_vptr), reinterpret_cast<uintptr_t>(object.vptr)),
          "a foreign vptr is not ours");
    VSync_ObserveNativeVptr(object_handle); // ours: nothing happens
    Check(s_records.Size() == records_before && s_vptr_replacements.load() == 0, "an unchanged vptr is a no-op");
    object.vptr = foreign_vptr;
    VSync_ObserveNativeVptr(object_handle);
    Check(s_vptr_replacements.load() == 1 && object.vptr == foreign_vptr && s_records.Size() == records_before &&
          s_records.IndexOfCopy(reinterpret_cast<uintptr_t>(object.vptr)) == vtable_copy::kNoRecord,
          "a vptr another module replaced after us is counted and left: no new copy, no write");
    VSync_ObserveNativeVptr(object_handle);
    Check(s_vptr_replacements.load() == 1 && s_records.Size() == records_before && object.vptr == foreign_vptr,
          "the same foreign vptr is not counted again");
    s_present_targets[0].original = InlineProbeOriginal;
    vt_before = EntryCount(NativeGateEntry::VtablePresent); inner_before = EntryCount(NativeGateEntry::InnerPassThrough);
    calls_before = s_vt_present_calls;
    Check(reinterpret_cast<PFN_Present>(object.vptr[8])(self, 3, 4) == 1001,
          "the other module's detour reaches our first detour, which forwards to the original");
    Check(EntryCount(NativeGateEntry::VtablePresent) == vt_before + 1 && s_vt_present_calls == calls_before + 1,
          "one vtable entry, the original once");

    // Destroy: the vptr goes back only while it still equals our copy.
    VSync_OnNativeDestroyed(object_handle);
    Check(object.vptr == foreign_vptr && s_vtable_restores.load() == initial_restores, "destroy leaves a vptr that is not ours");

    // The history is process-global: destroy and init of the same native object do not forget the foreign vptr, and
    // it is left alone on the next selection of this object and of any other (no copy over it, no chain into a cycle).
    s_vtable_failures.store(0);
    VSync_OnNativeSelected(object_handle);
    Check(object.vptr == foreign_vptr && s_records.Size() == records_before && s_vtable_failures.load() == 1 &&
          LastLogHas("foreign vtable"), "a foreign vptr seen over our copy is left alone on re-selection of the same object");
    // The destroy dropped the binding; the history still names the class vtable our copy had replaced, so an inline
    // patch would go on the implementation's own functions, not on the foreign vtable's hook.
    const NativeTargets foreign_targets = ResolveNativeTargets(static_cast<uintptr_t>(object_handle),
        reinterpret_cast<uintptr_t>(&ForeignPresent), reinterpret_cast<uintptr_t>(&VtPresent1));
    Check(!foreign_targets.carried && foreign_targets.present == reinterpret_cast<void*>(&VtPresent) &&
          foreign_targets.present1 == reinterpret_cast<void*>(&VtPresent1),
          "a foreign vptr seen over our copy resolves to the class vtable that copy replaced, after its binding was dropped");
    VSync_OnNativeDestroyed(object_handle);
    other.vptr = foreign_vptr;
    VSync_OnNativeSelected(reinterpret_cast<uint64_t>(&other));
    Check(other.vptr == foreign_vptr && s_records.Size() == records_before && s_vtable_failures.load() == 2,
          "and on the selection of another object");
    VSync_OnNativeDestroyed(reinterpret_cast<uint64_t>(&other));
    other.vptr = original_vptr;
    object.vptr = original_vptr;
    VSync_OnNativeSelected(object_handle); // a re-created swapchain at the same address: the existing record is reused
    Check(s_records.IndexOfCopy(reinterpret_cast<uintptr_t>(object.vptr)) == first && s_records.Size() == records_before,
          "the same vtable image shares its copy and record");
    VSync_OnNativeDestroyed(object_handle);
    Check(object.vptr == original_vptr && s_vtable_restores.load() == initial_restores + 1, "destroy restores the vptr it replaced");

    // Developer key vtable_copy_at_present: a selection leaves an object without a copy as DXGI made it (a layer
    // above inspects it when the creation call returns) and names no owner; the object's first present event makes
    // the copy; a later selection of the object (a resize) keeps it; with the key off a selection copies again.
    {
        const unsigned restores_before = s_vtable_restores.load();
        const uint64_t override_before = s_native_handle_override;
        s_native_handle_override = object_handle;
        g_config.vtable_copy_at_present = true;
        VSync_OnNativeSelected(object_handle);
        Check(object.vptr == original_vptr && s_route_object.load() == 0 && s_records.Size() == records_before,
              "with vtable_copy_at_present a selection leaves the vptr and names no owner");
        VSync_ObserveNativeVptr(object_handle);
        Check(s_records.IndexOfCopy(reinterpret_cast<uintptr_t>(object.vptr)) == first && s_records.Size() == records_before &&
              s_route_object.load() == object_handle,
              "the object's first present event makes the copy and names the selected object as the owner");
        void** const copy_vptr = object.vptr;
        VSync_ObserveNativeVptr(object_handle);
        VSync_OnNativeSelected(object_handle);
        Check(object.vptr == copy_vptr && s_route_object.load() == object_handle,
              "a later present event and a later selection keep the copy");
        VSync_OnNativeDestroyed(object_handle);
        Check(object.vptr == original_vptr && s_vtable_restores.load() == restores_before + 1,
              "destroy restores the vptr the deferred copy replaced");
        // The record of the class vtable as it is at the deferred selection is made there, so a hook another module
        // writes into the class vtable before the first Present is not the earliest record's Present.
        void* const slot5 = vtable_storage[1 + 5];
        vtable_storage[1 + 5] = reinterpret_cast<void*>(&VtQueryInterface); // a class vtable image without a record
        VSync_OnNativeSelected(object_handle);
        Check(object.vptr == original_vptr && s_records.Size() == records_before + 1,
              "a deferred selection makes the record of the class vtable as it is then and leaves the object");
        const size_t prepared = s_records.Size() - 1;
        Check(s_records.Get(prepared)->original_present == reinterpret_cast<uintptr_t>(&VtPresent),
              "the record's Present is the implementation's own");
        vtable_storage[1 + 8] = reinterpret_cast<void*>(&ForeignPresent); // another module hooks the class slot in place
        VSync_ObserveNativeVptr(object_handle);
        Check(s_records.Size() == records_before + 2 &&
              s_records.IndexOfCopy(reinterpret_cast<uintptr_t>(object.vptr)) == prepared + 1,
              "the first present event copies the class vtable as it is then, in a later record");
        VSync_OnNativeDestroyed(object_handle);
        vtable_storage[1 + 8] = reinterpret_cast<void*>(&VtPresent);
        vtable_storage[1 + 5] = slot5;
        g_config.vtable_copy_at_present = false;
        VSync_OnNativeSelected(object_handle);
        Check(s_records.IndexOfCopy(reinterpret_cast<uintptr_t>(object.vptr)) == first,
              "with the key off a selection makes the copy again");
        VSync_OnNativeDestroyed(object_handle);
        s_vtable_restores.store(restores_before);
        s_native_handle_override = override_before;
    }

    // Teardown restores the vptrs that are still ours, no others.
    VSync_OnNativeSelected(object_handle);
    VSync_OnNativeSelected(reinterpret_cast<uint64_t>(&other));
    other.vptr = foreign_vptr;
    VSync_RestoreVtables();
    Check(object.vptr == original_vptr && other.vptr == foreign_vptr,
          "teardown restores only the vptrs that are still ours");
    other.vptr = original_vptr;

    // The vtables below are in private pages: the residency check is replaced where a page vtable must install.
    s_vptr_in_module = AcceptAllVptrs;
    static FakeObject page_object;
    void* const private_thunk = NonModuleCode();

    // ANY of slots 0 to 40 outside every loaded module (a hook's private thunk, whose owner can free it) refuses
    // the install, and the slot is in the log. The vtable ends exactly at unreadable memory with 41 slots.
    s_vtable_failures.store(0);
    for (size_t slot : {size_t(31), size_t(0), vtable_copy::kPresentSlot, vtable_copy::kPresent1Slot, size_t(40)}) {
        void** const page_vptr = VtableBeforeGuardPage(vtable_copy::kRequiredSlots);
        page_vptr[8] = reinterpret_cast<void*>(&VtPresent);
        page_vptr[22] = reinterpret_cast<void*>(&VtPresent1);
        page_vptr[slot] = private_thunk;
        page_object.vptr = page_vptr;
        const uint32_t failures_before = s_vtable_failures.load();
        VSync_OnNativeSelected(reinterpret_cast<uint64_t>(&page_object));
        char needle[32];
        std::snprintf(needle, sizeof(needle), "slot %zu (", slot);
        Check(page_object.vptr == page_vptr && s_vtable_failures.load() == failures_before + 1 && LastLogHas(needle),
              "a non-module pointer in any of slots 0 to 40 refuses the install and logs the slot");
        VSync_OnNativeDestroyed(reinterpret_cast<uint64_t>(&page_object));
    }

    // The same vtable with every slot in a module installs, verbatim, and destroy restores it.
    void** const good_vptr = VtableBeforeGuardPage(vtable_copy::kRequiredSlots);
    good_vptr[8] = reinterpret_cast<void*>(&VtPresent);
    good_vptr[22] = reinterpret_cast<void*>(&VtPresent1);
    page_object.vptr = good_vptr;
    VSync_OnNativeSelected(reinterpret_cast<uint64_t>(&page_object));
    Check(page_object.vptr != good_vptr && s_records.IndexOfCopy(reinterpret_cast<uintptr_t>(page_object.vptr)) !=
          vtable_copy::kNoRecord && page_object.vptr[30] == good_vptr[30] && page_object.vptr[40] == good_vptr[40] &&
          page_object.vptr[-1] == good_vptr[-1], "a vtable with exactly 41 readable slots, all in modules, installs verbatim");
    VSync_OnNativeDestroyed(reinterpret_cast<uint64_t>(&page_object));
    Check(page_object.vptr == good_vptr, "destroy restores it");

    // A copy goes only over a class vtable: a vptr in private memory (the real residency check) is refused.
    s_vptr_in_module = &PinClassVtable;
    page_object.vptr = good_vptr;
    const uint32_t failures_private = s_vtable_failures.load();
    VSync_OnNativeSelected(reinterpret_cast<uint64_t>(&page_object));
    Check(page_object.vptr == good_vptr && s_vtable_failures.load() == failures_private + 1 && LastLogHas("private memory"),
          "a vptr in private memory is never copied over");
    VSync_OnNativeDestroyed(reinterpret_cast<uint64_t>(&page_object));
    s_vptr_in_module = AcceptAllVptrs;

    // Fewer than the 41 slots of IDXGISwapChain4 are readable: the read stops at the guard page WITHOUT touching it
    // (the guard stays set), and nothing is installed.
    void** const short_vptr = VtableBeforeGuardPage(vtable_copy::kRequiredSlots - 1, PAGE_READWRITE | PAGE_GUARD);
    short_vptr[8] = reinterpret_cast<void*>(&VtPresent);
    short_vptr[22] = reinterpret_cast<void*>(&VtPresent1);
    page_object.vptr = short_vptr;
    const uint32_t failures_short = s_vtable_failures.load();
    VSync_OnNativeSelected(reinterpret_cast<uint64_t>(&page_object));
    MEMORY_BASIC_INFORMATION guard_info{};
    Check(VirtualQuery(s_last_guard_page, &guard_info, sizeof(guard_info)) == sizeof(guard_info) &&
          (guard_info.Protect & PAGE_GUARD) != 0, "the guard page was not touched: its guard is still set");
    Check(page_object.vptr == short_vptr && s_vtable_failures.load() == failures_short + 1,
          "a vtable with fewer than 41 readable slots is left alone and refused");
    VSync_OnNativeDestroyed(reinterpret_cast<uint64_t>(&page_object));

    // A foreign vptr over ours when the binding is dropped, with NO present event in between (a resize's destroy and
    // init): destroy remembers it before forgetting the binding, so the selection leaves it; teardown does the same.
    static void* race_storage[2][1 + kVtSlots];
    void** race_vptr[2];
    for (int k = 0; k < 2; ++k) {
        race_storage[k][0] = reinterpret_cast<void*>(&object);
        for (size_t i = 0; i < kVtSlots; ++i) race_storage[k][1 + i] = reinterpret_cast<void*>(&VtSlot);
        race_storage[k][1 + 8] = reinterpret_cast<void*>(&ForeignPresent);
        race_storage[k][1 + 22] = reinterpret_cast<void*>(&VtPresent1);
        race_vptr[k] = race_storage[k] + 1;
    }
    static FakeObject race_object;
    race_object.vptr = original_vptr;
    VSync_OnNativeSelected(reinterpret_cast<uint64_t>(&race_object));
    Check(race_object.vptr != original_vptr, "the race object has our copy");
    race_object.vptr = race_vptr[0]; // another module replaces it; no present event observes that
    Check(!s_history.IsForeign(reinterpret_cast<uintptr_t>(race_vptr[0])), "not remembered yet");
    VSync_OnNativeDestroyed(reinterpret_cast<uint64_t>(&race_object));
    Check(race_object.vptr == race_vptr[0] && s_history.IsForeign(reinterpret_cast<uintptr_t>(race_vptr[0])),
          "destroy remembers the foreign vptr before the binding is cleared");
    s_vtable_failures.store(0);
    VSync_OnNativeSelected(reinterpret_cast<uint64_t>(&race_object));
    Check(race_object.vptr == race_vptr[0] && s_vtable_failures.load() == 1 && LastLogHas("foreign vtable"),
          "the init of the same object leaves the foreign vptr: no copy over it");
    VSync_OnNativeDestroyed(reinterpret_cast<uint64_t>(&race_object));
    race_object.vptr = original_vptr;
    VSync_OnNativeSelected(reinterpret_cast<uint64_t>(&race_object));
    race_object.vptr = race_vptr[1];
    VSync_RestoreVtables(); // teardown drops the binding too
    Check(race_object.vptr == race_vptr[1] && s_history.IsForeign(reinterpret_cast<uintptr_t>(race_vptr[1])),
          "teardown remembers a foreign vptr before dropping the binding");

    // Every binding holds a live copy: a ninth object is refused (no live binding is forgotten, since a foreign
    // replacement of that object would then look like an unseen vtable) until one of them is destroyed.
    static FakeObject live[8];
    for (FakeObject& o : live) {
        o.vptr = original_vptr;
        VSync_OnNativeSelected(reinterpret_cast<uint64_t>(&o));
        Check(o.vptr != original_vptr, "eight objects hold live copies");
    }
    static FakeObject ninth;
    ninth.vptr = original_vptr;
    s_vtable_failures.store(0);
    VSync_OnNativeSelected(reinterpret_cast<uint64_t>(&ninth));
    Check(ninth.vptr == original_vptr && s_vtable_failures.load() == 1 && LastLogHas("bindings are taken"),
          "with every binding taken a new object is refused, not evicting one");
    Check(live[0].vptr != original_vptr && s_records.IndexOfCopy(reinterpret_cast<uintptr_t>(live[0].vptr)) != vtable_copy::kNoRecord,
          "the first object keeps its binding and its copy");
    // A binding whose object's vptr an overlay changed is NOT evicted either (no guess that its copy is gone):
    // the ninth stays refused, and the foreign vptr is remembered when that object's destroy drops the binding.
    static void* overlay_storage[1 + kVtSlots];
    overlay_storage[0] = reinterpret_cast<void*>(&object);
    for (size_t i = 0; i < kVtSlots; ++i) overlay_storage[1 + i] = reinterpret_cast<void*>(&VtSlot);
    overlay_storage[1 + 8] = reinterpret_cast<void*>(&ForeignPresent);
    overlay_storage[1 + 22] = reinterpret_cast<void*>(&VtPresent1);
    void** const overlay_vptr = overlay_storage + 1;
    void** const live1_copy = live[1].vptr;
    live[1].vptr = overlay_vptr;
    VSync_OnNativeSelected(reinterpret_cast<uint64_t>(&ninth));
    Check(ninth.vptr == original_vptr && live[1].vptr == overlay_vptr,
          "a binding whose vptr was changed by an overlay is not evicted: the ninth is still refused");
    VSync_OnNativeDestroyed(reinterpret_cast<uint64_t>(&live[1]));
    Check(live[1].vptr == overlay_vptr && s_history.IsForeign(reinterpret_cast<uintptr_t>(overlay_vptr)),
          "destroy of that object remembers the overlay's vptr");
    live[1].vptr = live1_copy; // keep the object consistent for the cleanup below
    VSync_OnNativeDestroyed(reinterpret_cast<uint64_t>(&live[0]));
    VSync_OnNativeSelected(reinterpret_cast<uint64_t>(&ninth));
    Check(ninth.vptr != original_vptr, "a destroyed object frees a binding for the next one");
    for (FakeObject& o : live) VSync_OnNativeDestroyed(reinterpret_cast<uint64_t>(&o));
    VSync_OnNativeDestroyed(reinterpret_cast<uint64_t>(&ninth));

    // History full: nothing NEW is installed (and it is logged); a vptr already in the history still is.
    s_vptr_in_module = &PinClassVtable;
    for (uintptr_t v = 0x1000; !s_history.Full(); v += 8) s_history.Note(v, vtable_copy::VptrHistory::Kind::Replaced);
    static void* fresh_storage[1 + kVtSlots];
    fresh_storage[0] = reinterpret_cast<void*>(&object);
    for (size_t i = 0; i < kVtSlots; ++i) fresh_storage[1 + i] = reinterpret_cast<void*>(&VtSlot);
    fresh_storage[1 + 8] = reinterpret_cast<void*>(&VtPresent);
    fresh_storage[1 + 22] = reinterpret_cast<void*>(&VtPresent1);
    static FakeObject fresh_object;
    fresh_object.vptr = fresh_storage + 1;
    s_vtable_failures.store(0);
    VSync_OnNativeSelected(reinterpret_cast<uint64_t>(&fresh_object));
    Check(fresh_object.vptr == fresh_storage + 1 && s_vtable_failures.load() == 1 && LastLogHas("history is full"),
          "with the vptr history full a new vptr is not installed over");
    VSync_OnNativeDestroyed(reinterpret_cast<uint64_t>(&fresh_object));
    object.vptr = original_vptr; // already in the history: still installs
    VSync_OnNativeSelected(object_handle);
    Check(object.vptr != original_vptr && s_records.IndexOfCopy(reinterpret_cast<uintptr_t>(object.vptr)) == first,
          "a vptr already in the history is still installed over");
    VSync_OnNativeDestroyed(object_handle);
    const auto handle_of = [](const void* o) { return reinterpret_cast<uint64_t>(o); };
    const auto bindings_of = [&](const void* o) {
        size_t n = 0;
        for (const VtableBinding& b : s_bindings) if (b.object == reinterpret_cast<uintptr_t>(o)) ++n;
        return n;
    };

    // Provenance: an overlay's temporary restore of the original vptr does not erase the installed copy's identity
    // (a refused reinstall keeps it), so when the overlay puts our saved copy back, destroy still restores it.
    static FakeObject prov;
    prov.vptr = original_vptr;
    VSync_OnNativeSelected(handle_of(&prov));
    void** const prov_copy = prov.vptr;
    Check(prov_copy != original_vptr && s_records.IndexOfCopy(reinterpret_cast<uintptr_t>(prov_copy)) != vtable_copy::kNoRecord,
          "the provenance object has our copy");
    s_vptr_in_module = RejectAllVptrs; // the reinstall is refused: nothing but the binding remembers the copy
    prov.vptr = original_vptr;         // the overlay restores the original
    VSync_ObserveNativeVptr(handle_of(&prov));
    Check(prov.vptr == original_vptr, "a refused reinstall leaves the original vptr");
    s_vptr_in_module = &PinClassVtable;
    prov.vptr = prov_copy;             // the overlay restores our saved copy
    VSync_ObserveNativeVptr(handle_of(&prov));
    VSync_OnNativeDestroyed(handle_of(&prov));
    Check(prov.vptr == original_vptr, "destroy restores the copy whose identity survived the overlay's restore");

    // Admission is checked under the vtable lock in both entry points: a callback paused past a stopping check
    // cannot install after the teardown's restoration.
    static FakeObject stopping;
    stopping.vptr = original_vptr;
    s_test_stopping = true;
    VSync_OnNativeSelected(handle_of(&stopping));
    VSync_ObserveNativeVptr(handle_of(&stopping));
    s_test_stopping = false;
    Check(stopping.vptr == original_vptr && bindings_of(&stopping) == 0, "nothing installs or binds while stopping");
    VSync_ObserveNativeVptr(handle_of(&stopping));
    Check(stopping.vptr != original_vptr, "the same present event installs once the addon runs");
    VSync_OnNativeDestroyed(handle_of(&stopping));

    // An unbound object that already shows one of our copies is adopted, never copied again.
    static FakeObject adopted;
    adopted.vptr = prov_copy;
    const size_t records_adopt = s_records.Size();
    const uint32_t adopted_before = s_vtable_adopted.load();
    VSync_ObserveNativeVptr(handle_of(&adopted));
    Check(adopted.vptr == prov_copy && s_records.Size() == records_adopt && s_vtable_adopted.load() == adopted_before + 1 &&
          bindings_of(&adopted) == 1, "an unbound object showing our copy is adopted, not copied");
    VSync_OnNativeDestroyed(handle_of(&adopted));
    Check(adopted.vptr == original_vptr, "its destroy restores the original the record names");
    adopted.vptr = prov_copy;
    VSync_OnNativeSelected(handle_of(&adopted));
    Check(adopted.vptr == prov_copy && s_records.Size() == records_adopt, "selection adopts as well");
    VSync_OnNativeDestroyed(handle_of(&adopted));

    // A lost destroy: the next selection or present of the same address is judged by the same rule on the SAME binding.
    static FakeObject lost;
    lost.vptr = original_vptr;
    VSync_OnNativeSelected(handle_of(&lost));
    void** const lost_copy = lost.vptr;
    Check(lost_copy != original_vptr && bindings_of(&lost) == 1, "the lost-destroy object has a binding and our copy");
    lost.vptr = original_vptr; // no destroy: a new object lives at this address with the class vtable
    const size_t records_lost = s_records.Size();
    VSync_OnNativeSelected(handle_of(&lost));
    Check(lost.vptr == lost_copy && bindings_of(&lost) == 1 && s_records.Size() == records_lost,
          "a selection of the same address installs again on the same binding (one binding, one copy)");
    lost.vptr = original_vptr;
    VSync_ObserveNativeVptr(handle_of(&lost));
    Check(lost.vptr == lost_copy && bindings_of(&lost) == 1, "a present event does the same");
    lost.vptr = lost_copy; // our copy there: kept
    VSync_OnNativeSelected(handle_of(&lost));
    VSync_ObserveNativeVptr(handle_of(&lost));
    Check(lost.vptr == lost_copy && bindings_of(&lost) == 1 && s_records.Size() == records_lost, "our copy there is kept");
    lost.vptr = foreign_vptr; // anything else: the usual foreign rules, still one binding
    VSync_OnNativeSelected(handle_of(&lost));
    Check(lost.vptr == foreign_vptr && bindings_of(&lost) == 1 && s_history.IsForeign(reinterpret_cast<uintptr_t>(foreign_vptr)),
          "another vtable over a lost binding is left alone and remembered");
    VSync_OnNativeDestroyed(handle_of(&lost));

    // ── The inline pair: not installed where the vtable entry carries the route ──
    static FakeObject carried;
    carried.vptr = original_vptr;
    VSync_OnNativeSelected(handle_of(&carried));
    Check(carried.vptr != original_vptr, "the carried object has our copy");
    // Free two typed slots: earlier tests filled the table.
    s_present_targets[14].target.store(nullptr); s_present_targets[14].enabled = false;
    s_present_targets[13].target.store(nullptr); s_present_targets[13].enabled = false;
    const auto typed_has = [&](void* target) {
        for (const auto& e : s_present_targets) if (e.target.load() == target) return true;
        for (const auto& e : s_present1_targets) if (e.target.load() == target) return true;
        return false;
    };
    VSync_InstallDXGIHooksImpl(&carried);
    Check(!typed_has(reinterpret_cast<void*>(&VtPresent)) && !typed_has(reinterpret_cast<void*>(&VtPresent1)),
          "with the copy installed no inline hook is created for Present or Present1");
    DescribeWatch("Present", 0, text, sizeof(text));
    Check(std::strstr(text, "not installed because the vtable entry carries the route") != nullptr &&
          std::strstr(text, "never installed by us") == nullptr, "the diagnostics call it a normal state");
    DescribeVtableRoute(handle_of(&carried), text_route, sizeof(text_route));
    Check(std::strstr(text_route, "inline pair: not installed, the vtable entry carries the route") != nullptr,
          "the vtable route line says so");

    // The copy refused over a vptr in private memory (another module's vtable, not a class vtable): its slot names
    // another module's function, so no inline Present hook is installed on it and the log says why.
    static FakeObject refused;
    void** const refused_vptr = VtableBeforeGuardPage(vtable_copy::kRequiredSlots);
    refused_vptr[8] = reinterpret_cast<void*>(&VtPresentAlt);
    refused_vptr[22] = reinterpret_cast<void*>(&VtPresent1);
    refused.vptr = refused_vptr;
    VSync_OnNativeSelected(handle_of(&refused));
    Check(refused.vptr == refused_vptr, "the copy is refused over a private-memory vptr");
    VSync_InstallDXGIHooksImpl(&refused);
    Check(!typed_has(reinterpret_cast<void*>(&VtPresentAlt)) && LastLogHas("no inline Present patch is installed"),
          "the slot of a vtable in private memory is never patched (the implementation's own Present is not known)");
    VSync_OnNativeDestroyed(handle_of(&refused));

    // The fallback counter: 31 scopes without a vtable entry install nothing; an entry resets the run; a new copy
    // (a new generation) resets it; the 32nd consecutive scope installs the pair once, through one batch.
    s_native_handle_override = handle_of(&carried); // the swap manager's selection
    VSync_OnNativeSelected(handle_of(&carried));    // selected again after the refused one: the fallback's identity is this object's
    s_selected_vtable_entries.fetch_add(1);
    VSync_NoteFinishedPresent(handle_of(&carried)); // syncs the entry count
    for (int i = 0; i < 31; ++i) VSync_NoteFinishedPresent(handle_of(&carried));
    Check(!s_fallback.Fired() && s_fallback.Count() == 31 && !typed_has(reinterpret_cast<void*>(&VtPresent)),
          "31 scopes without a vtable entry install nothing");

    // Another object never counts for, or resets, the selected object's run; finishes of other swapchains do not count;
    // selecting an object again (its existing copy kept: no install, no adoption) refreshes the fallback's identity.
    static FakeObject bob;
    bob.vptr = original_vptr;
    const uint32_t generation_before = s_fallback.Generation();
    s_native_handle_override = handle_of(&bob);
    VSync_OnNativeSelected(handle_of(&bob));
    Check(s_route_object.load() == handle_of(&bob) && s_fallback.Count() == 0 && s_fallback.Generation() != generation_before,
          "selecting another object hands the fallback's identity over and starts its run");
    const uint64_t selected_entries = s_selected_vtable_entries.load(), all_entries = s_vtable_entries.load();
    reinterpret_cast<PFN_Present>(carried.vptr[8])(reinterpret_cast<IDXGISwapChain*>(&carried), 3, 4); // an unselected object's entry
    Check(s_vtable_entries.load() == all_entries + 1 && s_selected_vtable_entries.load() == selected_entries,
          "an entry on an object that is not selected counts in the diagnostics only");
    for (int i = 0; i < 40; ++i) VSync_NoteFinishedPresent(handle_of(&carried));
    Check(!s_fallback.Fired() && s_fallback.Count() == 0, "finishes of an unselected swapchain neither count nor consume the one-shot");
    const size_t records_reselect = s_records.Size();
    const uint32_t generation_bob = s_fallback.Generation();
    s_native_handle_override = handle_of(&carried);
    VSync_OnNativeSelected(handle_of(&carried)); // existing binding, copy kept
    Check(s_route_object.load() == handle_of(&carried) && s_fallback.Generation() != generation_bob &&
          s_fallback.Count() == 0 && s_records.Size() == records_reselect && carried.vptr != original_vptr,
          "selecting an object whose copy is kept publishes its identity again");
    for (int i = 0; i < 10; ++i) VSync_NoteFinishedPresent(handle_of(&carried));
    const uint64_t selected_before_bob = s_selected_vtable_entries.load();
    reinterpret_cast<PFN_Present>(bob.vptr[8])(reinterpret_cast<IDXGISwapChain*>(&bob), 3, 4); // bob is not selected now
    Check(s_selected_vtable_entries.load() == selected_before_bob, "the other object's entry is not selected-object evidence");
    VSync_NoteFinishedPresent(handle_of(&carried));
    Check(s_fallback.Count() == 11 && !s_fallback.Fired(), "and it does not reset the selected object's run");
    // Removing the selected route at destroy advances the fallback's generation: a finisher that counted 31 scopes for
    // the object that is now gone cannot commit the one-shot, and a later object's full run still fires once.
    s_native_handle_override = handle_of(&bob);
    VSync_OnNativeSelected(handle_of(&bob));
    for (int i = 0; i < 31; ++i) VSync_NoteFinishedPresent(handle_of(&bob));
    Check(s_route_object.load() == handle_of(&bob) && s_fallback.Count() == 31 && !s_fallback.Fired(), "31 scopes counted for the object");
    const uint32_t generation_gone = s_fallback.Generation();
    VSync_OnNativeDestroyed(handle_of(&bob));
    Check(s_route_object.load() == 0 && s_fallback.Generation() != generation_gone && s_fallback.Count() == 0,
          "destroying the selected route removes it and advances the counter's generation in one step");
    Check(!s_fallback.NoteScope(false, generation_gone) && !s_fallback.Fired(),
          "the stale 32nd finisher is refused: the one-shot is not consumed for an object that is gone");
    VSync_NoteFinishedPresent(handle_of(&bob));
    Check(s_fallback.Count() == 0, "a finish of the destroyed object counts nothing");
    s_native_handle_override = handle_of(&carried);
    VSync_OnNativeSelected(handle_of(&carried)); // the next selected object starts its own run (it fires below, once)
    reinterpret_cast<PFN_Present>(carried.vptr[8])(reinterpret_cast<IDXGISwapChain*>(&carried), 3, 4); // a vtable entry runs
    VSync_NoteFinishedPresent(handle_of(&carried));
    Check(!s_fallback.Fired() && s_fallback.Count() == 0, "a scope with a vtable entry resets the run");
    for (int i = 0; i < 20; ++i) VSync_NoteFinishedPresent(handle_of(&carried));
    VSync_OnNativeDestroyed(handle_of(&carried));
    carried.vptr = original_vptr;
    VSync_OnNativeSelected(handle_of(&carried)); // a new copy install: a new generation
    Check(s_fallback.Count() == 0 && !s_fallback.Fired(), "a new copy install starts the run again");
    for (int i = 0; i < 31; ++i) VSync_NoteFinishedPresent(handle_of(&carried));
    Check(!s_fallback.Fired() && !typed_has(reinterpret_cast<void*>(&VtPresent)), "31 scopes of the new generation install nothing");
    VSync_NoteFinishedPresent(handle_of(&carried));
    Check(s_fallback.Fired() && typed_has(reinterpret_cast<void*>(&VtPresent)) && typed_has(reinterpret_cast<void*>(&VtPresent1)) &&
          TypedTargetEnabled(s_present_targets, reinterpret_cast<void*>(&VtPresent)) &&
          TypedTargetEnabled(s_present1_targets, reinterpret_cast<void*>(&VtPresent1)),
          "the 32nd scope installs the inline Present / Present1 pair");
    size_t typed_count = 0;
    for (const auto& e : s_present_targets) if (e.target.load()) ++typed_count;
    for (int i = 0; i < 64; ++i) VSync_NoteFinishedPresent(handle_of(&carried));
    size_t typed_count_after = 0;
    for (const auto& e : s_present_targets) if (e.target.load()) ++typed_count_after;
    Check(typed_count == typed_count_after, "the pair is installed once, never again");
    DescribeVtableRoute(handle_of(&carried), text_route, sizeof(text_route));
    Check(std::strstr(text_route, "installed by the fallback") != nullptr, "the route line reports the fallback");
    VSync_OnNativeDestroyed(handle_of(&carried));
    s_native_handle_override = 0;
    s_present_targets[0].original = present_original;
}

// REFramework's D3D12 hook, reduced to what meets our copy (praydog/REFramework, src/D3D12Hook.cpp). Its first phase
// writes its present into the class vtable's slot 8 in place. Its first Present takes that out again (the slot names
// the implementation's Present once more) and puts its own vtable on the object, copied from what the object has (our
// copy), whose saved slot 8 (our detour) it calls from then on. A thread-local depth marks its own nested calls: at
// depth 1 it calls the saved function once more, deeper it returns S_OK WITHOUT presenting. A resize's reset takes
// its vtable off the object and writes the class slot again.
namespace rf {
void* s_class[1 + kVtSlots];
void* s_heap[1 + kVtSlots];
VtObject s_object;
bool s_phase1 = false;
PFN_Present s_saved = nullptr;
int s_depth = 0;
unsigned s_presented = 0; // calls that reached the implementation's Present
HRESULT STDMETHODCALLTYPE ImplPresent(IDXGISwapChain*, UINT, UINT) { ++s_presented; return S_OK; }
HRESULT STDMETHODCALLTYPE Present(IDXGISwapChain* self, UINT interval, UINT flags) {
    if (s_phase1) {
        s_class[1 + 8] = reinterpret_cast<void*>(&ImplPresent);
        void** const vptr = reinterpret_cast<VtObject*>(self)->vptr;
        for (size_t i = 0; i <= kVtSlots; ++i) s_heap[i] = vptr[static_cast<ptrdiff_t>(i) - 1];
        s_saved = reinterpret_cast<PFN_Present>(s_heap[1 + 8]);
        s_heap[1 + 8] = reinterpret_cast<void*>(&Present);
        reinterpret_cast<VtObject*>(self)->vptr = s_heap + 1;
        s_phase1 = false;
    }
    if (s_depth > 1) return S_OK; // "Just returning S_OK"
    ++s_depth;                    // at depth 1: "Attempting to call real present function"
    const HRESULT result = s_saved(self, interval, flags);
    --s_depth;
    return result;
}
} // namespace rf

// Our copy taken while REFramework's present sits in the class slot: the detours call the class slot as it is at each
// call, so every frame reaches the implementation's Present once. A copy that kept calling the value read at copy time
// re-entered REFramework below its own vtable on every frame, and its depth guard presented nothing (a black screen).
void TestVtableForwardsLiveClassSlot() {
    s_history = vtable_copy::VptrHistory{}; // a fresh process history: the test before filled it
    rf::s_class[0] = &rf::s_object;         // the locator
    for (size_t i = 0; i < kVtSlots; ++i) rf::s_class[1 + i] = reinterpret_cast<void*>(&VtSlot);
    rf::s_class[1 + 0] = reinterpret_cast<void*>(&VtQueryInterface);
    rf::s_class[1 + 22] = reinterpret_cast<void*>(&VtPresent1);
    rf::s_class[1 + 8] = reinterpret_cast<void*>(&rf::Present);
    rf::s_phase1 = true;
    void** const class_vptr = rf::s_class + 1;
    rf::s_object.vptr = class_vptr;
    const uint64_t handle = reinterpret_cast<uint64_t>(&rf::s_object);
    auto* self = reinterpret_cast<IDXGISwapChain*>(&rf::s_object);
    VSync_OnNativeSelected(handle);
    void** const copy = rf::s_object.vptr;
    const size_t index = s_records.IndexOfCopy(reinterpret_cast<uintptr_t>(copy));
    Check(index != vtable_copy::kNoRecord &&
          s_records.Get(index)->original_present == reinterpret_cast<uintptr_t>(&rf::Present),
          "our copy is taken while REFramework's present sits in the class slot");
    const auto frame = [&] { return reinterpret_cast<PFN_Present>(rf::s_object.vptr[8])(self, 3, 4); };

    uint64_t vt_before = EntryCount(NativeGateEntry::VtablePresent), inner_before = EntryCount(NativeGateEntry::InnerPassThrough);
    Check(frame() == S_OK && rf::s_presented == 1 && rf::s_object.vptr == rf::s_heap + 1 && !rf::s_phase1 &&
          rf::s_saved == s_vtable_present_thunks[index],
          "the first frame reaches REFramework's first phase through our detour, and presents once");
    Check(EntryCount(NativeGateEntry::VtablePresent) == vt_before + 2 &&
          EntryCount(NativeGateEntry::InnerPassThrough) == inner_before + 1,
          "REFramework calls our detour from inside it: one inner entry, which reaches the implementation");

    vt_before = EntryCount(NativeGateEntry::VtablePresent); inner_before = EntryCount(NativeGateEntry::InnerPassThrough);
    for (int i = 0; i < 3; ++i) Check(frame() == S_OK, "a later frame returns the implementation's result");
    Check(rf::s_presented == 4 && EntryCount(NativeGateEntry::VtablePresent) == vt_before + 3 &&
          EntryCount(NativeGateEntry::InnerPassThrough) == inner_before && rf::s_depth == 0,
          "later frames: REFramework's vtable, our detour once, the implementation's Present once");

    rf::s_object.vptr = copy; // a resize's reset: REFramework's vtable off the object, its class hook written again
    rf::s_class[1 + 8] = reinterpret_cast<void*>(&rf::Present);
    rf::s_phase1 = true;
    Check(frame() == S_OK && frame() == S_OK && rf::s_presented == 6 && rf::s_object.vptr == rf::s_heap + 1,
          "after REFramework hooks the class slot again, every frame still presents");

    rf::s_object.vptr = copy;
    VSync_OnNativeDestroyed(handle);
    Check(rf::s_object.vptr == class_vptr, "destroy restores the class vtable");
}

#ifdef _WIN64
void TestNativeQueueOverflow() {
    constexpr size_t capacity = sizeof(native_queue::entries) / sizeof(native_queue::entries[0]);
    for (uint64_t i = 1; i <= capacity; ++i)
        Check(native_queue::Remember(i, i + 100) == i + 100 && NativeQueue_Peek(i) == i + 100, "stored queue is discoverable at destroy");
    const uint64_t overflow = capacity + 1;
    Check(native_queue::Remember(overflow, overflow + 100) == overflow && NativeQueue_Peek(overflow) == overflow,
          "overflow returns the reported queue, not an undiscoverable resolution");
    for (uint64_t i = 1; i <= capacity; ++i) NativeQueue_Forget(i);
}
namespace output_test {
VtObject input{}, output{};
void* table[1 + vtable_copy::kMaxEntries]{};
StreamlineBaseBinding route{};
bool bind_inside_input = false, linked = false;
HRESULT output_result = S_OK;
uint64_t receiver = 0, scope_native = 0, scope_queue = 0, scope_device = 0, finished_native = 0;
UINT interval = 0, flags = 0;
const DXGI_PRESENT_PARAMETERS* parameters = nullptr;
unsigned boundaries = 0, outputs = 0, scope_begins = 0, scope_ends = 0;
thread_local unsigned scope_depth = 0;
StreamlineBoundaryEvidence boundary{};
HRESULT STDMETHODCALLTYPE Present1(IDXGISwapChain1* self, UINT sync, UINT f, const DXGI_PRESENT_PARAMETERS* p) {
    receiver = reinterpret_cast<uint64_t>(self);
    linked = t_dedup.Contains(route.alias);
    interval = sync; flags = f; parameters = p;
    return receiver == route.base ? output_result : S_OK;
}
HRESULT STDMETHODCALLTYPE Present(IDXGISwapChain* self, UINT sync, UINT f) {
    const auto result = Present1(reinterpret_cast<IDXGISwapChain1*>(self), sync, f, nullptr);
    if (bind_inside_input && receiver == route.alias) {
        bind_inside_input = false;
        Check(VSync_BindOutputAlias(ReserveProxyForEvent(route.proxy), route), "first discovery installs pending B");
        const auto calls = EntryCount(NativeGateEntry::GateCall);
        std::thread output_while_pending([&] {
            Check(reinterpret_cast<PFN_Present1>(output.vptr[22])(reinterpret_cast<IDXGISwapChain1*>(&output), sync, f, nullptr) == S_OK &&
                  receiver == route.base && !linked && EntryCount(NativeGateEntry::GateCall) == calls &&
                  !boundaries && !outputs, "pending B forwards on a different thread without overlapping N's body");
        });
        output_while_pending.join();
        return reinterpret_cast<PFN_Present1>(output.vptr[22])(reinterpret_cast<IDXGISwapChain1*>(&output), sync, f, nullptr);
    }
    return result;
}
void TestOutputAlias() {
    for (size_t i = 0; i < kVtSlots; ++i) table[1 + i] = reinterpret_cast<void*>(&VtSlot);
    table[1] = reinterpret_cast<void*>(&VtQueryInterface);
    table[1 + 8] = reinterpret_cast<void*>(&Present);
    table[1 + 22] = reinterpret_cast<void*>(&Present1);
    input.vptr = output.vptr = table + 1;
    route = {reinterpret_cast<uint64_t>(&output), reinterpret_cast<uint64_t>(&input), 0x1234, 0x4321, 0x9876};
    s_native_handle_override = route.alias;
    VSync_OnNativeSelected(route.alias);
    const auto input_present = [&] { return reinterpret_cast<PFN_Present>(input.vptr[8])(
        reinterpret_cast<IDXGISwapChain*>(&input), 3, DXGI_PRESENT_ALLOW_TEARING); };
    const auto e = [](NativeGateEntry entry) { return EntryCount(entry); };
    const auto gate = NativeGateEntry::GateCall, pass = NativeGateEntry::AliasPassThrough;
    const uint64_t calls = e(gate), passes = e(pass), inner = e(NativeGateEntry::InnerPassThrough);
    const int saved_mode = s_vsync_mode.exchange(2); // N must not override its caller's interval on the carried path.
    const auto discovery = s_alias_ticket;
    StreamlineBase_NoteProxyPresent(route.proxy); // usual order: Streamline calls into ReShade, not vice versa
    {
        StreamlineInputScope above(route.proxy, 3, 0);
        Check(!StreamlineBase_Carries(route.alias) && !StreamlineBase_AliasOf(route.base) &&
              !StreamlineBase_NoteGamePresent(route.alias, route.device, 0x5678), "no published base: the event owns output");
    }
    StreamlineBase_NoteProxyPresent(route.proxy); // the host may defer the previous event's finish on DO_NOT_WAIT
    Check(s_alias_ticket == discovery, "an event beneath the proxy cannot establish an output alias");
    Check(input_present() == S_OK && linked && interval == 1 && flags == 0 && e(gate) == calls + 1 &&
          e(pass) == passes && s_last_timeline_native == route.alias && s_last_pacing_present.native_swapchain == route.alias,
          "ordinary N runs the original body and owns the timeline and physical row");
    Check(!StreamlineBase_NoteGamePresentDone(route.alias), "ordinary event finishes its scopes");

    auto& events = streamline_base_present::events;
    {
        StreamlineInputScope above(route.proxy, 3, 0);
        for (unsigned i = 0; i < 128; ++i) {
            StreamlineBase_NoteGamePresent(route.alias, route.device, route.queue);
            Check(events.depth == 1, "repeating an abandoned event stays at depth one, without needing a pending binding");
        }
        for (uint64_t i = 1; i <= 4; ++i) StreamlineBase_NoteGamePresent(i, 0, 0);
        Check(events.depth == 4 && events.Find(3) == events.depth && events.Find(4) == 3,
              "overflow replaces only the top entry");
        StreamlineBase_NoteGamePresentDone(1);
        Check(events.depth == 1 && events.Find(route.alias) == 0, "finishing a parent drops its unfinished children");
        StreamlineBase_NoteGamePresentDone(route.alias);
    }
    Check(!StreamlineBase_NoteGamePresent(route.base, route.device, route.queue) &&
          !StreamlineBase_NoteGamePresent(route.alias, route.device, route.queue) && events.depth == 2,
          "distinct nested events retain the outer while the selected inner discovers its binding");
    bind_inside_input = true;
    Check(input_present() == S_OK && receiver == route.base && linked && e(gate) == calls + 2 &&
          e(NativeGateEntry::InnerPassThrough) == inner && boundaries == 0 && outputs == 0 && !scope_begins,
          "pending B also forwards on the discovering thread without running a second native body");
    std::thread unmatched_finish([&] {
        Check(!StreamlineBase_NoteGamePresentDone(route.alias) && streamline_base_present::events.depth == 0 &&
              !StreamlineBase_Carries(route.alias), "a finish on a different thread cannot activate the pending pair");
    });
    unmatched_finish.join();
    Check(!StreamlineBase_Carries(route.alias) && !StreamlineBase_NoteGamePresentDone(route.alias) &&
          StreamlineBase_AliasOf(route.base) == route.alias, "only the matching finish publishes the pending binding");
    Check(events.depth == 1 && events.Find(route.base) == 0 && !StreamlineBase_NoteGamePresentDone(route.base) &&
          !events.depth, "publishing the inner pair preserved the outer event until its own finish");

    Check(StreamlineBase_NoteGamePresent(route.alias, route.device, 0x5678), "carried input event skips output scopes");
    const auto selected = s_selected_vtable_entries.load();
    const auto rows = s_pacing_present_records;
    Check(input_present() == S_OK && receiver == route.alias && !linked && interval == 3 && flags == DXGI_PRESENT_ALLOW_TEARING &&
          e(pass) == passes + 1 && e(gate) == calls + 2 && s_selected_vtable_entries.load() == selected &&
          s_pacing_present_records == rows, "carried N forwards unchanged without output evidence");
    DXGI_PRESENT_PARAMETERS p{};
    Check(reinterpret_cast<PFN_Present1>(input.vptr[22])(reinterpret_cast<IDXGISwapChain1*>(&input), 4, 8, &p) == S_OK &&
          !linked && interval == 4 && flags == 8 && parameters == &p && e(pass) == passes + 2 &&
          s_selected_vtable_entries.load() == selected, "carried N Present1 neither counts as B nor changes arguments");

    const auto begins = e(NativeGateEntry::BeginScope), ends = e(NativeGateEntry::EndScope);
    Check(reinterpret_cast<PFN_Present>(output.vptr[8])(reinterpret_cast<IDXGISwapChain*>(&output), 3, DXGI_PRESENT_ALLOW_TEARING) == S_OK &&
          receiver == route.base && linked && interval == 1 && !flags && e(gate) == calls + 3 && scope_begins == 1 && scope_ends == 1,
          "synchronous B Present owns one scoped body even on the input event's thread");
    DWORD worker_id = 0;
    std::thread worker([&] {
        worker_id = GetCurrentThreadId();
        Check(reinterpret_cast<PFN_Present1>(output.vptr[22])(reinterpret_cast<IDXGISwapChain1*>(&output), 3, 4, &p) == S_OK &&
              linked && receiver == route.base && parameters == &p, "worker B Present1 reaches the physical original");
    });
    worker.join();
    Check(e(gate) == calls + 4 && e(NativeGateEntry::BeginScope) == begins + 2 && e(NativeGateEntry::EndScope) == ends + 2 &&
          boundaries == 2 && outputs == 2 && scope_begins == 2 && scope_ends == 2 && finished_native == route.alias &&
          scope_native == route.alias && scope_queue == route.queue && scope_device == route.device &&
          boundary.swapchain == route.alias && boundary.queue == route.queue && boundary.device == route.device &&
          boundary.generation == 1 && boundary.queue_device == route.device &&
          boundary.stage == StreamlineBoundaryStage::Present && s_last_gate_native == route.alias &&
          s_last_timeline_native == route.alias && s_last_dxgi_receiver == route.base && s_last_pacing_present.native_swapchain == route.base &&
          s_last_pacing_present.thread_id == worker_id && s_pacing_present_records == rows + 2 &&
          s_selected_vtable_entries.load() == selected + 2, "one body per output; logical timeline/scopes, physical ETW row and thread");
    const auto output_present = [&](UINT f) { return reinterpret_cast<PFN_Present>(output.vptr[8])(
        reinterpret_cast<IDXGISwapChain*>(&output), 3, f); };
    const auto output_present1 = [&](UINT f) { return reinterpret_cast<PFN_Present1>(output.vptr[22])(
        reinterpret_cast<IDXGISwapChain1*>(&output), 3, f, &p); };
    vtable_copy::DedupNode outer;
    Check(t_dedup.Link(&outer, route.alias) && output_present(0) == S_OK &&
          e(NativeGateEntry::InnerPassThrough) == inner + 1 && e(gate) == calls + 4, "active B deduplicates under N");
    t_dedup.Unlink(&outer);
    Check(output_present(DXGI_PRESENT_TEST) == S_OK && e(gate) == calls + 5 && boundaries == 2 &&
          outputs == 2 && scope_begins == 2 && scope_ends == 2, "TEST runs the native body but neither event");
    output_result = DXGI_ERROR_WAS_STILL_DRAWING;
    Check(output_present(DXGI_PRESENT_DO_NOT_WAIT) == output_result && outputs == 3 && scope_begins == 3 &&
          scope_ends == 3 && !scope_depth && e(NativeGateEntry::EndScope) == ends + 3,
          "still-drawing releases both scopes on the calling thread even if there is never a retry");
    Check(output_present1(DXGI_PRESENT_DO_NOT_WAIT) == output_result && e(gate) == calls + 7 && outputs == 3 &&
          scope_begins == 3 && scope_ends == 3 && !scope_depth, "a repeated still-drawing attempt is a body with no event");
    output_result = S_OK;
    Check(output_present1(DXGI_PRESENT_TEST) == S_OK && scope_ends == 3 && outputs == 3, "TEST preserves retry suppression");
    std::thread retry([&] {
        Check(!scope_depth && output_present1(DXGI_PRESENT_DO_NOT_WAIT) == S_OK && !scope_depth,
              "a retry on another thread runs its body without acquiring or releasing a readiness scope");
    });
    retry.join();
    Check(e(gate) == calls + 9 && boundaries == 3 && outputs == 3 && scope_begins == 3 && scope_ends == 3 &&
          !scope_depth && e(NativeGateEntry::BeginScope) == begins + 3 && e(NativeGateEntry::EndScope) == ends + 3,
          "retry does not count another output or take ownership of any scope");
    output_result = DXGI_ERROR_DEVICE_REMOVED;
    Check(output_present(DXGI_PRESENT_DO_NOT_WAIT) == output_result && outputs == 4 && !scope_depth &&
          scope_begins == 4 && scope_ends == 4 && e(NativeGateEntry::EndScope) == ends + 4,
          "the next event after a successful retry closes its own scopes even on device loss");
    output_result = S_OK;
    Check(StreamlineBase_NoteGamePresentDone(route.alias), "carried input event still skips its finish");

    VSync_NoteFinishedPresent(route.alias); // consumes B's evidence, not N's
    void** const old_copy = output.vptr;
    for (unsigned i = 0; i < vtable_copy::FallbackCounter::kScopes; ++i) {
        Check(input_present() == S_OK && !linked && StreamlineBase_Carries(route.alias), "input shells cannot provide output evidence");
        VSync_NoteFinishedPresent(route.alias);
    }
    Check(!StreamlineBase_Carries(route.alias) && !s_fallback.Fired() && s_fallback.Count() == 0 &&
          output.vptr == old_copy && !FindBinding(route.base) && input_present() == S_OK && linked &&
          s_last_pacing_present.native_swapchain == route.alias, "32 missing outputs retire B, not the inline one-shot");
    auto ticket = VSync_BeginOutputAlias(route.alias, route.proxy, s_next_probe_ms);
    Check(!VSync_BindOutputAlias(ticket, route), "the same missed base is refused");

    const auto bind = [](const StreamlineBaseBinding& b) {
        const auto t = VSync_BeginOutputAlias(b.alias, b.proxy, s_next_probe_ms);
        Check(VSync_BindOutputAlias(t, b), "install the new pending binding");
        VSync_ActivateOutputAlias(t, b.alias);
        Check(StreamlineBase_AliasOf(b.base) == b.alias, "publish the exact installed binding");
    };
    VtObject replacement{table + 1};
    auto next = route; next.base = reinterpret_cast<uint64_t>(&replacement);
    bind(next); // a different base under the same proxy clears refusal
    auto other_proxy = route; ++other_proxy.proxy;
    bind(other_proxy);
    Check(replacement.vptr != table + 1 && output.vptr == old_copy && !FindBinding(next.base),
          "proxy replacement abandons the old copy and adopts B's surviving one");
    void* const page = VirtualAlloc(nullptr, sizeof(VtObject), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    Check(page != nullptr, "allocate an observable stale receiver");
    auto guard_route = route;
    guard_route.proxy += 2;
    guard_route.base = reinterpret_cast<uint64_t>(new(page) VtObject{table + 1});
    const auto protect = [&](DWORD protection) {
        DWORD old = 0;
        Check(VirtualProtect(page, sizeof(VtObject), protection, &old) != FALSE, "protect the simulated stale B");
    };
    const auto untouched = [&] {
        MEMORY_BASIC_INFORMATION m{};
        return VirtualQuery(page, &m, sizeof(m)) == sizeof(m) && (m.Protect & PAGE_GUARD);
    };
    bind(guard_route);
    protect(PAGE_READWRITE | PAGE_GUARD);
    StreamlineBase_OnDestroyed(route.alias);
    Check(!StreamlineBase_Carries(route.alias) && output.vptr == old_copy && !FindBinding(route.base),
          "destroy never dereferences B; its surviving thunk is now an unselected forward");
    Check(untouched(), "destruction did not even read the unowned base vptr");
    protect(PAGE_READWRITE);
    const auto gate_after_destroy = e(gate);
    Check(output_present(0) == S_OK && e(gate) == gate_after_destroy && !linked, "abandoned copy is safe to call");
    ticket = VSync_BeginOutputAlias(route.alias, route.proxy, s_next_probe_ms);
    Check(VSync_BindOutputAlias(ticket, route), "stage binding before destruction");
    VSync_OnNativeDestroyed(route.alias);
    VSync_ActivateOutputAlias(ticket, route.alias);
    Check(!StreamlineBase_Carries(route.alias) && !VSync_BeginOutputAlias(route.alias, route.proxy, s_next_probe_ms),
          "late activation and resolution cannot resurrect the dead selection");
    VSync_OnNativeSelected(route.alias);
    StreamlineBase_NoteGamePresent(route.alias, route.device, route.queue);
    Check(VSync_BindOutputAlias(ReserveProxyForEvent(route.proxy), route), "P1 stages B1 on its own event");
    s_next_probe_ms = UINT64_MAX; // freeze the same-proxy probe interval; replacement cannot depend on wall time
    auto changed = next;
    ++changed.proxy;
    std::thread new_proxy([&] {
        StreamlineBase_NoteGamePresent(changed.alias, changed.device, changed.queue);
        Check(!ReserveProxyForEvent(route.proxy), "P1 does not supersede itself during its pending probe interval");
        auto t = ReserveProxyForEvent(changed.proxy);
        Check(t && !FindBinding(route.base) && VSync_BindOutputAlias(t, changed), "P2 immediately supersedes pending P1");
        StreamlineBase_NoteGamePresentDone(changed.alias);
    });
    new_proxy.join();
    StreamlineBase_NoteGamePresentDone(route.alias);
    Check(!StreamlineBase_AliasOf(route.base) && StreamlineBase_AliasOf(changed.base) == changed.alias,
          "P1's late finish cannot publish B1 or retire B2");
    StreamlineBase_OnDestroyed(route.alias);
    StreamlineBase_NoteGamePresent(route.base, route.device, route.queue);
    StreamlineBase_NoteGamePresent(route.alias, route.device, route.queue);
    const auto abandoned = ReserveProxyForEvent(route.proxy);
    Check(VSync_BindOutputAlias(abandoned, route), "stage a nested event that is abandoned by its outer finish");
    StreamlineBase_NoteGamePresentDone(route.base);
    VSync_ActivateOutputAlias(abandoned, route.alias);
    Check(!events.depth && !FindBinding(route.base) && !StreamlineBase_Carries(route.alias),
          "out-of-order finish drops, rather than publishes, the unfinished child's reservation");
    Check(!StreamlineBase_NoteGamePresent(route.alias, route.device, route.queue) &&
          VSync_BindOutputAlias(ReserveProxyForEvent(route.proxy), route), "stage an event which misses its finish");
    Check(!StreamlineBase_NoteGamePresent(route.alias, route.device, route.queue) && !StreamlineBase_Carries(route.alias) &&
          VSync_BindOutputAlias(ReserveProxyForEvent(next.proxy), next) && !StreamlineBase_NoteGamePresentDone(next.alias) &&
          StreamlineBase_AliasOf(next.base) == next.alias, "a later event resolves afresh and publishes on its own finish");
    bind(guard_route);
    protect(PAGE_READWRITE | PAGE_GUARD);
    VSync_RestoreVtables();
    Check(input.vptr == table + 1 && replacement.vptr != table + 1 && output.vptr == old_copy &&
          !StreamlineBase_Carries(next.alias), "common teardown restores N but leaves every B untouched");
    Check(untouched(), "teardown did not even read the unowned base vptr");
    VirtualFree(page, 0, MEM_RELEASE);
    s_native_handle_override = 0;
    s_vsync_mode.store(saved_mode);
}
} // namespace output_test
#endif

// The optional description query can fault after the gate/readiness scopes opened.
// It must neither abort the retry nor unwind the Present detour's owners.
unsigned s_desc_queries = 0;
HRESULT STDMETHODCALLTYPE RejectTearing(IDXGISwapChain*, UINT, UINT flags) {
    return flags & DXGI_PRESENT_ALLOW_TEARING ? DXGI_ERROR_INVALID_CALL : S_OK;
}
HRESULT STDMETHODCALLTYPE RaisingGetDesc(void*, DXGI_SWAP_CHAIN_DESC*) {
    ++s_desc_queries;
    RaiseException(0xE0000042u, 0, 0, nullptr);
    return 0;
}
HRESULT STDMETHODCALLTYPE RaisingPresent(IDXGISwapChain*, UINT, UINT) {
    RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    return E_FAIL;
}
HRESULT STDMETHODCALLTYPE RaisingPresent1(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*) {
    RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    return E_FAIL;
}
bool CallOutermostCatching(IDXGISwapChain* self) {
    __try {
        // Independently exercise the dedup termination handler. Raise at selection,
        // before the body constructs the native gate or readiness objects.
        PresentOutermost(RejectTearing, false, 0, nullptr, self, reinterpret_cast<uint64_t>(self), 3, 4);
        return false;
    } __except (GetExceptionCode() == 0xE0000042u ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return true;
    }
}
void TestDedupSehUnwind() {
    static void* storage[1 + kVtSlots];
    for (size_t i = 0; i < kVtSlots; ++i) storage[1 + i] = reinterpret_cast<void*>(&VtSlot);
    storage[1 + 12] = reinterpret_cast<void*>(&RaisingGetDesc); // IDXGISwapChain::GetDesc
    struct SehObject { void** vptr; };
    static SehObject object;
    object.vptr = storage + 1;
    auto* self = reinterpret_cast<IDXGISwapChain*>(&object);
    const auto saved_original = s_present_targets[0].original.load();
    const int saved_mode = s_vsync_mode.load();
    s_native_handle_override = reinterpret_cast<uint64_t>(&object); // the body runs: this is the selected swapchain
    s_vsync_mode.store(1);                                           // VSync off: ALLOW_TEARING is added ...
    s_present_targets[0].original = RejectTearing;                    // ... and rejected: the retry path calls GetDesc
    Check(s_present_thunks[0](self, 3, 4) == S_OK && s_desc_queries == 1,
          "a fault in the optional description query does not suppress the no-tearing retry");
    Check(s_callback_depth == 0 && s_readiness_depth == 0 && t_dedup.Depth() == 0,
          "the retry returned through all Present owners");
    s_raise_at_selection = true;
    Check(CallOutermostCatching(self), "a body exception still reaches the caller's handler");
    s_raise_at_selection = false;
    Check(!t_dedup.Contains(reinterpret_cast<uintptr_t>(&object)) && t_dedup.Depth() == 0,
          "the dedup node was unlinked during the unwind: no dead frame left in the thread's list");
    s_vsync_mode.store(0);
    s_present_targets[0].original = RaisingPresent;
    const auto saved_present1 = s_present1_targets[0].original.exchange(RaisingPresent1);
    Check(s_present_thunks[0](self, 0, 0) == DXGI_ERROR_INVALID_CALL &&
          s_present1_thunks[0](reinterpret_cast<IDXGISwapChain1*>(self), 0, 0, nullptr) == DXGI_ERROR_INVALID_CALL,
          "both foreign Present faults return the existing invalid-call HRESULT");
    Check(s_callback_depth == 0 && s_readiness_depth == 0 && t_dedup.Depth() == 0,
          "neither original-call exception skips Present cleanup");
    s_present1_targets[0].original = saved_present1;
    s_present_targets[0].original = InlineProbeOriginal;
    const uint64_t inner_before = EntryCount(NativeGateEntry::InnerPassThrough);
    Check(s_present_thunks[0](self, 3, 4) == 7 && EntryCount(NativeGateEntry::InnerPassThrough) == inner_before,
          "the next detour for the same object is an outermost entry, not a pass-through");
    s_native_handle_override = 0;
    s_vsync_mode.store(saved_mode);
    s_present_targets[0].original = saved_original;
}

// Teardown: the DXGI Present / Present1 inline patches are never disabled; every other hook is.
// The FF 25 jump of a hook chain decodes per architecture: x64 rip-relative to an 8-byte target, Win32 an absolute
// address of a 4-byte target (the diagnostics' DecodeJump picks by pointer size; both forms are tested here).
void TestDecodeJumpPerArchitecture() {
    alignas(8) uint8_t block[64]{};
    const uint64_t target64 = 0x1122334455667788ull;
    std::memcpy(block + 32, &target64, 8);
    uint8_t code[16]{0xFF, 0x25};
    const int32_t disp = 32 - 6; // rip-relative to the next instruction (jump at block[0], 6 bytes long)
    std::memcpy(code + 2, &disp, 4);
    uint64_t next = 0;
    Check(DecodeJumpFor(true, code, reinterpret_cast<uint64_t>(block), &next) && next == target64,
          "x64 FF 25 is rip-relative to an 8-byte target");

    // Relative targets wrap to 32 bits on Win32 (address + length + displacement mod 2^32), not on x64.
    uint8_t wrap_e9[16]{0xE9, 0x00, 0x00, 0x00, 0x10}; // +0x10000000
    Check(DecodeJumpFor(false, wrap_e9, 0xF8000000ull, &next) && next == 0x08000005ull &&
          DecodeJumpFor(true, wrap_e9, 0xF8000000ull, &next) && next == 0x108000005ull,
          "E9 wraps to 32 bits on Win32 only");
    uint8_t wrap_eb[16]{0xEB, 0x7F};
    Check(DecodeJumpFor(false, wrap_eb, 0xFFFFFFF0ull, &next) && next == 0x71ull &&
          DecodeJumpFor(true, wrap_eb, 0xFFFFFFF0ull, &next) && next == 0x100000071ull,
          "EB wraps to 32 bits on Win32 only");
    uint8_t back_eb[16]{0xEB, 0x80}; // -128 from 0x10 + 2
    Check(DecodeJumpFor(false, back_eb, 0x10ull, &next) && next == 0xFFFFFF92ull &&
          DecodeJumpFor(true, back_eb, 0x10ull, &next) && next == 0xFFFFFFFFFFFFFF92ull,
          "a backward EB below zero wraps to 32 bits on Win32 only");
    uint8_t back_e9[16]{0xE9, 0xF0, 0xFF, 0xFF, 0xFF}; // -16 from 0x100 + 5
    Check(DecodeJumpFor(false, back_e9, 0x100ull, &next) && next == 0xF5ull &&
          DecodeJumpFor(true, back_e9, 0x100ull, &next) && next == 0xF5ull, "a backward E9 inside the space is alike");

    // Win32 form: the operand is the ABSOLUTE address of a 4-byte cell (below 4 GB).
    unsigned char* low = static_cast<unsigned char*>(VirtualAlloc(
        sizeof(void*) == 4 ? nullptr : reinterpret_cast<void*>(0x30000000), 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (low && reinterpret_cast<uintptr_t>(low) <= 0xFFFFFFFFull - 64) {
        const uint32_t cell_target = 0x00ABCDEFu;
        std::memcpy(low + 16, &cell_target, 4);
        uint8_t code32[16]{0xFF, 0x25};
        const uint32_t cell_address = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(low + 16));
        std::memcpy(code32 + 2, &cell_address, 4);
        next = 0;
        Check(DecodeJumpFor(false, code32, 0x1234, &next) && next == cell_target,
              "Win32 FF 25 is an absolute address of a 4-byte target, not rip-relative");
        // The 64-bit-only forms are not decoded as Win32 jumps.
        uint8_t mov_rax[16]{0x48, 0xB8, 1, 2, 3, 4, 5, 6, 7, 8, 0xFF, 0xE0};
        Check(!DecodeJumpFor(false, mov_rax, 0x1234, &next) && DecodeJumpFor(true, mov_rax, 0x1234, &next),
              "mov rax + jmp rax is an x64 form only");
        // E9 is the same on both.
        uint8_t rel[16]{0xE9, 0x10, 0, 0, 0};
        Check(DecodeJumpFor(false, rel, 0x1000, &next) && next == 0x1015 && DecodeJumpFor(true, rel, 0x1000, &next) && next == 0x1015,
              "E9 rel32 decodes alike");
    }
}

void TestTeardownLeavesDxgiHooksEnabled() {
    // Installer admission is closed before the list is collected: a typed hook cannot be added after it.
    Hooks_BeginShutdown();
    bool already = false;
    Check(InstallTypedHook(s_present_targets, reinterpret_cast<void*>(&VtSlot), s_present_thunks, &already) == MH_ERROR_DISABLED,
          "after the admission closes no typed hook can be installed, so the collected list is complete");
    void* keep[32];
    const size_t kept = VSync_InlineTargetsLeftEnabled(keep, 32);
    size_t typed = 0;
    for (const auto& entry : s_present_targets) if (entry.target.load() && entry.enabled) ++typed;
    for (const auto& entry : s_present1_targets) if (entry.target.load() && entry.enabled) ++typed;
    Check(kept == typed && kept >= 2, "every enabled typed DXGI target is listed to stay enabled");
    Check(DisableAllHooksExcept(keep, kept) == MH_OK, "disable except the DXGI targets");
    bool others_disabled = true, others_exist = false;
    size_t typed_enabled = 0;
    for (const FakeHook& hook : s_fake_hooks) {
        if (!hook.target) continue;
        bool is_typed = false;
        for (size_t i = 0; i < kept; ++i) is_typed = is_typed || keep[i] == hook.target;
        if (is_typed) { if (hook.enabled) ++typed_enabled; }
        else { others_exist = true; if (hook.enabled) others_disabled = false; }
    }
    Check(typed_enabled == kept, "the DXGI Present / Present1 inline patches are not disabled (no bytes written back)");
    Check(others_exist && others_disabled, "every other hook is disabled as before");
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
bool Log_IsEnabled(LogLevel) noexcept { return true; }
void Log_Write(LogLevel, const char* format, ...) {
      va_list arguments;
      va_start(arguments, format);
      vsnprintf(s_last_test_log, sizeof(s_last_test_log), format, arguments);
      va_end(arguments);
}
RuntimeCallbackGuard::RuntimeCallbackGuard() noexcept : stopping_(s_test_stopping) { ++s_callback_depth; }
RuntimeCallbackGuard::~RuntimeCallbackGuard() { --s_callback_depth; }
bool RuntimeLifecycle_IsStopping() noexcept { return s_test_stopping; }
HANDLE RuntimeLifecycle_GetStopEvent() noexcept { return nullptr; }
bool BackgroundTasks_Start(BackgroundTaskProc, void*) noexcept {
      ++s_background_starts;
      return false;
}
#ifdef _WIN64
bool HookStreamlinePCL(HMODULE) { ++s_synchronous_installs; return false; }
std::atomic<bool> g_fg_presenting{false};
void NGXHooks_TryInstall() { ++s_synchronous_installs; }
#endif
bool FrameTimeline_BeginPresentCall(
      uint64_t native, PresentTimelineEntryPoint, PresentTimelineCall*, bool runtime) noexcept {
      s_last_timeline_native = native;
      return s_capture_native || runtime;
}
bool FrameTimeline_BeginPresentAttempt(
      const PresentTimelineCall&, uint32_t, PresentTimelineSpan* span, bool runtime) noexcept {
      span->capture_epoch=s_capture_native?1:0;return s_capture_native || runtime;
}
uint64_t FrameTimeline_GetPresentSwapchainGeneration(uint64_t) noexcept { return 1; }
uint64_t FrameTimeline_PresentGenerationLockFailures() noexcept { return 0; }
uint64_t SwapMgr_GetNativeHandle() {
      if (s_raise_at_selection) RaiseException(0xE0000042u, 0, 0, nullptr);
      return s_native_handle_override ? s_native_handle_override : 0x1000;
}
NativeGateContext NativeGate_GetContext(int64_t now) noexcept { auto c = s_native_context; c.now_qpc = now; return c; }
void NativeGate_Execute(NativeGateCall* call, uint64_t native) noexcept {
      s_last_gate_native = native;
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
extern "C" const char* WINAPI MH_StatusToString(MH_STATUS) { return "stub"; }
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
extern "C" MH_STATUS WINAPI MH_Initialize(VOID) { return MH_OK; }
extern "C" MH_STATUS WINAPI MH_QueueDisableHook(void* target) {
    std::lock_guard<std::mutex> lock(s_fake_mutex);
    for (FakeHook& hook : s_fake_hooks)
        if (!target || hook.target == target) hook.queued_enable = false;
    return MH_OK;
}
extern "C" MH_STATUS WINAPI MH_QueueEnableHook(void* target) {
    std::lock_guard<std::mutex> lock(s_fake_mutex);
    for (FakeHook& hook : s_fake_hooks)
        if (!target || hook.target == target) hook.queued_enable = true;
    return MH_OK;
}
extern "C" MH_STATUS WINAPI MH_ApplyQueued(VOID) {
    std::lock_guard<std::mutex> lock(s_fake_mutex);
    for (FakeHook& hook : s_fake_hooks)
        if (hook.target) hook.enabled = hook.queued_enable;
    return MH_OK;
}

int main() {
    // The copy at a selection is the path most of these tests drive, so the key starts off here; the default (the
    // copy at the first present event) has its own block in TestVtableCopyEntry.
    g_config.vtable_copy_at_present = false;
    PacingPresentAttribution previous{};
    previous.key = {PacingIdentityDomain::NvApiFrame, 1, 1};
    previous.marker_qpc = 1;
    PacingTelemetry_SetPresentAttribution(previous);
    PushPacingPresent(0x1000, {}, {}, false);
    Check(s_pacing_present_records == 0 && !PacingFrameKey_IsValid(t_pacing_present_attribution.key),
        "without consumers a Present clears probe attribution but publishes no record");
    g_pacing_telemetry_demand.store(kPacingRouteEvidence);
    PushPacingPresent(0x1000, {}, {}, false);
    Check(s_pacing_present_records == 1, "native PCL alone still receives Present candidates");
    g_pacing_telemetry_demand.store(kPacingReadout);
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
#ifdef _WIN64
      TestNativeQueueOverflow();
      output_test::TestOutputAlias(); // verify loss of B leaves the ordinary inline one-shot for the next test
#endif
      TestVtableCopyEntry();
      TestVtableForwardsLiveClassSlot();
      TestLoaderNotifications();
      TestDecodeJumpPerArchitecture();
      TestTeardownLeavesDxgiHooksEnabled();
      TestDedupSehUnwind();
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
      ++s_readiness_depth;
      s_readiness_span = span;
      if (!span) return;
      Check(span->p_qpc==0 && span->e_qpc==0, "signal instrumentation outside native P/E");
      s_readiness_qpc=CapturePresentQpc();
      Check(before_gate==s_early_readiness,"pre-hold submission only on requested route");
      Check(s_readiness_qpc>=span->native_gate.native_release_qpc, "signal follows existing gate hold");
      ++s_readiness_entries;
}
GpuReadinessCall::~GpuReadinessCall() { --s_readiness_depth; }
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
void StreamlineDiscovery_RecordBoundary(const StreamlineBoundaryEvidence& value) noexcept {
      ++output_test::boundaries; output_test::boundary = value;
}
void GpuReadiness_BeginScope(uint64_t native, uint64_t queue, uint64_t device) noexcept {
      Check(!output_test::scope_depth, "no previous B readiness scope was left on this thread");
      ++output_test::scope_depth;
      ++output_test::scope_begins; output_test::scope_native = native; output_test::scope_queue = queue; output_test::scope_device = device;
}
void GpuReadiness_EndScope(uint64_t native) noexcept {
      Check(output_test::scope_depth == 1, "readiness scope is closed by its opening thread");
      --output_test::scope_depth; ++output_test::scope_ends; output_test::finished_native = native;
}
void EnfDisp_NoteOutputPresent(int64_t) noexcept { ++output_test::outputs; }
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
void PacingTelemetry_RecordDxgiPresent(IDXGISwapChain* receiver, const PacingPresentRecord&) noexcept {
      s_last_dxgi_receiver = reinterpret_cast<uint64_t>(receiver);
}
int64_t GetQPCFrequency() { return 10'000'000; }