// Functional test of the REAL gpu_queue_probe glue (src/gpu_queue_probe_d3d12.cpp, src/streamline_queue_hooks.cpp, with
// the pure core, borrow and dependency units) through FAKE D3D12 queue / fence / device objects (tests/gpu_queue_glue_support.h).
// The test plays the game: its calls go through the production detours (streamline_queue::ObserveExecute / ObserveSignal /
// ObserveWait, which forward to the fake queue's own methods exactly as the installed detours forward to the native
// ones) and the production entry points of the Present side (GpuQueueProbe_NotePresentQueue / OnReadinessSite /
// DependencyScopeEnd). A fake GPU thread completes the queues' Signals in order; the probe's worker thread, its
// calibration helper, its events and fence registrations run for real, and the records it pops are collected.
//
// What it proves (each with the number the assertion is made on):
//   1. adoption of the render queue as role 3 (by the test seam by default, by the dependency rule with --rule), the
//      session reaching Running, the borrowing becoming steady: the game's Signal directly behind every submission is the
//      marker and the probe adds NO queue Signal (fake counters on the probe's fences);
//   2. the game's Signals still reach the queue exactly once each (the detour forwards);
//   3. records reach the pop with valid frames: identity (swapchain generation, present sequence, key), submit counts equal
//      to the frame's real submissions, borrowed_submits > 0, a done time;
//   4. unsatisfied Waits on the adopted queue produce one own probe Signal each, satisfied Waits none;
//   5. a short interleaved three-thread run (render thread, Present thread, a second submitter racing every tenth frame's
//      boundary on the adopted queue): no valid record has a wrong submit count, no identity is duplicated, and the
//      submissions of the racing pair of frames are conserved when both are valid (in-flight
//      visibility, the closing handshake, claims under the sequencing lock, as far as a short run reaches them);
//   6. a foreign Signal of the bound fence from another queue ends borrowing: from then on the probe marks every
//      submission itself and later records carry no borrowed submissions;
//   7. the one reference on the game's fence: taken at the bind, released with the session; Shutdown under load returns
//      and the queue, device and probe fences are released.
//
// What it cannot cover: a real driver's completion timing and wake-ups, the real D3D12 runtime's behaviour on a Signal
// below the completed value, hook installation by MinHook, the Streamline handoff (stubbed, streamline_ready stays
// false), the real frame generation / presenting-queue roles (only role 3 is driven), races that need a GPU's real
// latency, and anything that needs the game. Live validation stays required.
//
// Usage: test_gpu_queue_glue            adoption through the test seam (deterministic)
//        test_gpu_queue_glue --rule     adoption through the dependency rule; exit code 77 (skipped) when the rule does not
//                                       decide the render queue within its window (the log is printed)
// GLUE_LOG=1 echoes the probe's log lines.

#include "gpu_queue_glue_support.h"

#include "gpu_queue_probe.h"
#include "gpu_queue_probe_test_seam.h"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using glue::Game;
using Record = PacingGpuQueueRecord;

namespace {

[[noreturn]] void Fail(int line, const char* expression, const char* message) {
    std::fprintf(stderr, "FAIL line %d: %s%s%s\n", line, expression, message[0] ? " -- " : "", message);
    glue::DumpLog(stderr, 40);
    std::fflush(stderr);
    std::_Exit(1);
}
void Check(bool ok, int line, const char* expression, const char* format = "", ...) {
    if (ok) return;
    char message[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    Fail(line, expression, message);
}
#define CHECK(value) Check(static_cast<bool>(value), __LINE__, #value)
#define CHECKM(value, ...) Check(static_cast<bool>(value), __LINE__, #value, __VA_ARGS__)

// The valid records of frames [lo, hi] (a record is valid when its submit count is known).
std::vector<Record> ValidIn(uint64_t lo, uint64_t hi) {
    std::vector<Record> out;
    for (const Record& r : glue::Records())
        if ((r.valid & Record::kSubmits) && r.present_sequence >= lo && r.present_sequence <= hi) out.push_back(r);
    return out;
}
void WaitRecords(uint64_t sequence, const char* what) {
    CHECKM(glue::WaitForRecordSequence(sequence, 10000), "no record reached the present sequence %llu (%s)",
        static_cast<unsigned long long>(sequence), what);
}
// The identity of a valid record is the frame's: generation, sequence, key, role.
void CheckIdentity(const Game& game, const Record& r) {
    CHECKM(r.swapchain_generation == game.generation, "record of frame %llu has swapchain generation %llu",
        static_cast<unsigned long long>(r.present_sequence), static_cast<unsigned long long>(r.swapchain_generation));
    CHECKM(r.key.frame_id == r.present_sequence, "record of frame %llu carries key frame id %llu",
        static_cast<unsigned long long>(r.present_sequence), static_cast<unsigned long long>(r.key.frame_id));
    CHECK(r.queue_role == 3);
    CHECK(r.session_generation != 0);
}

// ── 1/2/3: the borrowed steady state ──
void PhaseBorrowed(Game& game) {
    const uint64_t first = game.frame + 1;
    const uint64_t own0 = game.ProbeQueueSignals(), marker0 = game.marker->queue_signals.load();
    uint64_t submissions = 0;
    for (int i = 0; i < 80; ++i) {
        submissions += Game::Submissions(game.frame + 1);
        game.PlayFrame();
        Sleep(1);
    }
    const uint64_t last = game.frame;
    CHECKM(game.marker->queue_signals.load() - marker0 == submissions, "the game's Signals reached the queue %llu times for %llu submissions",
        static_cast<unsigned long long>(game.marker->queue_signals.load() - marker0), static_cast<unsigned long long>(submissions));
    CHECKM(game.ProbeQueueSignals() - own0 == 0, "the probe added %llu queue Signals while the game's own marked every submission",
        static_cast<unsigned long long>(game.ProbeQueueSignals() - own0));
    WaitRecords(last - 6, "borrowed steady state");

    const std::vector<Record> valid = ValidIn(first + 10, last - 6);
    const size_t frames = static_cast<size_t>((last - 6) - (first + 10) + 1);
    CHECKM(valid.size() * 2 >= frames, "only %zu of %zu steady frames came back valid", valid.size(), frames);
    size_t borrowed_all = 0, with_done = 0;
    for (const Record& r : valid) {
        CheckIdentity(game, r);
        CHECKM(r.submits == Game::Submissions(r.present_sequence), "frame %llu has %u submissions, the game made %u",
            static_cast<unsigned long long>(r.present_sequence), r.submits, Game::Submissions(r.present_sequence));
        if (r.borrowed_submits == r.submits) ++borrowed_all;
        if ((r.valid & Record::kLastDone) && r.last_done_qpc > r.first_submit_qpc) ++with_done;
    }
    CHECKM(borrowed_all * 2 >= valid.size() && borrowed_all > 0, "%zu of %zu valid steady frames were fully borrowed", borrowed_all, valid.size());
    CHECKM(with_done > 0, "no steady record carries a done time (the worker stamped nothing through the fake fences)");
    std::printf("ok: borrowed steady state: %zu/%zu frames valid, %zu fully borrowed, %zu with a done time; "
                "%llu game Signals forwarded, 0 probe Signals\n", valid.size(), frames, borrowed_all, with_done,
                static_cast<unsigned long long>(submissions));
}

// ── 4: Waits ──
void PhaseWaits(Game& game) {
    const uint64_t first = game.frame + 1;
    const uint64_t own0 = game.ProbeQueueSignals(), waits0 = game.render->waits.load(), reads0 = game.gate->reads.load();
    constexpr unsigned kWaitFrames = 8;
    for (unsigned f = 0; f < kWaitFrames; ++f) {
        const uint64_t sequence = game.frame + 1;
        for (unsigned i = 0, n = Game::Submissions(sequence); i < n; ++i) {
            game.Submit();
            if (i == 1) game.gate->Advance(game.WaitUnsatisfied()); // unsatisfied at the call, released right after it
        }
        game.BoundarySignal(sequence);
        game.PresentSide(sequence);
        game.frame = sequence;
        Sleep(1);
    }
    CHECKM(game.render->waits.load() - waits0 == kWaitFrames, "the render queue saw %llu Waits",
        static_cast<unsigned long long>(game.render->waits.load() - waits0));
    CHECKM(game.gate->reads.load() - reads0 >= kWaitFrames, "the Waits' fence was read %llu times",
        static_cast<unsigned long long>(game.gate->reads.load() - reads0));
    CHECKM(game.ProbeQueueSignals() - own0 == kWaitFrames, "%llu own probe Signals for %u unsatisfied Waits (one each, the submissions stay borrowed)",
        static_cast<unsigned long long>(game.ProbeQueueSignals() - own0), kWaitFrames);
    // Satisfied Waits: the fence value is reached at the call, nothing is signalled (inside a regular frame).
    const uint64_t own1 = game.ProbeQueueSignals();
    {
        const uint64_t sequence = game.frame + 1;
        for (unsigned i = 0, n = Game::Submissions(sequence); i < n; ++i) {
            game.Submit();
            glue::GameWait(game.render, game.gate, game.gate->Completed());
        }
        game.BoundarySignal(sequence);
        game.PresentSide(sequence);
        game.frame = sequence;
    }
    CHECKM(game.ProbeQueueSignals() - own1 == 0, "%llu probe Signals for satisfied Waits",
        static_cast<unsigned long long>(game.ProbeQueueSignals() - own1));
    const uint64_t last = game.frame;
    game.PlayFrame();
    Sleep(1);
    WaitRecords(last, "wait frames");
    for (const Record& r : ValidIn(first, last)) {
        CheckIdentity(game, r);
        CHECKM(r.submits == Game::Submissions(r.present_sequence), "wait frame %llu has %u submissions", static_cast<unsigned long long>(r.present_sequence), r.submits);
    }
    std::printf("ok: Waits: %u unsatisfied -> %u own Signals, 4 satisfied -> 0\n", kWaitFrames, kWaitFrames);
}

// ── 5: three threads ──
void PhaseRace(Game& game) {
    constexpr unsigned kFrames = 240;
    const uint64_t first = game.frame + 1, last = first + kFrames - 1;
    std::atomic<uint64_t> rendered{first - 1}, race{0}, ack{0};
    std::atomic<bool> stop{false};
    std::thread render([&] {
        for (uint64_t s = first; s <= last; ++s) {
            for (unsigned j = 0, n = Game::Submissions(s); j < n; ++j) game.Submit();
            const bool racing = (s % 10) == 0;
            if (racing) race.store(s, std::memory_order_release); // the second submitter starts racing the boundary now
            game.BoundarySignal(s);
            if (racing) while (ack.load(std::memory_order_acquire) != s) SwitchToThread(); // it is done before the next frame starts
            rendered.store(s, std::memory_order_release);
            Sleep(1);
        }
    });
    std::thread second([&] {
        uint64_t seen = 0;
        while (!stop.load(std::memory_order_acquire)) {
            const uint64_t s = race.load(std::memory_order_acquire);
            if (s == 0 || s == seen) { YieldProcessor(); continue; }
            seen = s;
            game.ExecuteOnly(1); // another thread's submission and Signal on the adopted queue
            glue::GameSignal(game.render, game.aux, game.aux_value.fetch_add(1, std::memory_order_relaxed) + 1);
            ack.store(s, std::memory_order_release);
        }
    });
    std::thread presenter([&] {
        for (uint64_t s = first; s <= last; ++s) {
            while (rendered.load(std::memory_order_acquire) < s) SwitchToThread();
            game.PresentSide(s); // the Present thread: concurrently with the render thread's next frame
        }
    });
    render.join();
    presenter.join();
    stop.store(true, std::memory_order_release);
    second.join();
    game.frame = last;
    WaitRecords(last - 6, "interleaved run");

    const std::vector<Record> all = glue::Records();
    std::map<uint64_t, Record> valid;
    for (const Record& r : all) {
        if (!(r.valid & Record::kSubmits) || r.present_sequence < first || r.present_sequence > last) continue;
        CheckIdentity(game, r);
        CHECKM(valid.find(r.present_sequence) == valid.end(), "two valid records for frame %llu", static_cast<unsigned long long>(r.present_sequence));
        const unsigned base = Game::Submissions(r.present_sequence);
        const bool contaminated = (r.present_sequence % 10) == 0 || (r.present_sequence % 10) == 1;
        // Only the frames the second submitter races (the boundary of s % 10 == 0 and the frame after) may hold its one
        // extra submission; every other valid frame holds exactly the render thread's.
        CHECKM(contaminated ? (r.submits == base || r.submits == base + 1) : r.submits == base,
            "frame %llu (racing %d): %u submissions, the render thread made %u", static_cast<unsigned long long>(r.present_sequence),
            contaminated ? 1 : 0, r.submits, base);
        valid[r.present_sequence] = r;
    }
    size_t pairs_checked = 0;
    for (uint64_t s = (first + 9) / 10 * 10; s + 1 <= last - 6; s += 10) {
        const auto a = valid.find(s), b = valid.find(s + 1);
        if (a == valid.end() || b == valid.end()) continue;
        ++pairs_checked;
        CHECKM(a->second.submits + b->second.submits == Game::Submissions(s) + Game::Submissions(s + 1) + 1,
            "frames %llu and %llu hold %u submissions together, expected %u (the racing submission is in one of them)",
            static_cast<unsigned long long>(s), static_cast<unsigned long long>(s + 1), a->second.submits + b->second.submits,
            Game::Submissions(s) + Game::Submissions(s + 1) + 1);
    }
    CHECKM(valid.size() * 4 >= kFrames, "only %zu of %u frames of the interleaved run came back valid", valid.size(), kFrames);
    std::printf("ok: interleaved run: %zu/%u frames valid, none misassigned or duplicated, %zu racing pairs conserved\n", valid.size(), kFrames,
        pairs_checked);
}

// ── 6: a foreign Signal of the bound fence ──
void PhaseForeign(Game& game) {
    CHECKM(game.WaitBorrowSteady(), "borrowing did not become steady again after the interleaved run");
    const long refs_bound = game.marker->refs.load();
    CHECKM(refs_bound == 2, "the game's fence holds %ld references while the probe binds it (the test's and the probe's one)", refs_bound);
    const uint64_t foreign_frame = game.frame;
    game.SignalForeign();
    CHECKM(game.marker->refs.load() == refs_bound, "the reference on the fence was released before the session ended");
    const uint64_t own0 = game.ProbeQueueSignals();
    uint64_t submissions = 0;
    for (int i = 0; i < 40; ++i) {
        submissions += Game::Submissions(game.frame + 1);
        game.PlayFrame();
        Sleep(1);
    }
    const uint64_t last = game.frame;
    CHECKM(game.ProbeQueueSignals() - own0 == submissions, "%llu probe Signals for %llu submissions after the foreign Signal (the probe marks each itself)",
        static_cast<unsigned long long>(game.ProbeQueueSignals() - own0), static_cast<unsigned long long>(submissions));
    WaitRecords(last - 6, "after the foreign Signal");
    const std::vector<Record> valid = ValidIn(foreign_frame + 4, last - 6);
    CHECKM(!valid.empty(), "no valid record after the foreign Signal");
    for (const Record& r : valid) {
        CheckIdentity(game, r);
        CHECKM(r.borrowed_submits == 0, "frame %llu after the foreign Signal still reports %u borrowed submissions",
            static_cast<unsigned long long>(r.present_sequence), r.borrowed_submits);
        CHECKM(r.submits == Game::Submissions(r.present_sequence), "frame %llu has %u submissions", static_cast<unsigned long long>(r.present_sequence), r.submits);
    }
    std::printf("ok: foreign Signal: borrowing off, %llu own Signals for %llu submissions, %zu valid records with none borrowed\n",
        static_cast<unsigned long long>(game.ProbeQueueSignals() - own0), static_cast<unsigned long long>(submissions), valid.size());
}

// ── 7: the end of the session, under load ──
void PhaseShutdown(Game& game) {
    std::atomic<bool> stop{false};
    std::thread load([&] {
        uint64_t s = game.frame;
        while (!stop.load(std::memory_order_acquire)) {
            for (unsigned i = 0, n = Game::Submissions(s + 1); i < n; ++i) game.Submit();
            game.BoundarySignal(++s);
            Sleep(1);
        }
        game.frame = s;
    });
    Sleep(30);
    const bool ended = GpuQueueProbe_Shutdown(8000);
    stop.store(true, std::memory_order_release);
    load.join();
    CHECK(ended);
    CHECK(!GpuQueueProbeTest_Running());
    CHECKM(game.marker->refs.load() == 1, "the game's fence still holds %ld references after the session", game.marker->refs.load());
    CHECKM(game.render->refs.load() == 1, "the render queue still holds %ld references after the session", game.render->refs.load());
    CHECKM(game.device->refs.load() == 1, "the device still holds %ld references after the session", game.device->refs.load());
    size_t probe_fences = 0;
    for (const glue::Fence* fence : game.device->fences) {
        if (!fence->from_com) continue;
        ++probe_fences;
        CHECKM(fence->refs.load() == 0, "the probe's fence '%s' is not released (%ld)", fence->name.c_str(), fence->refs.load());
    }
    CHECKM(probe_fences >= 2, "%zu probe fences were created (own and calibration expected)", probe_fences);
    std::printf("ok: Shutdown under load: session ended, game fence / queue / device / %zu probe fences released\n", probe_fences);
}

// ── A rundown that times out retains the bound game fence ──
// A thread is held inside the probe's callback across the native Wait of an unsatisfied Wait on the adopted queue (the fake
// queue's Wait blocks on an event the test releases after the session ended). The session's rundown (200 ms) times out:
// the references (the game's fence, the queue, the device, the probe's fences) must be retained, the session dead, and no
// bind may open afterwards.
void PhaseRundownTimeout(Game& game) {
    const long marker_refs = game.marker->refs.load(), queue_refs = game.render->refs.load(), device_refs = game.device->refs.load();
    CHECKM(marker_refs == 2, "the game's fence holds %ld references while the probe binds it", marker_refs);
    HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    CHECK(release != nullptr);
    game.render->wait_gate.store(release, std::memory_order_release);
    std::thread held([&] { game.WaitUnsatisfied(); }); // enters the probe's WaitBegin (admitted), then blocks in the native Wait
    for (int i = 0; i < 5000 && game.render->waits_blocked.load() == 0; ++i) Sleep(1);
    CHECKM(game.render->waits_blocked.load() == 1, "the held Wait never reached the fake queue's native Wait");
    const bool ended = GpuQueueProbe_Shutdown(10000);
    CHECKM(ended, "the session's thread did not end although its rundown is bounded");
    CHECK(GpuQueueProbeTest_Dead());
    CHECK(!GpuQueueProbeTest_Running());
    CHECK(GpuQueueProbeTest_AdoptedQueue() == nullptr);
    CHECKM(glue::LogContains("retained"), "the session did not report retaining its references");
    CHECKM(game.marker->refs.load() == marker_refs, "the bound game fence was released by a timed-out rundown (%ld references, %ld before)",
        game.marker->refs.load(), marker_refs);
    CHECKM(game.render->refs.load() == queue_refs, "the queue reference was released by a timed-out rundown");
    CHECKM(game.device->refs.load() == device_refs, "the device reference was released by a timed-out rundown");
    size_t probe_fences = 0;
    for (const glue::Fence* fence : game.device->fences) {
        if (!fence->from_com) continue;
        ++probe_fences;
        CHECKM(fence->refs.load() == 1, "the probe's fence '%s' was released by a timed-out rundown (%ld)", fence->name.c_str(), fence->refs.load());
    }
    CHECK(probe_fences >= 2);
    // The held hook returns after the session ended.
    game.render->wait_gate.store(nullptr, std::memory_order_release);
    SetEvent(release);
    held.join();
    game.gate->Advance(game.gate_value.load()); // the queue passes the Wait
    CHECKM(game.marker->refs.load() == marker_refs, "the game's fence was released when the held hook returned");
    // No bind opens on a dead session: the game's Signals still reach the queue, the probe adds nothing and records nothing.
    const uint64_t own1 = game.ProbeQueueSignals(), marker1 = game.marker->queue_signals.load();
    const size_t records1 = glue::Records().size();
    uint64_t submissions = 0;
    for (int i = 0; i < 20; ++i) {
        submissions += Game::Submissions(game.frame + 1);
        game.PlayFrame();
        Sleep(1);
    }
    CHECKM(game.marker->queue_signals.load() - marker1 == submissions, "the game's Signals did not all reach the queue after the dead session");
    CHECKM(game.ProbeQueueSignals() == own1, "the dead session added %llu probe Signals", static_cast<unsigned long long>(game.ProbeQueueSignals() - own1));
    CHECKM(glue::Records().size() == records1, "the dead session produced %zu new records", glue::Records().size() - records1);
    CHECK(GpuQueueProbeTest_Dead());
    CHECK(!GpuQueueProbeTest_AdoptRenderQueue(static_cast<void*>(glue::Com(game.render)),
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(glue::Com(game.dependency))), game.generation, game.frame + 1));
    CHECKM(game.marker->refs.load() == marker_refs, "the game's fence reference changed after the dead session");
    std::printf("ok: timed-out rundown: session dead, game fence / queue / device / %zu probe fences retained, no bind or record afterwards\n", probe_fences);
}

// ── A borrowed marker whose value the game fence had already reached ──
// The game CPU-signals its fence (ID3D12Fence::Signal, not the intercepted queue Signal) to the value of its next queue
// Signal before the Execute: the Execute-enter observation already covers the marker's value, so it was not completed by this
// submission's work: the marker must not be bound as borrowed and borrowing ends for the session.
void PhaseCoveredBind(Game& game) {
    const uint64_t own0 = game.ProbeQueueSignals();
    const uint64_t injected = game.frame + 1;
    const unsigned n = Game::Submissions(injected);
    for (unsigned i = 0; i < n; ++i) {
        if (i == 1) {
            const uint64_t value = game.marker_value.fetch_add(1, std::memory_order_relaxed) + 1;
            CHECK(SUCCEEDED(glue::Com(game.marker)->Signal(value))); // the CPU Signal: the fence stands at the value
            glue::GameExecute(game.render, 2);                        // Execute enter reads completed >= value
            glue::GameSignal(game.render, game.marker, value);        // the queue Signal of that value
        } else {
            game.Submit();
        }
    }
    game.BoundarySignal(injected);
    game.PresentSide(injected);
    game.frame = injected;
    Sleep(1);
    // The injected marker is bound (pushed borrowed, then untrusted by the epoch and the core) and borrowing ends at once, so the
    // probe adds no Signal for it; every LATER submission of the frame is marked by the probe's own Signal.
    CHECKM(game.ProbeQueueSignals() - own0 == n - 2, "%llu own probe Signals in the injected frame of %u submissions (expected %u: every one after the "
        "injected one)", static_cast<unsigned long long>(game.ProbeQueueSignals() - own0), n, n - 2);
    CHECKM(glue::LogContains("off for this session"), "borrowing did not end for the session");
    const uint64_t own1 = game.ProbeQueueSignals();
    uint64_t submissions = 0;
    for (int i = 0; i < 40; ++i) {
        submissions += Game::Submissions(game.frame + 1);
        game.PlayFrame();
        Sleep(1);
    }
    const uint64_t last = game.frame;
    CHECKM(game.ProbeQueueSignals() - own1 == submissions, "%llu own probe Signals for %llu later submissions (no bind may open again)",
        static_cast<unsigned long long>(game.ProbeQueueSignals() - own1), static_cast<unsigned long long>(submissions));
    WaitRecords(last - 6, "after the covered bind");
    // The injected frame carries no valid record (its marker is untrusted: the frame is invalidated), and no later valid frame
    // reports a borrowed submission.
    for (const Record& r : glue::Records()) {
        if (r.present_sequence == injected)
            CHECKM(!(r.valid & Record::kSubmits), "the injected frame %llu came back valid (a marker with pre-issue completion evidence was trusted)",
                static_cast<unsigned long long>(injected));
    }
    const std::vector<Record> later = ValidIn(injected + 4, last - 6);
    CHECKM(!later.empty(), "no valid record after the covered bind");
    for (const Record& r : later) {
        CheckIdentity(game, r);
        CHECKM(r.borrowed_submits == 0, "frame %llu after the covered bind reports %u borrowed submissions",
            static_cast<unsigned long long>(r.present_sequence), r.borrowed_submits);
        CHECKM(r.submits == Game::Submissions(r.present_sequence), "frame %llu has %u submissions", static_cast<unsigned long long>(r.present_sequence), r.submits);
    }
    std::printf("ok: covered bind: injected frame %llu untrusted, borrowing off, %zu later valid records with none borrowed\n",
        static_cast<unsigned long long>(injected), later.size());
}

// ── The dependency consumers never record a zero time; the clock is read once per hook, and only when consumed ──
int64_t ClockNow() {
    LARGE_INTEGER q{};
    QueryPerformanceCounter(&q);
    return q.QuadPart;
}
void PhaseDependencyClock(Game& game) {
    void* const queue = static_cast<void*>(glue::Com(game.spare));
    void* const fence = static_cast<void*>(glue::Com(game.aux));
    void* const wait_fence = static_cast<void*>(glue::Com(game.gate));
    CHECK(g_gpu_queue_probe.load() && g_gpu_queue_probe_source.load() != 0);
    constexpr uint64_t kBase = 0x70000000ull;
    int64_t recorded = 0;

    // The production consumers called with now == 0 (a key published between the hook's gate and the consumer): their own read.
    int64_t t0 = ClockNow();
    GpuQueueProbe_DependencySignalActive(queue, fence, kBase + 1, 0);
    int64_t t1 = ClockNow();
    CHECK(GpuQueueProbeTest_DependencyEntryQpc(true, queue, fence, kBase + 1, &recorded));
    CHECKM(recorded != 0 && recorded >= t0 && recorded <= t1, "Signal recorded with now == 0 got time %lld, expected within [%lld, %lld]",
        static_cast<long long>(recorded), static_cast<long long>(t0), static_cast<long long>(t1));
    t0 = ClockNow();
    GpuQueueProbe_DependencyWaitActive(queue, wait_fence, kBase + 2, 0);
    t1 = ClockNow();
    CHECK(GpuQueueProbeTest_DependencyEntryQpc(false, queue, wait_fence, kBase + 2, &recorded));
    CHECKM(recorded != 0 && recorded >= t0 && recorded <= t1, "Wait recorded with now == 0 got time %lld, expected within [%lld, %lld]",
        static_cast<long long>(recorded), static_cast<long long>(t0), static_cast<long long>(t1));
    // A time the hook did read is recorded as it is (the one clock read of the entry is shared).
    const int64_t given = ClockNow();
    GpuQueueProbe_DependencySignalActive(queue, fence, kBase + 3, given);
    GpuQueueProbe_DependencyWaitActive(queue, wait_fence, kBase + 4, given);
    CHECK(GpuQueueProbeTest_DependencyEntryQpc(true, queue, fence, kBase + 3, &recorded) && recorded == given);
    CHECK(GpuQueueProbeTest_DependencyEntryQpc(false, queue, wait_fence, kBase + 4, &recorded) && recorded == given);

    // Through the detours, per key state: the clock reads of the calling thread per Signal and per Wait (a Wait of a value the
    // fence already stands at: nothing queued), and the recorded time of each entry.
    struct KeyState { bool probe; int source; unsigned reads; const char* name; };
    const KeyState states[] = {{true, 1, 1, "both keys on"}, {false, 1, 0, "probe off, source on"},
        {true, 0, 0, "probe on, source off"}, {false, 0, 0, "both off"}};
    uint64_t value = kBase + 100;
    for (const KeyState& s : states) {
        g_gpu_queue_probe.store(s.probe, std::memory_order_relaxed);
        g_gpu_queue_probe_source.store(s.source, std::memory_order_relaxed);
        const uint64_t signal_value = ++value, wait_value = ++value;
        const uint64_t reads0 = glue::g_thread.qpc_reads;
        t0 = ClockNow();
        glue::GameSignal(game.spare, game.aux, signal_value);
        t1 = ClockNow();
        const uint64_t signal_reads = glue::g_thread.qpc_reads - reads0;
        const uint64_t reads1 = glue::g_thread.qpc_reads;
        glue::GameWait(game.spare, game.gate, 0);
        const uint64_t wait_reads = glue::g_thread.qpc_reads - reads1;
        (void)wait_value;
        CHECKM(signal_reads == s.reads, "%s: %llu clock reads for one Signal through the detour, expected %u", s.name,
            static_cast<unsigned long long>(signal_reads), s.reads);
        CHECKM(wait_reads == s.reads, "%s: %llu clock reads for one Wait through the detour, expected %u", s.name,
            static_cast<unsigned long long>(wait_reads), s.reads);
        if (s.reads) {
            CHECK(GpuQueueProbeTest_DependencyEntryQpc(true, queue, fence, signal_value, &recorded));
            CHECKM(recorded != 0 && recorded >= t0 && recorded <= t1, "%s: the detour's Signal entry time %lld is outside [%lld, %lld]", s.name,
                static_cast<long long>(recorded), static_cast<long long>(t0), static_cast<long long>(t1));
        } else {
            CHECKM(!GpuQueueProbeTest_DependencyEntryQpc(true, queue, fence, signal_value, &recorded), "%s: an entry was recorded with a key off", s.name);
        }
    }
    g_gpu_queue_probe.store(true, std::memory_order_relaxed);
    g_gpu_queue_probe_source.store(1, std::memory_order_relaxed);
    std::printf("ok: dependency clock: now == 0 never records a zero time (Signal and Wait), a given time is kept, 1 read per hook with both keys "
                "on and 0 with either off (counted: %s)\n", glue::g_thread.qpc_reads ? "yes" : "NO READS COUNTED, force-include inactive");
}

// ── The slot-ownership family: a thread held at a pause point of the probe, observers of every claim and every pushed Submit ──
// The seam (GpuQueueProbeTest_SetBindPause) calls BindPause on the calling thread at point 0 (the binder before the take of the
// pending word), 1 (after a successful take), 2 (after the slot's value is published), 3 (before its Submit is pushed), 4 (the
// claim, before it releases the Signal lock), 5 / 6 (the lock-timeout taker after its Submit is built / after Lost is published),
// 7 (the worker's tail read after loading the cursor and `issued`), 8 (after the tag of a slot above the cursor). Several points may be
// armed; a point holds the one armed thread (thread 0: the first to reach it) once, on an event the test releases.
constexpr unsigned kPoints = 9;
struct PausePoint {
    std::atomic<bool> armed{false}, hit{false};
    std::atomic<DWORD> thread{0};
    std::atomic<uint64_t> k{0};
    HANDLE release = nullptr;
};
PausePoint g_points[kPoints];
// Point 8 is watched, not armed: a hit with a k at or below the consumed cursor is the OLD ReadTail reading a consumed slot.
std::atomic<bool> g_watch8{false};
std::atomic<uint64_t> g_consumed_k{0}, g_stale8{0}, g_stale8_k{0};
// A pause hold that EXPIRES lets the held thread run on unobserved: it is recorded here and fails the test (at the next release of
// any point and at the end of the run), never counted as a pause that worked.
constexpr DWORD kHoldMs = 30000;
std::atomic<uint64_t> g_hold_expired{0};
std::atomic<unsigned> g_hold_expired_point{0};
void HoldOn(HANDLE release, unsigned point) {
    if (WaitForSingleObject(release, kHoldMs) != WAIT_OBJECT_0) {
        g_hold_expired_point.store(point, std::memory_order_release);
        g_hold_expired.fetch_add(1, std::memory_order_acq_rel);
    }
}
void CheckHoldsIntact(const char* where) {
    CHECKM(g_hold_expired.load(std::memory_order_acquire) == 0, "a pause hold expired (%llu, first at point %u) before the test released it (%s): the held thread ran on unobserved",
        static_cast<unsigned long long>(g_hold_expired.load()), g_hold_expired_point.load(), where);
}
void BindPause(unsigned point, uint64_t k) {
    if (point >= kPoints) return;
    if (point == 8 && g_watch8.load(std::memory_order_acquire) && k <= g_consumed_k.load(std::memory_order_acquire)) {
        g_stale8_k.store(k, std::memory_order_release);
        g_stale8.fetch_add(1, std::memory_order_acq_rel);
        HoldOn(g_points[8].release, 8); // hold it there: what the old code would then read is a replacement
        return;
    }
    PausePoint& p = g_points[point];
    if (!p.armed.load(std::memory_order_acquire)) return;
    const DWORD want = p.thread.load(std::memory_order_acquire);
    if (want != 0 && want != GetCurrentThreadId()) return;
    if (!p.armed.exchange(false)) return;
    p.k.store(k, std::memory_order_release);
    p.hit.store(true, std::memory_order_release);
    HoldOn(p.release, point);
}
void ArmPoint(unsigned point, DWORD thread) {
    g_points[point].hit.store(false, std::memory_order_release);
    g_points[point].thread.store(thread, std::memory_order_release);
    g_points[point].armed.store(true, std::memory_order_release);
}
void WaitPoint(unsigned point, const char* what) {
    for (int i = 0; i < 5000 && !g_points[point].hit.load(std::memory_order_acquire); ++i) Sleep(1);
    CHECKM(g_points[point].hit.load(std::memory_order_acquire), "no thread reached pause point %u (%s)", point, what);
}
void ReleasePoint(unsigned point) {
    CheckHoldsIntact("at the release of a pause point"); // an expired hold means the thread was NOT held until here
    SetEvent(g_points[point].release);
}

struct ClaimRec { uint64_t k; GpuQueueEvent event; uint64_t epoch; };
struct PushRec { unsigned taker; GpuQueueEvent event; };
std::mutex g_obs_lock;
std::vector<ClaimRec> g_claims;
std::vector<PushRec> g_pushes;
std::vector<std::pair<uint64_t, uint64_t>> g_arms;
std::atomic<bool> g_arms_on{false};
void OnClaim(uint64_t k, const GpuQueueEvent& event, uint64_t epoch) {
    std::lock_guard<std::mutex> guard(g_obs_lock);
    g_claims.push_back({k, event, epoch});
}
void OnPush(unsigned taker, const GpuQueueEvent& submit) {
    std::lock_guard<std::mutex> guard(g_obs_lock);
    g_pushes.push_back({taker, submit});
}
void OnArm(uint64_t own_k, uint64_t game_k) {
    if (!g_arms_on.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> guard(g_obs_lock);
    g_arms.emplace_back(own_k, game_k);
}
void InstallPause() {
    for (PausePoint& p : g_points) {
        p.release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        CHECK(p.release != nullptr);
    }
    GpuQueueProbeTest_SetBindPause(&BindPause);
    GpuQueueProbeTest_SetPushObserver(&OnPush);
    GpuQueueProbeTest_SetClaimObserver(&OnClaim);
    GpuQueueProbeTest_SetArmObserver(&OnArm);
}
bool FindClaim(uint64_t k, ClaimRec* out) {
    std::lock_guard<std::mutex> guard(g_obs_lock);
    for (size_t i = g_claims.size(); i-- > 0;) if (g_claims[i].k == k) { *out = g_claims[i]; return true; }
    return false;
}
uint64_t MaxClaimK() {
    std::lock_guard<std::mutex> guard(g_obs_lock);
    uint64_t k = 0;
    for (const ClaimRec& c : g_claims) k = (std::max)(k, c.k);
    return k;
}
// The Submits a taker pushed (taker < 0: any) whose done value is `done`.
std::vector<PushRec> PushesWithDone(uint64_t done, int taker) {
    std::lock_guard<std::mutex> guard(g_obs_lock);
    std::vector<PushRec> out;
    for (const PushRec& p : g_pushes) if (p.event.done_value == done && (taker < 0 || p.taker == static_cast<unsigned>(taker))) out.push_back(p);
    return out;
}
size_t PushesBy(unsigned taker) {
    std::lock_guard<std::mutex> guard(g_obs_lock);
    size_t n = 0;
    for (const PushRec& p : g_pushes) if (p.taker == taker) ++n;
    return n;
}
// A pushed Submit carries the identity of the claim it was taken from: the call order, the Execute's enqueue time and the saved
// observation (and, for a borrowed marker, the claim epoch).
void CheckSameIdentity(const GpuQueueEvent& pushed, const ClaimRec& claim, const char* who, bool epoch_carried) {
    CHECKM(pushed.index == claim.event.index, "%s: Submit index %llu, the claim of k = %llu had %llu", who, static_cast<unsigned long long>(pushed.index),
        static_cast<unsigned long long>(claim.k), static_cast<unsigned long long>(claim.event.index));
    CHECKM(pushed.enq_qpc == claim.event.enq_qpc, "%s: Submit enq %lld, the claim's %lld", who, static_cast<long long>(pushed.enq_qpc),
        static_cast<long long>(claim.event.enq_qpc));
    CHECKM(pushed.obs_valid == claim.event.obs_valid && pushed.obs_game_completed == claim.event.obs_game_completed &&
        pushed.obs_own_completed == claim.event.obs_own_completed && pushed.obs_before_qpc == claim.event.obs_before_qpc,
        "%s: Submit observation (%u, game %llu, own %llu, before %lld) differs from the claim's (%u, %llu, %llu, %lld)", who, pushed.obs_valid,
        static_cast<unsigned long long>(pushed.obs_game_completed), static_cast<unsigned long long>(pushed.obs_own_completed),
        static_cast<long long>(pushed.obs_before_qpc), claim.event.obs_valid, static_cast<unsigned long long>(claim.event.obs_game_completed),
        static_cast<unsigned long long>(claim.event.obs_own_completed), static_cast<long long>(claim.event.obs_before_qpc));
    if (epoch_carried)
        CHECKM(pushed.claim_epoch == claim.epoch, "%s: Submit claim epoch %llu, the claim's %llu", who, static_cast<unsigned long long>(pushed.claim_epoch),
            static_cast<unsigned long long>(claim.epoch));
}

// (a) The lost-take schedule. Binder A (the game's Signal of the marker of submission k) is held BEFORE the take of the
// pending word. Another thread's Execute on the adopted queue repairs the pending word (the marker is settled with an own
// Signal), and the traffic goes on until the worker has consumed k and the slot of k has been claimed again (k + 256: every
// submission after the repair claims a new value). A is released: its take fails, so it must publish no marker, push no Submit
// and read or write no slot: the only Submit for k is the repair's, with the claim's identity; the frame in progress at the
// release holds exactly its own submissions, nothing is untrusted by A, borrowing is unchanged and the session healthy.
// `covered_obs`: the same with A's marker value already reached on the game fence by a CPU ID3D12Fence::Signal before its Execute (the saved
// observation covers it): a binder that read the slot before it owned it would act on that read although its take is lost and end borrowing
// through the foreign-Signal path; the winner of the word only does, so here A, the loser, must not.
void PhaseBindRaceTakeLost(Game& game, bool covered_obs) {
    InstallPause();
    // Every Signal the game sends on its marker fence takes its value from marker_value (Submit / A's Signal), so the count the
    // game really sent is the counter's advance: WaitBorrowSteady and the repair frames play submissions this phase does not tally.
    if (covered_obs) Sleep(10); // the queue's earlier Signals have completed: the CPU Signal below is the highest value the fence stands at
    const uint64_t own0 = game.ProbeQueueSignals(), marker0 = game.marker->queue_signals.load(), value0 = game.marker_value.load();
    uint64_t submissions = 1; // A's
    std::thread binder([&] {
        if (covered_obs) {
            const uint64_t value = game.marker_value.fetch_add(1, std::memory_order_relaxed) + 1;
            CHECK(SUCCEEDED(glue::Com(game.marker)->Signal(value))); // the fence stands at the value before the Execute
            ArmPoint(0, GetCurrentThreadId());
            glue::GameExecute(game.render, 2);                       // claims k borrowed; its enter observation covers `value`
            glue::GameSignal(game.render, game.marker, value);       // the bind is held before its take
        } else {
            ArmPoint(0, GetCurrentThreadId());
            game.Submit(); // the Execute claims k; the Signal's bind is held before its take
        }
    });
    WaitPoint(0, "the binder before the take");
    const uint64_t k = g_points[0].k.load();
    CHECK(k != 0);
    game.ExecuteOnly(2); // thread B: the Execute on the adopted queue repairs A's pending word
    CHECKM(game.ProbeQueueSignals() - own0 >= 1, "the repair of A's pending marker issued no own Signal");
    // The lap: well over kSlots (256) further claims, with the worker consuming k on the way.
    while (submissions < 700) {
        submissions += Game::Submissions(game.frame + 1);
        game.PlayFrame();
        Sleep(1);
    }
    CHECKM(game.WaitBorrowSteady(), "borrowing did not become steady again after the repair");
    Sleep(100); // the worker consumes what is outstanding
    ClaimRec claim{}, replacement{};
    CHECKM(FindClaim(k, &claim), "the claim of k = %llu was not observed", static_cast<unsigned long long>(k));
    if (covered_obs)
        CHECKM(claim.event.obs_valid != 0 && claim.event.obs_game_completed > value0, "A's claim observation (game %llu) does not cover its value %llu: the schedule is not the covered one",
            static_cast<unsigned long long>(claim.event.obs_game_completed), static_cast<unsigned long long>(value0 + 1));
    CHECKM(FindClaim(k + 256, &replacement), "the slot of k = %llu was never claimed again (k + 256): the lap did not happen", static_cast<unsigned long long>(k));
    const uint64_t untrusted0 = GpuQueueProbeTest_MarkersUntrusted(), own1 = game.ProbeQueueSignals();
    CHECKM(!glue::LogContains("off for this session"), "borrowing was disabled before A was released");
    ReleasePoint(0);
    binder.join();
    GpuQueueProbeTest_SetBindPause(nullptr);
    // The frame in progress at the release: all of it is submitted after A's take failed.
    const uint64_t x = game.frame + 1;
    const unsigned m = Game::Submissions(x);
    submissions += m;
    game.PlayFrame();
    Sleep(1);
    uint64_t later = 0;
    for (int i = 0; i < 40; ++i) {
        later += Game::Submissions(game.frame + 1);
        game.PlayFrame();
        Sleep(1);
    }
    submissions += later;
    const uint64_t last = game.frame;
    WaitRecords(last - 6, "after the lost take");
    // The observers: the lost binder pushed NOTHING (no bind Submit for k, none with its claim's call order); the only Submit for k is
    // the repair's (taker 2), carrying the claim's identity, not the replacement's.
    CHECKM(PushesWithDone(k, 0).empty(), "the lost binder pushed %zu Submit(s) for k = %llu", PushesWithDone(k, 0).size(), static_cast<unsigned long long>(k));
    {
        std::lock_guard<std::mutex> guard(g_obs_lock);
        for (const PushRec& p : g_pushes)
            CHECKM(!(p.taker == 0 && p.event.index == claim.event.index), "a bind pushed a Submit with the lost claim's call order %llu",
                static_cast<unsigned long long>(claim.event.index));
    }
    const std::vector<PushRec> repairs = PushesWithDone(k, 2);
    CHECKM(repairs.size() == 1, "%zu repair Submits for k = %llu, expected exactly one", repairs.size(), static_cast<unsigned long long>(k));
    CheckSameIdentity(repairs[0].event, claim, "the repair of the lost-take marker", false);
    CHECKM(repairs[0].event.index != replacement.event.index, "the repair's Submit carries the replacement's call order");
    const uint64_t sent = game.marker_value.load() - value0;
    CHECKM(game.marker->queue_signals.load() - marker0 == sent, "the game's Signals reached the queue %llu times for %llu sent (A's included)",
        static_cast<unsigned long long>(game.marker->queue_signals.load() - marker0), static_cast<unsigned long long>(sent));
    CHECKM(game.ProbeQueueSignals() == own1, "the probe added %llu Signals after A's release (borrowing was disturbed)",
        static_cast<unsigned long long>(game.ProbeQueueSignals() - own1));
    CHECKM(!glue::LogContains("off for this session"), "A's lost take ended borrowing");
    CHECKM(GpuQueueProbeTest_MarkersUntrusted() == untrusted0, "A's lost take made %llu markers untrusted",
        static_cast<unsigned long long>(GpuQueueProbeTest_MarkersUntrusted() - untrusted0));
    CHECK(GpuQueueProbeTest_Running());
    bool found = false;
    for (const Record& r : glue::Records()) {
        if (r.present_sequence != x) continue;
        found = true;
        // Every Submit of the frame in progress at the release is borrowed and the game's own: a Submit pushed by A would add one.
        CHECKM(r.borrowed_submits == m, "frame %llu (the one in progress at A's release) holds %u borrowed Submits, the game made %u: a stray Submit",
            static_cast<unsigned long long>(x), r.borrowed_submits, m);
    }
    CHECKM(found, "no record for the frame in progress at A's release");
    const std::vector<Record> valid = ValidIn(x + 3, last - 6);
    CHECKM(!valid.empty(), "no valid record after A's release");
    for (const Record& r : valid) {
        CheckIdentity(game, r);
        CHECKM(r.submits == Game::Submissions(r.present_sequence), "frame %llu has %u submissions", static_cast<unsigned long long>(r.present_sequence), r.submits);
        CHECKM(r.borrowed_submits == r.submits, "frame %llu has %u borrowed of %u", static_cast<unsigned long long>(r.present_sequence), r.borrowed_submits, r.submits);
    }
    std::printf("ok: lost take%s: A released after the repair and %llu further submissions (k = %llu): no Submit from A, the repair's Submit is the claim's, no untrusted "
                "marker, borrowing intact, %zu later valid frames exact\n", covered_obs ? " (A's value covered by a CPU Signal)" : "", static_cast<unsigned long long>(submissions - 1),
                static_cast<unsigned long long>(k), valid.size());
}

// (b) The covered-binder schedule. A successful COVERED binder (its Execute-enter observation saw the game fence already at the
// value: a CPU ID3D12Fence::Signal) is held after the slot's value is published (point 2) or before its Submit is pushed
// (point 3). The value is complete, so the worker consumes k; another thread (B) then claims k + 256 on the same slot with
// the gate still open (no frame boundary meanwhile, so the missing pattern marker of A does not close it) and its own game
// Signals. A is released: its Submit was built before the slot's value was published, so the original submission's own event
// (carrying the covered observation) reaches the core: exactly one bind Submit for k, with the claim's identity (call order, enqueue,
// observation, claim epoch) and the game's value, none carrying the replacement's; the marker is untrusted, borrowing ends, the
// frame holds one borrowed Submit for A and one for each of B's.
void PhaseBindRaceCovered(Game& game, unsigned point) {
    InstallPause();
    const uint64_t own0 = game.ProbeQueueSignals();
    const uint64_t s = game.frame + 1; // the frame A and B submit in; no boundary until A is released
    Sleep(10); // the queue's earlier Signals have completed: the CPU Signal below is the highest value the fence stands at
    std::atomic<uint64_t> covered_value{0};
    std::thread binder([&] {
        const uint64_t value = game.marker_value.fetch_add(1, std::memory_order_relaxed) + 1;
        covered_value.store(value);
        CHECK(SUCCEEDED(glue::Com(game.marker)->Signal(value))); // the fence stands at the value before the Execute
        ArmPoint(point, GetCurrentThreadId());
        glue::GameExecute(game.render, 2);                       // claims k borrowed; its enter observation covers `value`
        glue::GameSignal(game.render, game.marker, value);       // the bind wins the word, publishes the value, and is held
    });
    WaitPoint(point, point == 2 ? "after the value is published" : "before the Submit is pushed");
    const uint64_t k = g_points[point].k.load();
    constexpr unsigned kLap = 320; // more than kSlots (256) further claims on the same slot ring
    for (unsigned i = 0; i < kLap; ++i) {
        game.Submit();
        if (i % 24 == 23) Sleep(2); // the worker consumes: a claim more than 256 ahead of its cursor is refused
    }
    CHECKM(game.ProbeQueueSignals() - own0 == 0, "the gate closed during the lap (%llu own Signals): the schedule did not reach k + 256 with borrowing open",
        static_cast<unsigned long long>(game.ProbeQueueSignals() - own0));
    ClaimRec claim{}, replacement{};
    CHECKM(FindClaim(k, &claim), "the claim of k = %llu was not observed", static_cast<unsigned long long>(k));
    CHECKM(FindClaim(k + 256, &replacement), "the slot of k = %llu was never claimed again (k + 256)", static_cast<unsigned long long>(k));
    CHECKM(PushesWithDone(k, 0).empty(), "A's Submit was pushed while it was held");
    Sleep(100);
    const uint64_t untrusted0 = GpuQueueProbeTest_MarkersUntrusted();
    ReleasePoint(point);
    binder.join();
    GpuQueueProbeTest_SetBindPause(nullptr);
    game.BoundarySignal(s);
    game.PresentSide(s);
    game.frame = s;
    // The observers: exactly ONE bind Submit for k, the claim's identity and the game's value; the replacement's own Submit is its own.
    const std::vector<PushRec> a_pushes = PushesWithDone(k, 0);
    CHECKM(a_pushes.size() == 1, "%zu bind Submits for k = %llu, expected exactly one", a_pushes.size(), static_cast<unsigned long long>(k));
    const GpuQueueEvent& pushed = a_pushes[0].event;
    CheckSameIdentity(pushed, claim, "the covered binder", true);
    CHECKM(pushed.marker_game == 1 && pushed.marker_value == covered_value.load() && pushed.borrowed == 1,
        "the covered binder's Submit: marker_game %u, marker_value %llu (the game's value %llu), borrowed %u", pushed.marker_game,
        static_cast<unsigned long long>(pushed.marker_value), static_cast<unsigned long long>(covered_value.load()), pushed.borrowed);
    CHECKM(pushed.index != replacement.event.index && pushed.enq_qpc != replacement.event.enq_qpc,
        "the covered binder's Submit carries the replacement's identity (call order %llu)", static_cast<unsigned long long>(pushed.index));
    const std::vector<PushRec> b_pushes = PushesWithDone(k + 256, 0);
    CHECKM(b_pushes.size() == 1 && b_pushes[0].event.index == replacement.event.index,
        "%zu bind Submits for the replacement k + 256, or not its own call order", b_pushes.size());
    for (int i = 0; i < 600 && GpuQueueProbeTest_MarkersUntrusted() <= untrusted0; ++i) Sleep(5);
    CHECKM(GpuQueueProbeTest_MarkersUntrusted() > untrusted0, "the core never made A's marker untrusted: its original Submit (the covered enter observation) did not arrive");
    CHECKM(glue::LogContains("off for this session"), "borrowing did not end after the covered bind");
    CHECK(GpuQueueProbeTest_Running());
    const uint64_t own1 = game.ProbeQueueSignals();
    uint64_t submissions = 0;
    for (int i = 0; i < 40; ++i) {
        submissions += Game::Submissions(game.frame + 1);
        game.PlayFrame();
        Sleep(1);
    }
    const uint64_t last = game.frame;
    CHECKM(game.ProbeQueueSignals() - own1 == submissions, "%llu own Signals for %llu later submissions", static_cast<unsigned long long>(game.ProbeQueueSignals() - own1),
        static_cast<unsigned long long>(submissions));
    WaitRecords(last - 6, "after the held covered bind");
    bool found = false;
    for (const Record& r : glue::Records()) {
        if (r.present_sequence != s) continue;
        found = true;
        CHECKM(!(r.valid & Record::kSubmits), "frame %llu with the covered submission came back valid", static_cast<unsigned long long>(s));
        // A's Submit (its own event, borrowed) and B's kLap Submits: none lost, none replaced by a copy of a lapped slot.
        CHECKM(r.borrowed_submits == 1 + kLap, "frame %llu holds %u borrowed Submits, expected %u (A's and B's %u)", static_cast<unsigned long long>(s),
            r.borrowed_submits, 1 + kLap, kLap);
    }
    CHECKM(found, "no record for the frame of the held covered bind");
    const std::vector<Record> later = ValidIn(s + 4, last - 6);
    CHECKM(!later.empty(), "no valid record after the held covered bind");
    for (const Record& r : later) {
        CheckIdentity(game, r);
        CHECKM(r.borrowed_submits == 0, "frame %llu after the held covered bind reports %u borrowed submissions", static_cast<unsigned long long>(r.present_sequence),
            r.borrowed_submits);
        CHECKM(r.submits == Game::Submissions(r.present_sequence), "frame %llu has %u submissions", static_cast<unsigned long long>(r.present_sequence), r.submits);
    }
    std::printf("ok: held covered bind (point %u, k = %llu): A's one Submit has the claim's identity after the slot lap (marker untrusted, %u borrowed Submits in its "
                "frame), borrowing ended, %zu later valid frames with none borrowed\n", point, static_cast<unsigned long long>(k), 1 + kLap, later.size());
}

// The lock-timeout taker. Producer C is held in the claim of k (pending word published, Signal lock held, point 4). Thread A
// (an interrupting Wait on the adopted queue: RepairPending) times out on the Signal lock, wins the word and runs LoseWord; it is held
// at point 6 (Lost published, its Submit already built into a local). C finishes and forwards its Signal; the worker consumes Lost
// and more than 256 further pairs are issued, so the slot of k is claimed again (k + 256) while A is held. A is released: the Submit
// it pushes (taker 1) is the ORIGINAL claim's (call order, enqueue, saved observation), a no-done untrusted invalid one, and A pushed
// no second Submit carrying the replacement's identity.
void PhaseLoseWordLap(Game& game) {
    InstallPause();
    const uint64_t own0 = game.ProbeQueueSignals();
    std::thread producer([&] {
        ArmPoint(4, GetCurrentThreadId());
        game.Submit(); // the Execute's leave claims k and is held with the Signal lock
    });
    WaitPoint(4, "the claim holding the Signal lock");
    const uint64_t k = g_points[4].k.load();
    std::thread interrupter([&] {
        ArmPoint(6, GetCurrentThreadId());
        game.gate->Advance(game.WaitUnsatisfied()); // WaitBegin: RepairPending: the lock times out, the word is lost
    });
    WaitPoint(6, "the lock-timeout taker after Lost is published");
    CHECKM(g_points[6].k.load() == k, "the lock-timeout taker lost k = %llu, the claim was %llu", static_cast<unsigned long long>(g_points[6].k.load()),
        static_cast<unsigned long long>(k));
    ReleasePoint(4); // C resumes: unlocks, finishes its Execute, forwards its game Signal
    producer.join();
    constexpr unsigned kLap = 300; // more than kSlots (256) further claims: the slot of k is claimed again while A is held
    for (unsigned i = 0; i < kLap; ++i) {
        game.Submit();
        if (i % 24 == 23) Sleep(2);
    }
    ClaimRec claim{}, replacement{};
    CHECKM(FindClaim(k, &claim), "the claim of k = %llu was not observed", static_cast<unsigned long long>(k));
    CHECKM(FindClaim(k + 256, &replacement), "the slot of k = %llu was never claimed again (k + 256): the lap did not happen while A was held", static_cast<unsigned long long>(k));
    CHECKM(game.ProbeQueueSignals() - own0 == 0, "an own Signal was issued during the lap (%llu): the schedule is not the review's", static_cast<unsigned long long>(game.ProbeQueueSignals() - own0));
    CHECKM(PushesBy(1) == 0, "the lock-timeout taker pushed before it was released");
    Sleep(100);
    ReleasePoint(6);
    interrupter.join();
    GpuQueueProbeTest_SetBindPause(nullptr);
    CHECKM(PushesBy(1) == 1, "the lock-timeout taker pushed %zu Submits, expected exactly one", PushesBy(1));
    std::lock_guard<std::mutex> guard(g_obs_lock);
    const GpuQueueEvent* lost = nullptr;
    for (const PushRec& p : g_pushes) if (p.taker == 1) lost = &p.event;
    CHECK(lost != nullptr);
    CheckSameIdentity(*lost, claim, "the lost-word Submit", false);
    CHECKM(lost->index != replacement.event.index && lost->enq_qpc != replacement.event.enq_qpc,
        "the lost-word Submit carries the replacement's identity (call order %llu, k + 256)", static_cast<unsigned long long>(lost->index));
    CHECKM(lost->done_value == 0 && lost->marker_trusted == 0 && lost->overlap == 1 && lost->borrowed == 0,
        "the lost-word Submit: done %llu, trusted %u, overlap %u, borrowed %u (expected a no-done, untrusted, invalid one)",
        static_cast<unsigned long long>(lost->done_value), lost->marker_trusted, lost->overlap, lost->borrowed);
    std::printf("ok: lost word after a lap: A's Submit carries the original claim (k = %llu, call order %llu, not the replacement's %llu), no second Submit\n",
        static_cast<unsigned long long>(k), static_cast<unsigned long long>(lost->index), static_cast<unsigned long long>(replacement.event.index));
}

// The worker's tail read of a CONSUMED marker. Everything is consumed (the cursor equals `issued`, nothing open). (1) A POSITIVE
// HANDSHAKE before any lap traffic: an idle step is held inside ReadTail at point 7 (the cursor and `issued` loaded), point 8 is watched, the
// read is released, and the test requires that step's acknowledgement (its arm observation) within a bound: a reader that obeys the cursor
// returns before the consumed slot and the step goes on; the old reader stops at the watched point 8 (held there: it cannot acknowledge) and
// the test fails on the count of such reads. No fixed-length watch: the verdict is tied to a step that is proven to have run.
// (2) The worker is held at point 7 again; 256 further borrowed markers are issued (k + 256 reuses the slot) with the last one's native Signal
// held back; the worker is released: the planner never arms the consumed k, a registration is outstanding for the newest marker (the held step's
// drained registration, completed + 1, IS that value here), the core finds no orphaned awaited stamp, and the newest marker's frame carries all
// its borrowed Submits. NOT COVERED: that the newest marker gets an AWAITED interval of its own (the held step makes a drained registration,
// not an armed one: the planner arms nothing while a registration is outstanding); that is planner behaviour, not asserted here.
void PhaseTailConsumed(Game& game) {
    InstallPause();
    game.PlayFrame();
    Sleep(200);
    const uint64_t consumed = MaxClaimK();
    CHECKM(consumed != 0, "no claim was observed");
    // The worker's CONSUMED cursor (not the claim frontier) must have reached the last claim before the watch means anything: a
    // delayed worker may legitimately read the tag of an unconsumed slot (point 8) on the fixed code. Bounded 5 s, then a failure.
    {
        bool consumed_cursor = false;
        for (int i = 0; i < 5000 && !consumed_cursor; ++i) {
            consumed_cursor = GpuQueueProbeTest_ConsumedCursor() >= consumed;
            if (!consumed_cursor) Sleep(1);
        }
        CHECKM(consumed_cursor, "the worker's consumed cursor (%llu) did not reach the last claim (%llu) within 5 s: the session is not idle and consumed",
            static_cast<unsigned long long>(GpuQueueProbeTest_ConsumedCursor()), static_cast<unsigned long long>(consumed));
    }
    g_consumed_k.store(consumed);
    // (1) the handshake: an idle tail read held at point 7, the watch opened, the read released, its step acknowledged.
    ArmPoint(7, 0);
    WaitPoint(7, "the idle tail read of the handshake");
    CHECKM(g_points[7].k.load() == consumed, "the idle tail read loaded issued = %llu, the last claim was %llu", static_cast<unsigned long long>(g_points[7].k.load()),
        static_cast<unsigned long long>(consumed));
    {
        std::lock_guard<std::mutex> guard(g_obs_lock);
        g_arms.clear();
    }
    g_watch8.store(true); // the watch is open BEFORE anything is released; the held read cannot run on unobserved (an expired hold is a failure at the release)
    g_arms_on.store(true);
    ReleasePoint(7);
    bool acknowledged = false;
    for (int i = 0; i < 2000 && !acknowledged && g_stale8.load() == 0; ++i) { // bounded: 2 s
        {
            std::lock_guard<std::mutex> guard(g_obs_lock);
            acknowledged = !g_arms.empty();
        }
        if (!acknowledged) Sleep(1);
    }
    CHECKM(g_stale8.load() == 0, "the worker's tail read reached the tag of the consumed slot k = %llu (point 8, %llu times): it reads slots at or below its cursor",
        static_cast<unsigned long long>(g_stale8_k.load()), static_cast<unsigned long long>(g_stale8.load()));
    CHECKM(acknowledged, "the step of the released tail read did not acknowledge within 2 s (no arm observation after the release): the handshake proves nothing");
    g_watch8.store(false);
    g_arms_on.store(false);
    ResetEvent(g_points[7].release); // manual-reset: re-armed for the lap schedule below
    // (2) the lap schedule: the worker held at point 7 again, before the tag load.
    ArmPoint(7, 0);
    WaitPoint(7, "the worker's tail read");
    const uint64_t old_k = g_points[7].k.load();
    CHECKM(old_k == consumed, "the worker read issued = %llu, the last claim was %llu: the session was not idle and consumed", static_cast<unsigned long long>(old_k),
        static_cast<unsigned long long>(consumed));
    const uint64_t s = game.frame + 1;
    const uint64_t own0 = game.ProbeQueueSignals();
    HANDLE gate_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    CHECK(gate_event != nullptr);
    std::thread producer([&] {
        for (unsigned i = 0; i < 255; ++i) game.Submit();
        game.render->signal_gate_fence.store(game.marker);
        game.render->signal_gate.store(gate_event);
        game.Submit(); // the 256th: bound, its Submit pushed, its native Signal held back
    });
    for (int i = 0; i < 5000 && game.render->signals_blocked.load() == 0; ++i) Sleep(1);
    CHECKM(game.render->signals_blocked.load() == 1, "the last marker's native Signal was never held back");
    const uint64_t last_k = old_k + 256;
    CHECKM(MaxClaimK() == last_k, "the claims reached k = %llu, expected %llu (256 markers above the consumed one)", static_cast<unsigned long long>(MaxClaimK()),
        static_cast<unsigned long long>(last_k));
    CHECKM(game.ProbeQueueSignals() - own0 == 0, "an own Signal was issued while the worker was held (%llu)", static_cast<unsigned long long>(game.ProbeQueueSignals() - own0));
    {
        std::lock_guard<std::mutex> guard(g_obs_lock);
        g_arms.clear();
    }
    g_arms_on.store(true);
    const uint64_t orphaned0 = GpuQueueProbeTest_Orphaned();
    const uint64_t last_value = game.marker_value.load(); // the game value of the newest marker (its Signal is held back: incomplete)
    ReleasePoint(7);
    // The newest marker is registered on the game's fence: either the planner arms it (armed k == last issued) or the drained registration of
    // the held step (completed + 1, which IS the newest marker's value here) is outstanding for it. Either way the registration is for it.
    bool newest = false;
    for (int i = 0; i < 1000 && !newest; ++i) {
        {
            std::lock_guard<std::mutex> guard(g_obs_lock);
            for (const auto& a : g_arms) newest = newest || a.first == last_k || a.second == last_k;
        }
        {
            std::lock_guard<std::mutex> guard(game.marker->lock);
            for (const auto& r : game.marker->registered) newest = newest || r.value == last_value;
        }
        if (!newest) Sleep(2);
    }
    {
        std::lock_guard<std::mutex> guard(g_obs_lock);
        for (const auto& a : g_arms)
            CHECKM(a.first != old_k && a.second != old_k, "the planner armed the consumed marker k = %llu (own %llu, game %llu)", static_cast<unsigned long long>(old_k),
                static_cast<unsigned long long>(a.first), static_cast<unsigned long long>(a.second));
    }
    {
        std::lock_guard<std::mutex> guard(game.marker->lock);
        for (const auto& r : game.marker->registered)
            CHECKM(r.value >= last_value - 255, "the worker registered the game fence for the consumed value %llu (newest %llu)", static_cast<unsigned long long>(r.value),
                static_cast<unsigned long long>(last_value));
    }
    CHECKM(newest, "neither the planner armed the newest marker (k = %llu) nor an outstanding registration names its value %llu", static_cast<unsigned long long>(last_k),
        static_cast<unsigned long long>(last_value));
    SetEvent(gate_event); // the newest marker's Signal is forwarded and completes
    producer.join();
    game.render->signal_gate.store(nullptr);
    game.BoundarySignal(s);
    game.PresentSide(s);
    game.frame = s;
    for (int i = 0; i < 8; ++i) { game.PlayFrame(); Sleep(2); }
    Sleep(300);
    g_arms_on.store(false);
    CHECKM(GpuQueueProbeTest_Orphaned() == orphaned0, "the core counted %llu orphaned awaited stamps (the wake was routed to an accounted marker)",
        static_cast<unsigned long long>(GpuQueueProbeTest_Orphaned() - orphaned0));
    WaitRecords(s, "the frame of the newest marker");
    bool found = false;
    for (const Record& r : glue::Records()) {
        if (r.present_sequence != s) continue;
        found = true;
        CHECKM(r.borrowed_submits == 256, "the frame of the 256 markers holds %u borrowed Submits", r.borrowed_submits);
        if (r.valid & Record::kSubmits) CHECKM(r.submits == 256, "the frame holds %u submissions, the game made 256", r.submits);
    }
    CHECKM(found, "no record for the frame of the newest marker");
    std::printf("ok: tail of a consumed marker: slot never read at or below the cursor, the newest marker (k = %llu, not the consumed %llu) is the one registered, no orphaned stamp, "
                "the frame holds its 256 borrowed Submits\n", static_cast<unsigned long long>(last_k), static_cast<unsigned long long>(old_k));
}
} // namespace

int main(int argc, char** argv) {
    bool by_rule = false, rundown = false, covered = false, clock_gate = false, lost_take = false, lost_take_covered = false, lose_word = false, tail_consumed = false;
    unsigned covered_point = 0;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--rule") == 0) by_rule = true;
        else if (std::strcmp(argv[i], "--rundown-timeout") == 0) rundown = true;
        else if (std::strcmp(argv[i], "--covered-bind") == 0) covered = true;
        else if (std::strcmp(argv[i], "--dependency-clock") == 0) clock_gate = true;
        else if (std::strcmp(argv[i], "--bind-race-lost-take") == 0) lost_take = true;
        else if (std::strcmp(argv[i], "--bind-race-lost-take-covered") == 0) lost_take_covered = true;
        else if (std::strcmp(argv[i], "--bind-race-covered-2") == 0) covered_point = 2;
        else if (std::strcmp(argv[i], "--bind-race-covered-3") == 0) covered_point = 3;
        else if (std::strcmp(argv[i], "--lose-word-lap") == 0) lose_word = true;
        else if (std::strcmp(argv[i], "--tail-consumed") == 0) tail_consumed = true;
        else { std::fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }
    {
        std::string detail;
        CHECKM(glue::VerifyVtableLayout(&detail), "%s", detail.c_str());
        std::printf("vtable layout: %s\n", detail.c_str());
    }
    Game game;
    std::string how;
    if (clock_gate) { // no session: the keys are toggled, which would end one
        PhaseDependencyClock(game);
        std::printf("ALL OK\n");
        std::fflush(stdout);
        std::_Exit(0);
    }
    if (!game.BringUp(by_rule, &how)) {
        if (by_rule) {
            std::printf("SKIP: %s\n", how.c_str());
            glue::DumpLog(stdout, 30);
            std::fflush(stdout);
            std::_Exit(77);
        }
        CHECKM(false, "bring-up: %s", how.c_str());
    }
    CHECK(GpuQueueProbeTest_Running());
    CHECK(GpuQueueProbeTest_AdoptedQueue() == static_cast<void*>(glue::Com(game.render)));
    CHECK(GpuQueueProbeTest_AdoptedRole() == 3);
    std::printf("ok: adopted the render queue as role 3 through %s; running, borrowing steady (frame %llu)\n", how.c_str(),
        static_cast<unsigned long long>(game.frame));

    glue::SetGpuLatencyUs(150); // completions arrive after a delay: the worker's registered events fire for real
    if (rundown) {
        // Each of these two ends the session in its own way, so each runs alone in its own process (ctest).
        PhaseRundownTimeout(game);
    } else if (covered) {
        PhaseCoveredBind(game);
    } else if (lost_take || lost_take_covered) {
        PhaseBindRaceTakeLost(game, lost_take_covered);
    } else if (covered_point != 0) {
        PhaseBindRaceCovered(game, covered_point);
    } else if (lose_word) {
        PhaseLoseWordLap(game);
    } else if (tail_consumed) {
        PhaseTailConsumed(game);
    } else {
        PhaseBorrowed(game);
        PhaseWaits(game);
        PhaseRace(game);
        PhaseForeign(game);
        PhaseShutdown(game);
    }
    CheckHoldsIntact("at the end of the run");
    std::printf("ALL OK\n");
    std::fflush(stdout);
    std::_Exit(0); // threads of the probe's session are gone; the fake GPU thread is not worth a join
}
