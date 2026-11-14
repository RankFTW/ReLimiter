// Fake D3D12 queue / fence / device, the fake GPU thread, the stubs of the glue's externals and the Game model of
// the glue test and bench. See gpu_queue_glue_support.h.

#include "gpu_queue_glue_support.h"

#include "gpu_queue_law_feed.h"
#include "gpu_queue_probe.h"
#include "gpu_queue_probe_test_seam.h"
#include "gpu_readiness_d3d12_queue.h"
#include "hooks.h"
#include "instrumentation_state.h"
#include "logger.h"
#include "startup_breadcrumb.h"
#include "pacing_telemetry.h"
#include "runtime_lifecycle.h"
#include "streamline_gpu_handoff.h"
#include "streamline_queue_hooks.h"

#include <timeapi.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace glue {

thread_local ThreadCounters g_thread;

namespace {

std::atomic<int64_t> g_latency_ticks{0};
std::atomic<bool> g_gpu_stop{false};
HANDLE g_gpu_thread = nullptr;
constexpr size_t kMaxQueues = 16;
Queue* g_queues[kMaxQueues] = {};
std::atomic<size_t> g_queue_count{0};

int64_t QpcNow() noexcept {
    LARGE_INTEGER q{};
    QueryPerformanceCounter(&q);
    return q.QuadPart;
}
bool Same(REFIID a, REFIID b) noexcept { return IsEqualGUID(a, b) != FALSE; }

// ── COM methods (x64: one calling convention; the vtables below put them at the SDK's slots) ──
HRESULT STDMETHODCALLTYPE Unimplemented(void*) noexcept { return E_NOTIMPL; }
template <class T> ULONG STDMETHODCALLTYPE AddRefOf(T* self) noexcept { return static_cast<ULONG>(++self->refs); }
template <class T> ULONG STDMETHODCALLTYPE ReleaseOf(T* self) noexcept { return static_cast<ULONG>(--self->refs); }

HRESULT STDMETHODCALLTYPE FenceQueryInterface(Fence* self, REFIID riid, void** out) noexcept {
    if (!out) return E_POINTER;
    if (Same(riid, __uuidof(IUnknown)) || Same(riid, __uuidof(ID3D12Object)) || Same(riid, __uuidof(ID3D12DeviceChild)) ||
        Same(riid, __uuidof(ID3D12Pageable)) || Same(riid, __uuidof(ID3D12Fence))) {
        ++self->refs;
        *out = self;
        return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
}
HRESULT STDMETHODCALLTYPE FenceGetDevice(Fence* self, REFIID, void** out) noexcept {
    if (!out) return E_POINTER;
    ++self->device->refs;
    *out = self->device;
    return S_OK;
}
UINT64 STDMETHODCALLTYPE FenceGetCompletedValue(Fence* self) noexcept {
    ++g_thread.fence_reads;
    self->reads.fetch_add(1, std::memory_order_relaxed);
    return self->completed.load(std::memory_order_acquire);
}
HRESULT STDMETHODCALLTYPE FenceSetEventOnCompletion(Fence* self, UINT64 value, HANDLE event) noexcept {
    self->registrations.fetch_add(1, std::memory_order_relaxed);
    std::unique_lock<std::mutex> guard(self->lock);
    if (self->completed.load(std::memory_order_acquire) >= value) {
        guard.unlock();
        if (event) SetEvent(event); // an already reached value signals the event at once, as D3D12 does
        return S_OK;
    }
    if (self->registered.size() >= 256) self->registered.erase(self->registered.begin());
    self->registered.push_back({value, event});
    return S_OK;
}
HRESULT STDMETHODCALLTYPE FenceSignal(Fence* self, UINT64 value) noexcept {
    ++g_thread.cpu_signals;
    self->cpu_signals.fetch_add(1, std::memory_order_relaxed);
    self->Advance(value);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE QueueQueryInterface(Queue* self, REFIID riid, void** out) noexcept {
    if (!out) return E_POINTER;
    if (Same(riid, __uuidof(IUnknown)) || Same(riid, __uuidof(ID3D12Object)) || Same(riid, __uuidof(ID3D12DeviceChild)) ||
        Same(riid, __uuidof(ID3D12Pageable)) || Same(riid, __uuidof(ID3D12CommandQueue))) {
        ++self->refs;
        *out = self;
        return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
}
HRESULT STDMETHODCALLTYPE QueueGetDevice(Queue* self, REFIID, void** out) noexcept {
    if (!out) return E_POINTER;
    ++self->device->refs;
    *out = self->device;
    return S_OK;
}
void STDMETHODCALLTYPE QueueExecute(Queue* self, UINT count, ID3D12CommandList* const*) noexcept {
    ++g_thread.executes;
    self->executes.fetch_add(1, std::memory_order_relaxed);
    self->lists.fetch_add(count, std::memory_order_relaxed);
}
HRESULT STDMETHODCALLTYPE QueueSignal(Queue* self, Fence* fence, UINT64 value) noexcept {
    if (HANDLE gate = self->signal_gate.load(std::memory_order_acquire)) {
        if (self->signal_gate_fence.load(std::memory_order_acquire) == fence) {
            self->signals_blocked.fetch_add(1, std::memory_order_acq_rel);
            WaitForSingleObject(gate, 30000);
            self->signals_blocked.fetch_sub(1, std::memory_order_acq_rel);
        }
    }
    ++g_thread.queue_signals;
    self->signals.fetch_add(1, std::memory_order_relaxed);
    fence->queue_signals.fetch_add(1, std::memory_order_relaxed);
    self->Push(fence, value, false);
    return S_OK;
}
HRESULT STDMETHODCALLTYPE QueueWait(Queue* self, Fence* fence, UINT64 value) noexcept {
    ++g_thread.queue_waits;
    if (HANDLE gate = self->wait_gate.load(std::memory_order_acquire)) {
        self->waits_blocked.fetch_add(1, std::memory_order_acq_rel);
        WaitForSingleObject(gate, 30000);
        self->waits_blocked.fetch_sub(1, std::memory_order_acq_rel);
    }
    self->waits.fetch_add(1, std::memory_order_relaxed);
    self->Push(fence, value, true);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceQueryInterface(Device* self, REFIID riid, void** out) noexcept {
    if (!out) return E_POINTER;
    if (Same(riid, __uuidof(IUnknown)) || Same(riid, __uuidof(ID3D12Object)) || Same(riid, __uuidof(ID3D12Device))) {
        ++self->refs;
        *out = self;
        return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
}

Fence* NewFence(Device* device, const char* name, uint64_t initial, bool from_com) {
    auto* fence = new Fence;
    fence->device = device;
    fence->name = name;
    fence->from_com = from_com;
    fence->completed.store(initial, std::memory_order_release);
    // vtbl is set by the caller's table accessor below (declared after the tables)
    return fence;
}

void* const* FenceVtable() {
    static void* table[11];
    static const bool built = [] {
        for (auto& entry : table) entry = reinterpret_cast<void*>(&Unimplemented);
        table[0] = reinterpret_cast<void*>(&FenceQueryInterface);
        table[1] = reinterpret_cast<void*>(&AddRefOf<Fence>);
        table[2] = reinterpret_cast<void*>(&ReleaseOf<Fence>);
        table[7] = reinterpret_cast<void*>(&FenceGetDevice);
        table[8] = reinterpret_cast<void*>(&FenceGetCompletedValue);
        table[9] = reinterpret_cast<void*>(&FenceSetEventOnCompletion);
        table[10] = reinterpret_cast<void*>(&FenceSignal);
        return true;
    }();
    (void)built;
    return table;
}
void* const* QueueVtable() {
    static void* table[19];
    static const bool built = [] {
        for (auto& entry : table) entry = reinterpret_cast<void*>(&Unimplemented);
        table[0] = reinterpret_cast<void*>(&QueueQueryInterface);
        table[1] = reinterpret_cast<void*>(&AddRefOf<Queue>);
        table[2] = reinterpret_cast<void*>(&ReleaseOf<Queue>);
        table[7] = reinterpret_cast<void*>(&QueueGetDevice);
        table[10] = reinterpret_cast<void*>(&QueueExecute);
        table[14] = reinterpret_cast<void*>(&QueueSignal);
        table[15] = reinterpret_cast<void*>(&QueueWait);
        return true;
    }();
    (void)built;
    return table;
}

HRESULT STDMETHODCALLTYPE DeviceCreateFence(Device* self, UINT64 initial, D3D12_FENCE_FLAGS, REFIID riid, void** out) noexcept {
    (void)riid; // the probe asks for ID3D12Fence
    self->create_fence_calls.fetch_add(1, std::memory_order_relaxed);
    if (!out) return S_FALSE;
    Fence* fence = NewFence(self, "probe fence", initial, true);
    fence->vtbl = FenceVtable();
    {
        std::lock_guard<std::mutex> guard(self->lock);
        self->fences.push_back(fence);
    }
    *out = fence; // one reference, the caller's
    return S_OK;
}

void* const* DeviceVtable() {
    static void* table[64];
    static const bool built = [] {
        for (auto& entry : table) entry = reinterpret_cast<void*>(&Unimplemented);
        table[0] = reinterpret_cast<void*>(&DeviceQueryInterface);
        table[1] = reinterpret_cast<void*>(&AddRefOf<Device>);
        table[2] = reinterpret_cast<void*>(&ReleaseOf<Device>);
        table[36] = reinterpret_cast<void*>(&DeviceCreateFence);
        return true;
    }();
    (void)built;
    return table;
}

// ── The fake GPU ──
DWORD WINAPI GpuMain(void*) {
    unsigned idle = 0;
    while (!g_gpu_stop.load(std::memory_order_acquire)) {
        const int64_t latency = g_latency_ticks.load(std::memory_order_relaxed);
        const int64_t now = latency ? QpcNow() : 0;
        bool did = false;
        const size_t count = g_queue_count.load(std::memory_order_acquire);
        for (size_t i = 0; i < count; ++i) did = g_queues[i]->Process(now) || did;
        if (did) idle = 0;
        else if (++idle >= 32) { idle = 0; SwitchToThread(); }
        else YieldProcessor();
    }
    return 0;
}

// ── What the stubs capture ──
std::mutex g_log_lock;
std::vector<std::string> g_log;
std::mutex g_records_lock;
std::vector<PacingGpuQueueRecord> g_records;
bool g_log_echo = GetEnvironmentVariableA("GLUE_LOG", nullptr, 0) > 0;

} // namespace

void Fence::Advance(uint64_t value) {
    std::lock_guard<std::mutex> guard(lock);
    completed.store(value, std::memory_order_release);
    for (size_t i = 0; i < registered.size();) {
        if (registered[i].value <= value) {
            if (registered[i].event) SetEvent(registered[i].event);
            registered[i] = registered.back();
            registered.pop_back();
        } else {
            ++i;
        }
    }
}

void Queue::Push(Fence* fence, uint64_t value, bool wait) noexcept {
    const int64_t latency = g_latency_ticks.load(std::memory_order_relaxed);
    const int64_t ready = latency ? QpcNow() + latency : 0;
    for (;;) {
        while (lock_.exchange(true, std::memory_order_acquire)) YieldProcessor();
        if (tail_ - head_ < kRing) {
            Op& op = ring[tail_ % kRing];
            op.fence = fence;
            op.value = value;
            op.ready = ready;
            op.wait = wait;
            ++tail_;
            lock_.store(false, std::memory_order_release);
            return;
        }
        lock_.store(false, std::memory_order_release); // full: the GPU thread has to catch up
        SwitchToThread();
    }
}

bool Queue::Process(int64_t now) noexcept {
    bool did = false;
    while (lock_.exchange(true, std::memory_order_acquire)) YieldProcessor();
    while (head_ != tail_) {
        Op& op = ring[head_ % kRing];
        if (op.ready > now) break;
        if (op.wait) {
            if (op.fence->Completed() < op.value) break; // the queue is blocked on this Wait
        } else {
            op.fence->Advance(op.value);
        }
        ++head_;
        did = true;
    }
    lock_.store(false, std::memory_order_release);
    return did;
}

Device* CreateDevice() {
    auto* device = new Device;
    device->vtbl = DeviceVtable();
    return device;
}

Queue* CreateQueue(Device* device, const char* name) {
    auto* queue = new Queue;
    queue->vtbl = QueueVtable();
    queue->device = device;
    queue->name = name;
    const size_t index = g_queue_count.load(std::memory_order_relaxed);
    if (index >= kMaxQueues) { std::fprintf(stderr, "too many fake queues\n"); std::_Exit(2); }
    g_queues[index] = queue;
    g_queue_count.store(index + 1, std::memory_order_release);
    return queue;
}

Fence* CreateGameFence(Device* device, const char* name) {
    Fence* fence = NewFence(device, name, 0, false);
    fence->vtbl = FenceVtable();
    std::lock_guard<std::mutex> guard(device->lock);
    device->fences.push_back(fence);
    return fence;
}

std::array<uint64_t, 3> NativeMethods(Queue* queue) {
    return {reinterpret_cast<uint64_t>(queue->vtbl[10]), reinterpret_cast<uint64_t>(queue->vtbl[14]),
        reinterpret_cast<uint64_t>(queue->vtbl[15])};
}

void StartGpu() {
    if (g_gpu_thread) return;
    g_gpu_stop.store(false, std::memory_order_release);
    g_gpu_thread = CreateThread(nullptr, 0, GpuMain, nullptr, 0, nullptr);
}
void StopGpu() {
    if (!g_gpu_thread) return;
    g_gpu_stop.store(true, std::memory_order_release);
    WaitForSingleObject(g_gpu_thread, 5000);
    CloseHandle(g_gpu_thread);
    g_gpu_thread = nullptr;
}
void SetGpuLatencyUs(int latency_us) {
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    g_latency_ticks.store(latency_us > 0 ? frequency.QuadPart * latency_us / 1000000 : 0, std::memory_order_relaxed);
}

// ── The slot indices the fakes assume, against the SDK ──
namespace {
// The slot a pointer to a virtual member function stands for, from the compiler's virtual call thunk (MSVC x64):
// mov rax,[rcx]; jmp [rax+imm]   or, with control flow guard,   mov rax,[rcx]; mov rax,[rax+imm]; ...
// -1 when the thunk is not one of these (an incremental link's jump is followed first).
template <class Member> int SlotOfVirtual(Member member) noexcept {
    static_assert(sizeof(Member) == sizeof(void*), "single inheritance member pointer expected");
    void* address = nullptr;
    std::memcpy(&address, &member, sizeof(address));
    const unsigned char* p = static_cast<const unsigned char*>(address);
    if (p[0] == 0xE9) { // jmp rel32 (incremental linking)
        int32_t rel = 0;
        std::memcpy(&rel, p + 1, sizeof(rel));
        p = p + 5 + rel;
    }
    if (p[0] != 0x48 || p[1] != 0x8B || p[2] != 0x01) return -1; // mov rax,[rcx]
    if (p[3] == 0xFF && p[4] == 0x20) return 0;                       // jmp [rax]
    if (p[3] == 0xFF && p[4] == 0x60) return p[5] / 8;                // jmp [rax+imm8]
    if (p[3] == 0xFF && p[4] == 0xA0) { int32_t o = 0; std::memcpy(&o, p + 5, 4); return o / 8; }
    if (p[3] == 0x48 && p[4] == 0x8B && p[5] == 0x40) return p[6] / 8; // mov rax,[rax+imm8]
    if (p[3] == 0x48 && p[4] == 0x8B && p[5] == 0x80) { int32_t o = 0; std::memcpy(&o, p + 6, 4); return o / 8; }
    return -1;
}
} // namespace

bool VerifyVtableLayout(std::string* detail) {
    struct Case { const char* name; int actual; int expected; };
    const Case cases[] = {
        {"ID3D12CommandQueue::ExecuteCommandLists", SlotOfVirtual(&ID3D12CommandQueue::ExecuteCommandLists), 10},
        {"ID3D12CommandQueue::Signal", SlotOfVirtual(&ID3D12CommandQueue::Signal), 14},
        {"ID3D12CommandQueue::Wait", SlotOfVirtual(&ID3D12CommandQueue::Wait), 15},
        {"ID3D12Fence::GetCompletedValue", SlotOfVirtual(&ID3D12Fence::GetCompletedValue), 8},
        {"ID3D12Fence::SetEventOnCompletion", SlotOfVirtual(&ID3D12Fence::SetEventOnCompletion), 9},
        {"ID3D12Fence::Signal", SlotOfVirtual(&ID3D12Fence::Signal), 10},
        {"ID3D12Device::CreateFence", SlotOfVirtual(&ID3D12Device::CreateFence), 36},
    };
    size_t decoded = 0;
    detail->clear();
    for (const Case& c : cases) {
        if (c.actual < 0) continue;
        ++decoded;
        if (c.actual != c.expected) {
            char line[200];
            std::snprintf(line, sizeof(line), "%s is slot %d in the SDK, the fake assumes %d", c.name, c.actual, c.expected);
            *detail = line;
            return false;
        }
    }
    *detail = decoded ? "the fake vtable slots match the SDK's (virtual call thunks decoded)"
                      : "not verified (the compiler's virtual call thunks are not decodable here)";
    return true;
}

bool LogContains(const char* text) {
    std::lock_guard<std::mutex> guard(g_log_lock);
    for (const std::string& line : g_log) if (line.find(text) != std::string::npos) return true;
    return false;
}
void DumpLog(std::FILE* out, size_t last_lines) {
    std::lock_guard<std::mutex> guard(g_log_lock);
    const size_t first = g_log.size() > last_lines ? g_log.size() - last_lines : 0;
    for (size_t i = first; i < g_log.size(); ++i) std::fprintf(out, "[probe log] %s\n", g_log[i].c_str());
}
std::vector<PacingGpuQueueRecord> Records() {
    std::lock_guard<std::mutex> guard(g_records_lock);
    return g_records;
}
bool WaitForRecordSequence(uint64_t sequence, DWORD timeout_ms) {
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    for (;;) {
        {
            std::lock_guard<std::mutex> guard(g_records_lock);
            for (const PacingGpuQueueRecord& record : g_records) if (record.present_sequence >= sequence) return true;
        }
        if (GetTickCount64() > deadline) return false;
        Sleep(2);
    }
}

// ── The game ──
Game::Game() {
    timeBeginPeriod(1);
    device = CreateDevice();
    render = CreateQueue(device, "render");
    present = CreateQueue(device, "present");
    other = CreateQueue(device, "other");
    spare = CreateQueue(device, "spare");
    marker = CreateGameFence(device, "game marker");
    dependency = CreateGameFence(device, "dependency");
    gate = CreateGameFence(device, "gate");
    aux = CreateGameFence(device, "aux");
    g_gpu_queue_probe.store(true, std::memory_order_relaxed);
    g_gpu_queue_probe_source.store(1, std::memory_order_relaxed);
    // The real detours over the fake methods: the shared entry point of the probe's adoption and of Streamline.
    if (!StreamlineQueue_EnsureHooks(NativeMethods(render))) {
        std::fprintf(stderr, "StreamlineQueue_EnsureHooks refused the fake queue methods\n");
        std::_Exit(2);
    }
    StartGpu();
}

Game::~Game() {
    StopGpu();
    timeEndPeriod(1);
}

void Game::Submit(unsigned lists) {
    GameExecute(render, lists);
    GameSignal(render, marker, marker_value.fetch_add(1, std::memory_order_relaxed) + 1);
}
void Game::ExecuteOnly(unsigned lists) { GameExecute(render, lists); }
void Game::BoundarySignal(uint64_t sequence) { GameSignal(render, dependency, sequence); }

void Game::PresentSide(uint64_t sequence) {
    PacingFrameKey key;
    key.domain = PacingIdentityDomain::NvApiFrame;
    key.producer_epoch = 1;
    key.frame_id = sequence;
    GpuQueueProbe_NotePresentQueue(Com(present));       // ReShade's present event: the Present scope opens
    GameExecute(present, 1);                            // the game's Present work on the presenting queue
    GameWait(present, dependency, sequence);            // the proxy waits for the render queue's frame inside the scope
    GpuQueueProbe_OnReadinessSite(generation, sequence, key);
    GpuQueueProbe_DependencyScopeEnd();                 // finish_present
}

void Game::PlayFrame() {
    const uint64_t sequence = frame + 1;
    for (unsigned i = 0, n = Submissions(sequence); i < n; ++i) Submit();
    BoundarySignal(sequence);
    PresentSide(sequence);
    frame = sequence;
}

uint64_t Game::WaitUnsatisfied() {
    const uint64_t value = gate_value.fetch_add(1, std::memory_order_relaxed) + 1; // above the completed value
    GameWait(render, gate, value);
    return value;
}

void Game::SignalForeign() {
    GameSignal(other, marker, marker_value.fetch_add(1, std::memory_order_relaxed) + 1);
}

uint64_t Game::ProbeQueueSignals() const {
    std::lock_guard<std::mutex> guard(device->lock);
    uint64_t total = 0;
    for (const Fence* fence : device->fences) if (fence->from_com) total += fence->queue_signals.load(std::memory_order_relaxed);
    return total;
}

bool Game::WaitBorrowSteady() {
    uint64_t own = ProbeQueueSignals();
    int stable = 0;
    for (int i = 0; i < 600 && stable < 12; ++i) {
        PlayFrame();
        Sleep(1);
        const uint64_t now = ProbeQueueSignals();
        stable = now == own ? stable + 1 : 0;
        own = now;
    }
    return stable >= 12;
}

bool Game::BringUp(bool by_rule, std::string* how) {
    void* const render_com = static_cast<void*>(Com(render));
    if (by_rule) {
        for (int i = 0; i < 260 && !GpuQueueProbeTest_AdoptedQueue(); ++i) { PlayFrame(); Sleep(1); }
        if (GpuQueueProbeTest_AdoptedQueue() != render_com || GpuQueueProbeTest_AdoptedRole() != 3) {
            *how = GpuQueueProbeTest_AdoptedQueue()
                ? "the dependency rule adopted another queue or role than the render queue as role 3"
                : "the dependency rule did not adopt a queue within 260 frames";
            return false;
        }
        *how = "the dependency rule";
    } else {
        for (int i = 0; i < 6; ++i) { PlayFrame(); Sleep(1); }
        if (!GpuQueueProbeTest_AdoptRenderQueue(render_com, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(Com(dependency))),
                generation, frame + 1)) {
            *how = "the test seam's adoption was refused";
            return false;
        }
        *how = "the test seam (GpuQueueProbeTest_AdoptRenderQueue)";
    }
    for (int i = 0; i < 3000 && !GpuQueueProbeTest_Running(); ++i) { PlayFrame(); Sleep(1); }
    if (!GpuQueueProbeTest_Running()) {
        *how += ": the session never reached Running (calibration)";
        return false;
    }
    if (!WaitBorrowSteady()) {
        *how += ": borrowing did not become steady (the probe kept adding its own Signals)";
        return false;
    }
    return true;
}

} // namespace glue

BOOL GlueCountedQpc(LARGE_INTEGER* value) noexcept {
    ++glue::g_thread.qpc_reads;
    return QueryPerformanceCounter(value); // this file is compiled without the counting macro
}

// ── Stubs of the externals the glue calls (tests/AGENTS.md: update with the contract they stand for) ──
// The trail (startup_breadcrumb.h): the glue marks session start and end, the queue hook entry and the first call of
// each detour; nothing here records them.
void Breadcrumb_Trail(TrailKind, uint8_t, uint32_t, uint16_t) noexcept {}
void Log_Write(LogLevel, const char* fmt, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    _vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, fmt, args);
    va_end(args);
    std::lock_guard<std::mutex> guard(glue::g_log_lock);
    glue::g_log.emplace_back(buffer);
    if (glue::g_log_echo) std::fprintf(stderr, "[probe log] %s\n", buffer);
}

bool RuntimeLifecycle_IsStopping() noexcept { return false; }
RuntimeCallbackGuard::RuntimeCallbackGuard() noexcept { stopping_ = false; }
RuntimeCallbackGuard::~RuntimeCallbackGuard() {}

// Hook installation: the "trampoline" of a hook is the target itself (the fake queue's native method), which is what
// the original pointer is for the probe (its own Signals and the detours' forwarding).
HookInstallGuard::HookInstallGuard() noexcept { admitted_ = true; }
HookInstallGuard::~HookInstallGuard() {}
bool Hooks_PinTargetModule(void*) noexcept { return true; }
MH_STATUS InstallHookPublished(void* target, void*, void* original_slot, HookOriginalPublisher publish) {
    publish(original_slot, target);
    return MH_OK;
}
MH_STATUS Hooks_FlushBatch() noexcept { return MH_OK; }
bool Hooks_TargetEnabled(void*) noexcept { return true; }

// The Streamline handoff is not part of the glue under test (streamline_ready stays false): nothing is tracked.
void StreamlineHandoff_Invalidate(const char*, uint64_t) noexcept {}
void StreamlineHandoff_Service(bool) noexcept {}
StreamlineFenceCall StreamlineHandoff_BeforeFence(ID3D12CommandQueue*, ID3D12Fence*, uint64_t, uint64_t, bool) noexcept { return {}; }
void StreamlineHandoff_AfterFence(const StreamlineFenceCall&, HRESULT) noexcept {}
void StreamlineHandoff_Execute(ID3D12CommandQueue*) noexcept {}
bool StreamlineHandoff_Interested(ID3D12Fence*) noexcept { return false; }
void StreamlineHandoff_RejectQueueMethods() noexcept {}
void StreamlineHandoff_RecordQueueFence(ID3D12CommandQueue*, ID3D12Fence*, uint64_t, bool) noexcept {}
void StreamlineHandoff_ChainCallFailed(ID3D12CommandQueue*) noexcept {}
void StreamlineHandoff_ProbeSourceSignal(ID3D12CommandQueue*) noexcept {}

bool GpuReadinessQueue_Retain(ID3D12CommandQueue* queue) noexcept {
    queue->AddRef();
    return true;
}

void PacingTelemetry_PushGpuQueue(const PacingGpuQueueRecord& record) noexcept {
    std::lock_guard<std::mutex> guard(glue::g_records_lock);
    glue::g_records.push_back(record);
}
void GpuQueueLawFeed_Push(const PacingGpuQueueRecord&) noexcept {}
void GpuQueueLawFeed_RetireSession(uint32_t) noexcept {}
