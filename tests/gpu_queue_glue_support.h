#pragma once

// Support of the two executables that run the REAL gpu_queue_probe glue (src/gpu_queue_probe_d3d12.cpp,
// src/streamline_queue_hooks.cpp, with the pure core, borrow and dependency units) against FAKE D3D12 objects:
// tests/test_gpu_queue_glue.cpp (ctest, functional) and tests/bench_gpu_queue_hooks.cpp (not a ctest, cost).
//
// What is real: every line of the glue and of the pure units, the queue detours' entry points
// (streamline_queue::ObserveExecute / ObserveSignal / ObserveWait are called exactly as the installed detours are:
// the "native" methods they forward to are the fake queue's own), the worker thread, the calibration helper
// thread, the events and the fences' registrations, the telemetry records the worker pops.
// What is fake: ID3D12CommandQueue / ID3D12Fence / ID3D12Device (hand-written COM vtables laid out like the SDK's:
// IUnknown 0-2, ID3D12Object 3-6, GetDevice 7; queue Execute 10, Signal 14, Wait 15; fence GetCompletedValue 8,
// SetEventOnCompletion 9, Signal 10; device CreateFence 36), a small GPU thread that completes the queues' Signals in
// queue order (a Wait blocks the queue until its fence value is reached), and stubs of the externals the glue calls
// (logger, runtime lifecycle, hook installation, the Streamline handoff, telemetry, the law feed, the queue
// reference). What is not covered: see the header comment of each executable.

#include <Windows.h>
#include <d3d12.h>

#include <array>
#include <cstdio>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "pacing_telemetry_types.h"

// The production detours (src/streamline_queue_hooks.cpp, external linkage in that namespace).
namespace streamline_queue {
void STDMETHODCALLTYPE ObserveExecute(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists);
HRESULT STDMETHODCALLTYPE ObserveSignal(ID3D12CommandQueue* queue, ID3D12Fence* fence, UINT64 value);
HRESULT STDMETHODCALLTYPE ObserveWait(ID3D12CommandQueue* queue, ID3D12Fence* fence, UINT64 value);
}

namespace glue {

// What the calling thread did through the fakes (thread-local, so the measured thread is not mixed with the probe's
// worker and helper threads). qpc_reads counts the glue's QueryPerformanceCounter reads when the glue sources are
// compiled with tests/gpu_queue_glue_qpc_counter.h (MSVC force-include of tests/CMakeLists.txt).
struct ThreadCounters {
    uint64_t fence_reads = 0;   // ID3D12Fence::GetCompletedValue
    uint64_t queue_signals = 0; // ID3D12CommandQueue::Signal
    uint64_t queue_waits = 0;   // ID3D12CommandQueue::Wait
    uint64_t executes = 0;      // ID3D12CommandQueue::ExecuteCommandLists
    uint64_t cpu_signals = 0;   // ID3D12Fence::Signal
    uint64_t qpc_reads = 0;     // QueryPerformanceCounter, by the glue
};
extern thread_local ThreadCounters g_thread;

struct Device;

// A fence. The completed value moves only when the fake GPU (or a CPU Signal) says so.
struct Fence {
    void* const* vtbl = nullptr; // FIRST member: this struct is the COM object
    std::atomic<long> refs{1};
    Device* device = nullptr;
    std::string name;
    bool from_com = false;       // created through ID3D12Device::CreateFence (by the probe), not by the test
    std::atomic<uint64_t> completed{0};
    std::atomic<uint64_t> queue_signals{0}; // ID3D12CommandQueue::Signal calls naming this fence, any thread
    std::atomic<uint64_t> reads{0};         // GetCompletedValue calls, any thread
    std::atomic<uint64_t> cpu_signals{0};
    std::atomic<uint64_t> registrations{0}; // SetEventOnCompletion calls
    // The fence reaches `value` (sets the completed value, like a Signal does) and fires the registered events.
    void Advance(uint64_t value);
    uint64_t Completed() const noexcept { return completed.load(std::memory_order_acquire); }
    std::mutex lock;
    struct Registration { uint64_t value; HANDLE event; };
    std::vector<Registration> registered;
};

// A queue. Execute is counted; Signal and Wait are queued in call order and completed by the fake GPU.
struct Queue {
    struct Op { Fence* fence = nullptr; uint64_t value = 0; int64_t ready = 0; bool wait = false; };
    static constexpr size_t kRing = size_t(1) << 15;
    void* const* vtbl = nullptr; // FIRST member
    std::atomic<long> refs{1};
    Device* device = nullptr;
    std::string name;
    std::atomic<uint64_t> executes{0}, lists{0}, signals{0}, waits{0};
    // Test hook: while set, the queue's native Wait does not return (it waits on this event, 30 s at most): a detour held
    // inside the probe's callback across the native call. waits_blocked counts the calls inside.
    std::atomic<HANDLE> wait_gate{nullptr};
    std::atomic<unsigned> waits_blocked{0};
    // Test hook: while set and the Signal names signal_gate_fence, the queue's native Signal does not return (it waits on the
    // event, 30 s at most; it is not queued until then): the game's Signal held back after the probe's bind. signals_blocked counts them.
    std::atomic<HANDLE> signal_gate{nullptr};
    std::atomic<Fence*> signal_gate_fence{nullptr};
    std::atomic<unsigned> signals_blocked{0};
    Queue() : ring(new Op[kRing]) {}
    void Push(Fence* fence, uint64_t value, bool wait) noexcept;
    // The GPU side: completes, in order, what is ready (a Signal reaches its value, a Wait is passed once its fence is
    // there). `now` is only read for operations with a latency. True when it did anything.
    bool Process(int64_t now) noexcept;
private:
    std::atomic<bool> lock_{false};
    uint64_t head_ = 0, tail_ = 0;
    std::unique_ptr<Op[]> ring;
};

struct Device {
    void* const* vtbl = nullptr; // FIRST member
    std::atomic<long> refs{1};
    std::atomic<uint64_t> create_fence_calls{0};
    std::mutex lock;
    std::vector<Fence*> fences; // every fence of the device in creation order
};

inline ID3D12CommandQueue* Com(Queue* queue) noexcept { return reinterpret_cast<ID3D12CommandQueue*>(queue); }
inline ID3D12Fence* Com(Fence* fence) noexcept { return reinterpret_cast<ID3D12Fence*>(fence); }

Device* CreateDevice();
Queue* CreateQueue(Device* device, const char* name);
Fence* CreateGameFence(Device* device, const char* name); // a fence the test owns (not the probe's)
// The three native methods of a queue (vtable slots 10, 14, 15), for StreamlineQueue_EnsureHooks.
std::array<uint64_t, 3> NativeMethods(Queue* queue);

// The fake GPU thread: processes every queue created so far and later (up to 16), spinning. `latency_us` delays each
// queued operation (0 = complete as soon as the thread sees it).
void StartGpu();
void StopGpu();
void SetGpuLatencyUs(int latency_us);

// Compares the slot indices the fakes assume with the SDK's declarations (decoding the compiler's virtual call
// thunks); true when they agree or when the thunks cannot be decoded (detail says which).
bool VerifyVtableLayout(std::string* detail);

// What the stubs captured.
bool LogContains(const char* text);
void DumpLog(std::FILE* out, size_t last_lines);
std::vector<PacingGpuQueueRecord> Records(); // every record the worker popped so far, in pop order
// Waits until a record with present_sequence >= sequence was popped.
bool WaitForRecordSequence(uint64_t sequence, DWORD timeout_ms);

// The calls of the real detours, as the game makes them.
inline void GameExecute(Queue* queue, UINT lists = 1) {
    streamline_queue::ObserveExecute(Com(queue), lists, nullptr);
}
inline void GameSignal(Queue* queue, Fence* fence, uint64_t value) {
    streamline_queue::ObserveSignal(Com(queue), Com(fence), value);
}
inline void GameWait(Queue* queue, Fence* fence, uint64_t value) {
    streamline_queue::ObserveWait(Com(queue), Com(fence), value);
}

// A game: a render queue (the one the probe adopts, role 3), the presenting queue, another queue, a spare queue that
// is never adopted; the game's marker fence G (the Signal directly behind every submission), the dependency fence F
// (one Signal per frame on the render queue = the frame boundary, waited by the presenting queue inside the Present
// scope), a fence for unsatisfied Waits (gate) and an auxiliary one. Creating it installs the real detours over the
// fake queues' methods (StreamlineQueue_EnsureHooks), turns the probe keys on and starts the fake GPU.
class Game {
public:
    Game();
    ~Game();
    Game(const Game&) = delete;
    Game& operator=(const Game&) = delete;

    Device* device = nullptr;
    Queue *render = nullptr, *present = nullptr, *other = nullptr, *spare = nullptr;
    Fence *marker = nullptr, *dependency = nullptr, *gate = nullptr, *aux = nullptr;
    std::atomic<uint64_t> marker_value{0}, aux_value{0}, gate_value{0};
    uint64_t frame = 0;       // the last frame the single-threaded helpers closed
    uint64_t generation = 1;  // the swapchain generation of every Present

    // Frame f has Submissions(f) submissions: the tests identify a record's frame by its submit count.
    static unsigned Submissions(uint64_t sequence) noexcept { return 3 + static_cast<unsigned>(sequence % 4); }

    // Render thread: one submission (an Execute of `lists` command lists and the game's own Signal of G directly behind
    // it, value + 1), or an Execute alone.
    void Submit(unsigned lists = 2);
    void ExecuteOnly(unsigned lists = 2);
    // Render thread: the frame boundary: the Signal of the dependency fence F with value `sequence`.
    void BoundarySignal(uint64_t sequence);
    // Present thread: ReShade's present event (the presenting queue is noted), the game's Present on the presenting
    // queue (one Execute, the Wait of F = sequence inside the scope), the readiness site (the frame's identity:
    // generation, sequence, a valid key), finish_present.
    void PresentSide(uint64_t sequence);
    // One whole single-threaded frame (Submissions(frame + 1) submissions, boundary, present side).
    void PlayFrame();
    // An unsatisfied Wait on the render queue for the gate fence (a value above its completed one). Returns the value;
    // the caller releases it with gate->Advance(value).
    uint64_t WaitUnsatisfied();
    // Another queue signals the game's marker fence (a foreign Signal).
    void SignalForeign();

    // Signals of the probe's own fences (created through the device's CreateFence) on any queue.
    uint64_t ProbeQueueSignals() const;
    // Adopts the render queue as role 3 and plays frames until the session runs with borrowing healthy: the game's
    // Signals mark every submission and the probe adds none. by_rule: the Present side's dependency rule must decide it
    // (the dependency window of 64 frames); otherwise the test seam starts the session. `how` says what happened.
    bool BringUp(bool by_rule, std::string* how);
    // Plays frames until the probe has added no queue Signal for 12 frames in a row (false after 600 frames).
    bool WaitBorrowSteady();
};

} // namespace glue
