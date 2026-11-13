// Cost benchmark of the hooks' own code (NOT a ctest): nanoseconds per call of the REAL queue detours
// (src/streamline_queue_hooks.cpp ObserveExecute / ObserveSignal / ObserveWait) with the REAL gpu_queue_probe glue
// (src/gpu_queue_probe_d3d12.cpp, the pure core, borrow and dependency units) behind them, against FAKE queue / fence /
// device objects (tests/gpu_queue_glue_support.h: no GPU, completions by a fake GPU thread). Single measured thread, warm-up
// included, the median over many frames, with the calls of the thread counted (fence reads, queue Signals, clock reads).
//
// Cases (a "pair" is the game's ExecuteCommandLists followed directly by the game's own Signal of its marker fence):
//   fake only            the fake queue's methods called directly: the floor (what the fakes cost; subtract it)
//   detours, key off     the real detours with the probe key off (g_gpu_queue_probe = false): the shared detour alone
//   detours, key on      the real detours on a queue the probe does not adopt (the dependency tables' records, the probe's
//                        quick rejections)
//   adopted, borrowed    the real detours on the adopted render queue (role 3), borrowing steady: one Execute + the borrowed
//                        Signal pair per call group; the probe adds no Signal (checked: its own queue Signals are reported)
//   Wait (adopted)       an unsatisfied Wait on the adopted queue (WaitBegin, the native Wait, WaitEnd with the probe's own
//                        Signal behind it), measured per Wait; the same Wait on a queue that is not adopted and the fake
//                        floor are reported next to it
// "Added" columns are the case's median minus the floor's.
//
// What the numbers include: the glue's own code, its atomics and fences reads, its QueryPerformanceCounter reads (counted
// through tests/gpu_queue_glue_qpc_counter.h when built with MSVC), the fake queue's own cost of recording a Signal. What
// they do not: the real D3D12 runtime's cost of ExecuteCommandLists / Signal / GetCompletedValue (the fakes are a few
// atomics), kernel transitions of a real SetEventOnCompletion, cold caches of a real submission thread (frames are spaced by
// a spin, so the hooks' working set stays hot: a lower bound on the cost per call), and other threads' interference. The
// worker thread, its helper and the fake GPU thread run alongside on other cores.
//
// Usage: bench_gpu_queue_hooks [--frames=N] [--warmup=N] [--pairs=N] [--frame-us=N] [--rule]
//   defaults: 3000 measured frames after 1000 warm-up frames, 6 pairs per frame, one frame every 400 us.
//   --rule: the dependency rule adopts the render queue (64-frame window) instead of the test seam.

#include "gpu_queue_glue_support.h"

#include "gpu_queue_probe.h"
#include "gpu_queue_probe_test_seam.h"
#include "instrumentation_state.h"

#include <intrin.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using glue::Game;
using glue::ThreadCounters;

namespace {

struct Options {
    unsigned frames = 3000, warmup = 1000, pairs = 6;
    double frame_us = 400.0;
    bool by_rule = false;
};

double g_ns_per_tick = 0.0;
inline uint64_t Tsc() {
    _mm_lfence();
    const uint64_t t = __rdtsc();
    _mm_lfence();
    return t;
}
void CalibrateTsc() {
    LARGE_INTEGER frequency{}, a{}, b{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&a);
    const uint64_t t0 = Tsc();
    do { QueryPerformanceCounter(&b); } while ((b.QuadPart - a.QuadPart) * 1000 < frequency.QuadPart * 200); // 200 ms
    const uint64_t t1 = Tsc();
    g_ns_per_tick = static_cast<double>(b.QuadPart - a.QuadPart) * 1e9 / static_cast<double>(frequency.QuadPart) / static_cast<double>(t1 - t0);
}

struct Measurement {
    std::string name;
    double median = 0, p10 = 0, p90 = 0;
    double fence_reads = 0, queue_signals = 0, qpc_reads = 0;
    uint64_t probe_signals = 0;     // the probe's own queue Signals during the measured frames
    size_t calls = 0;
    int floor = -1;                 // index of the fake-only case this one is compared with (-1: it is one)
};

double Quantile(std::vector<double>& samples, double q) {
    if (samples.empty()) return 0.0;
    const size_t index = (std::min)(samples.size() - 1, static_cast<size_t>(q * static_cast<double>(samples.size())));
    std::nth_element(samples.begin(), samples.begin() + static_cast<std::ptrdiff_t>(index), samples.end());
    return samples[index];
}

// One case: per frame, `pre` (untimed), `body` (timed, `calls` calls of the measured kind), `after` (untimed: the frame's
// boundary and Present side); the frame's start to the next is spaced by a spin so the worker thread keeps up.
template <class Pre, class Body, class After>
Measurement Run(const char* name, const Options& options, Game& game, unsigned calls, Pre&& pre, Body&& body, After&& after) {
    Measurement m;
    m.name = name;
    std::vector<double> samples;
    samples.reserve(options.frames);
    const uint64_t spacing = static_cast<uint64_t>(options.frame_us * 1000.0 / g_ns_per_tick);
    ThreadCounters total{};
    uint64_t own_before = 0;
    for (unsigned f = 0; f < options.warmup + options.frames; ++f) {
        if (f == options.warmup) own_before = game.ProbeQueueSignals();
        const uint64_t start = Tsc();
        pre();
        const ThreadCounters c0 = glue::g_thread;
        const uint64_t t0 = Tsc();
        body();
        const uint64_t t1 = Tsc();
        const ThreadCounters c1 = glue::g_thread;
        after();
        if (f >= options.warmup) {
            samples.push_back(static_cast<double>(t1 - t0) * g_ns_per_tick / static_cast<double>(calls));
            total.fence_reads += c1.fence_reads - c0.fence_reads;
            total.queue_signals += c1.queue_signals - c0.queue_signals;
            total.executes += c1.executes - c0.executes;
            total.qpc_reads += c1.qpc_reads - c0.qpc_reads;
        }
        while (Tsc() - start < spacing) YieldProcessor();
    }
    m.calls = static_cast<size_t>(options.frames) * calls;
    const double n = static_cast<double>(m.calls);
    m.fence_reads = static_cast<double>(total.fence_reads) / n;
    m.queue_signals = static_cast<double>(total.queue_signals) / n;
    m.qpc_reads = static_cast<double>(total.qpc_reads) / n;
    m.probe_signals = game.ProbeQueueSignals() - own_before;
    m.p10 = Quantile(samples, 0.10);
    m.p90 = Quantile(samples, 0.90);
    m.median = Quantile(samples, 0.50);
    return m;
}

bool ParseOption(const char* arg, const char* key, double* out) {
    const size_t length = std::strlen(key);
    if (std::strncmp(arg, key, length) != 0 || arg[length] != '=') return false;
    *out = std::atof(arg + length + 1);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        double value = 0;
        if (ParseOption(argv[i], "--frames", &value)) options.frames = static_cast<unsigned>(value);
        else if (ParseOption(argv[i], "--warmup", &value)) options.warmup = static_cast<unsigned>(value);
        else if (ParseOption(argv[i], "--pairs", &value)) options.pairs = static_cast<unsigned>(value);
        else if (ParseOption(argv[i], "--frame-us", &value)) options.frame_us = value;
        else if (std::strcmp(argv[i], "--rule") == 0) options.by_rule = true;
        else { std::fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }
    if (options.pairs == 0 || options.frames == 0) { std::fprintf(stderr, "--pairs and --frames must be positive\n"); return 2; }
    CalibrateTsc();

    Game game;
    const auto none = [] {};
    const unsigned pairs = options.pairs;
    uint64_t spare_value = 0, spare_wait_value = 0;
    auto* const spare = glue::Com(game.spare);
    auto* const aux = glue::Com(game.aux);
    glue::Fence* const spare_gate = glue::CreateGameFence(game.device, "spare gate");
    auto* const spare_gate_com = glue::Com(spare_gate);

    std::vector<Measurement> results;
    const auto add = [&](Measurement m, int floor) {
        m.floor = floor;
        results.push_back(std::move(m));
        return static_cast<int>(results.size()) - 1;
    };
    // The frame's boundary and Present side, as every frame of the adopted queue has them.
    const auto end_frame = [&] {
        const uint64_t sequence = ++game.frame;
        game.BoundarySignal(sequence);
        game.PresentSide(sequence);
    };

    // Timer overhead: the timed region empty.
    const Measurement overhead = Run("timer overhead (empty region)", options, game, 1, none, none, none);

    // ── before the adoption (the probe keys off must not be toggled while a session runs: it would end it) ──
    const int floor_before = add(Run("pair: fake only", options, game, pairs, none, [&] {
        for (unsigned i = 0; i < pairs; ++i) {
            spare->ExecuteCommandLists(2, nullptr);
            spare->Signal(aux, ++spare_value);
        }
    }, none), -1);
    g_gpu_queue_probe.store(false, std::memory_order_relaxed);
    g_gpu_queue_probe_source.store(0, std::memory_order_relaxed);
    add(Run("pair: detours, probe key off", options, game, pairs, none, [&] {
        for (unsigned i = 0; i < pairs; ++i) {
            glue::GameExecute(game.spare, 2);
            glue::GameSignal(game.spare, game.aux, ++spare_value);
        }
    }, none), floor_before);
    g_gpu_queue_probe.store(true, std::memory_order_relaxed);
    g_gpu_queue_probe_source.store(1, std::memory_order_relaxed);

    std::string how;
    if (!game.BringUp(options.by_rule, &how)) {
        std::fprintf(stderr, "bring-up failed: %s\n", how.c_str());
        glue::DumpLog(stderr, 30);
        return 1;
    }
    std::printf("adopted the render queue as role 3 through %s; running, borrowing steady\n", how.c_str());
    std::printf("tsc %.4f ns/tick; %u measured frames after %u warm-up frames, %u pairs per frame, a frame every %.0f us\n\n", g_ns_per_tick,
        options.frames, options.warmup, options.pairs, options.frame_us);

    // ── with the session running ──
    const int floor_pair = add(Run("pair: fake only (session running)", options, game, pairs, none, [&] {
        for (unsigned i = 0; i < pairs; ++i) {
            spare->ExecuteCommandLists(2, nullptr);
            spare->Signal(aux, ++spare_value);
        }
    }, none), -1);
    add(Run("pair: detours, key on, not adopted", options, game, pairs, none, [&] {
        for (unsigned i = 0; i < pairs; ++i) {
            glue::GameExecute(game.spare, 2);
            glue::GameSignal(game.spare, game.aux, ++spare_value);
        }
    }, none), floor_pair);
    const int adopted_pair = add(Run("pair: adopted queue, borrowed", options, game, pairs, none, [&] {
        for (unsigned i = 0; i < pairs; ++i) game.Submit();
    }, end_frame), floor_pair);

    const int floor_wait = add(Run("Wait: fake only", options, game, 1, none, [&] {
        spare->Wait(spare_gate_com, ++spare_wait_value);
    }, [&] { spare_gate->Advance(spare_wait_value); }), -1);
    add(Run("Wait: detours, key on, not adopted", options, game, 1, none, [&] {
        glue::GameWait(game.spare, spare_gate, ++spare_wait_value);
    }, [&] { spare_gate->Advance(spare_wait_value); }), floor_wait);
    const int adopted_wait = add(Run("Wait: adopted queue, unsatisfied", options, game, 1,
        [&] { for (unsigned i = 0; i < pairs; ++i) game.Submit(); }, // the frame's submissions stay borrowed around it
        [&] { game.WaitUnsatisfied(); },
        [&] { game.gate->Advance(game.gate_value.load(std::memory_order_relaxed)); end_frame(); }), floor_wait);

    // ── report ──
    std::printf("%-36s %8s %8s %8s  %8s  %7s %7s %7s  %10s\n", "case (ns per pair, or per Wait)", "median", "p10", "p90", "added", "reads",
        "Signals", "QPC", "probe Sigs");
    std::printf("%-36s %8.1f   (the empty timed region; removed from every row below)\n", overhead.name.c_str(), overhead.median);
    for (const Measurement& m : results) {
        char added[32] = "";
        if (m.floor >= 0) std::snprintf(added, sizeof(added), "%+8.1f", m.median - results[static_cast<size_t>(m.floor)].median);
        std::printf("%-36s %8.1f %8.1f %8.1f  %8s  %7.2f %7.2f %7.2f  %10llu\n", m.name.c_str(), m.median - overhead.median,
            m.p10 - overhead.median, m.p90 - overhead.median, added, m.fence_reads, m.queue_signals, m.qpc_reads,
            static_cast<unsigned long long>(m.probe_signals));
    }
    std::printf("\nreads / Signals / QPC: GetCompletedValue calls, ID3D12CommandQueue::Signal calls and (MSVC counting force-include)\n"
                "QueryPerformanceCounter reads PER MEASURED CALL GROUP (pair or Wait) on the measured thread, whatever their kind (the\n"
                "game's own Signals count). probe Sigs: the probe's own queue Signals over the measured frames. \"added\" = the row's\n"
                "median minus its fake-only floor (the timer overhead cancels in it).\n");
    const Measurement& pair_row = results[static_cast<size_t>(adopted_pair)];
    const Measurement& wait_row = results[static_cast<size_t>(adopted_wait)];
    if (pair_row.qpc_reads == 0.0 && wait_row.qpc_reads == 0.0)
        std::printf("note: no QueryPerformanceCounter reads were counted (the counting force-include is MSVC only).\n");
    std::printf("steady-state check: adopted pairs: %llu probe Signals over %zu pairs (expected 0); adopted Waits: %llu probe Signals over "
                "%zu Waits (expected one each).\n", static_cast<unsigned long long>(pair_row.probe_signals), pair_row.calls,
                static_cast<unsigned long long>(wait_row.probe_signals), wait_row.calls);
    std::fflush(stdout);
    std::_Exit(0); // the session's threads and the fake GPU thread end with the process
}
