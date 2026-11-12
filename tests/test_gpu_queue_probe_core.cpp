// Directly tests the production accounting core of the developer probe gpu_queue_probe
// (src/gpu_queue_probe_core.cpp): the empty / first-gap split, immediate and non-immediate
// start stamps, margins, wait block, the monotone done chain across frames, parked and
// out-of-order stamps, boundary collapse and tagging, incomplete frames, lateness bounds, the
// bounded wait-free ring (overflow, loss marking, a small multi-producer push) and Reset.
// The D3D12 side (hook, fences, completion thread) is not covered here and needs live runs.
// The clock is scripted at 1 MHz so one tick is one microsecond.

#include "gpu_queue_probe_core.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

namespace
{
    int s_failures = 0;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,        \
                         #condition);                                           \
            ++s_failures;                                                       \
        }                                                                       \
    } while (false)

    using Rec = PacingGpuQueueRecord;
    constexpr int64_t kFreq = 1000000; // 1 tick = 1 us
    constexpr uint32_t kAll = (1u << 15) - 1u; // the fifteen valid bits

    bool Near(double a, double b, double eps = 1e-9)
    {
        return std::fabs(a - b) <= eps;
    }

    struct Sub {
        uint64_t done = 0;
    };

    struct Wd {
        uint64_t release = 0; // the fence value signalled right behind the Wait (0 = failed)
        uint64_t wait_value = 0;
    };

    struct H {
        std::unique_ptr<GpuQueueProbeCore> c = std::make_unique<GpuQueueProbeCore>();
        uint64_t next_value = 1;
        uint64_t index = 0;
        uint64_t wait_seq = 100;
        bool overlap_next = false; // the next Submit carries the overlap flag

        H() { c->Reset(kFreq); }

        static GpuQueueEvent BoundaryEvent(uint64_t gen, uint64_t seq)
        {
            GpuQueueEvent e;
            e.kind = GpuQueueEvent::Kind::Boundary;
            e.swapchain_generation = gen;
            e.present_sequence = seq;
            return e;
        }

        Sub Submit(int64_t enq, bool with_done = true)
        {
            GpuQueueEvent e;
            e.kind = GpuQueueEvent::Kind::Submit;
            e.index = ++index;
            e.overlap = static_cast<uint8_t>(overlap_next ? 1 : 0);
            overlap_next = false;
            e.enq_qpc = enq;
            Sub s;
            if (with_done) {
                s.done = next_value++;
                e.done_value = s.done;
            }
            CHECK(c->Push(e));
            return s;
        }

        // An unsatisfied queue Wait that sits after the submissions pushed so far.
        Wd Wait(int64_t enq, int64_t issue, bool with_release = true, uint64_t fence = 1)
        {
            GpuQueueEvent e;
            e.kind = GpuQueueEvent::Kind::Wait;
            e.index = index;
            e.enq_qpc = enq;
            e.release_issue_qpc = issue;
            e.fence_id = fence;
            e.wait_value = ++wait_seq;
            e.overlap = static_cast<uint8_t>(overlap_next ? 1 : 0);
            overlap_next = false;
            Wd w;
            w.wait_value = e.wait_value;
            if (with_release) {
                w.release = next_value++;
                e.release_value = w.release;
            }
            CHECK(c->Push(e));
            return w;
        }

        // A boundary on a dependency queue: the signalled fence value, no identity yet.
        void BoundaryId(uint64_t id)
        {
            GpuQueueEvent e = BoundaryEvent(0, 0);
            e.boundary_id = id;
            CHECK(c->Push(e));
        }

        void BoundaryIdent(uint64_t id, uint64_t gen, uint64_t seq)
        {
            GpuQueueEvent e = BoundaryEvent(gen, seq);
            e.boundary_id = id;
            CHECK(c->Push(e));
        }

        void Tag(uint64_t id, uint64_t gen, uint64_t seq)
        {
            GpuQueueEvent e = BoundaryEvent(gen, seq);
            e.kind = GpuQueueEvent::Kind::BoundaryTag;
            e.boundary_id = id;
            CHECK(c->Push(e));
        }

        // The frames that are ready now, without flushing.
        std::vector<Rec> Ready()
        {
            c->Drain();
            std::vector<Rec> out;
            Rec r;
            while (c->PopFrame(&r)) out.push_back(r);
            return out;
        }

        void HoldAnnounce(uint64_t fence, uint64_t wait_value, int64_t enq, uint64_t law_hold_sequence = 0)
        {
            GpuQueueEvent e;
            e.kind = GpuQueueEvent::Kind::HoldAnnounce;
            e.fence_id = fence;
            e.wait_value = wait_value;
            e.enq_qpc = enq;
            e.law_hold_sequence = law_hold_sequence;
            CHECK(c->Push(e));
        }

        void HoldEvent(uint64_t fence, uint64_t wait_value, int64_t enq, int64_t release)
        {
            GpuQueueEvent e;
            e.kind = GpuQueueEvent::Kind::Hold;
            e.fence_id = fence;
            e.wait_value = wait_value;
            e.enq_qpc = enq;
            e.release_qpc = release;
            CHECK(c->Push(e));
        }

        void Boundary(uint64_t gen, uint64_t seq) { CHECK(c->Push(BoundaryEvent(gen, seq))); }
        void BoundaryKey(uint64_t gen, uint64_t seq, const PacingFrameKey& key)
        {
            GpuQueueEvent e = BoundaryEvent(gen, seq);
            e.frame_key = key;
            CHECK(c->Push(e));
        }
        void BoundaryOverlap(uint64_t gen, uint64_t seq)
        {
            GpuQueueEvent e = BoundaryEvent(gen, seq);
            e.overlap = 1;
            CHECK(c->Push(e));
        }
        void Stamp(uint64_t value, int64_t qpc, double late = 0.0) { c->OnStamp(value, qpc, late); }
        void Drain() { c->Drain(); }

        void PushRaw(int64_t enq, uint64_t done_value)
        {
            GpuQueueEvent e;
            e.kind = GpuQueueEvent::Kind::Submit;
            e.enq_qpc = enq;
            e.done_value = done_value;
            CHECK(c->Push(e));
        }

        // Probe start: a boundary, then a first frame (tag 1,1) whose last done is warm_done.
        void Start(int64_t warm_done)
        {
            Boundary(1, 0);
            const Sub w = Submit(100);
            Stamp(w.done, warm_done);
            Boundary(1, 1);
        }

        std::vector<Rec> PopAll()
        {
            c->Flush();
            std::vector<Rec> out;
            Rec r;
            while (c->PopFrame(&r)) out.push_back(r);
            return out;
        }
    };

    const Rec* Find(const std::vector<Rec>& v, uint64_t seq)
    {
        for (const Rec& r : v)
            if (r.present_sequence == seq) return &r;
        return nullptr;
    }

    // Runs Start(warm_done), lets the caller script the frame tagged (1,2), pops it.
    template <typename F>
    Rec Frame2(int64_t warm_done, F&& script)
    {
        H h;
        h.Start(warm_done);
        script(h);
        h.Boundary(1, 2);
        const std::vector<Rec> all = h.PopAll();
        const Rec* r = Find(all, 2);
        CHECK(r != nullptr);
        return r != nullptr ? *r : Rec{};
    }

    // Pieces at an empty queue: empty term inside the frame, first submission is first_gap.
    void TestEmptyQueue()
    {
        const Rec r = Frame2(150, [](H& h) {
            const Sub a = h.Submit(1000);
            const Sub b = h.Submit(1300);
            const Sub c = h.Submit(1350);
            h.Stamp(a.done, 1100);
            h.Stamp(b.done, 1400);
            h.Stamp(c.done, 1500);
        });
        CHECK(r.valid == kAll);
        CHECK(r.submits == 3);
        CHECK(r.first_submit_qpc == 1000 && r.last_submit_qpc == 1350);
        CHECK(r.first_start_qpc == 1000 && r.last_done_qpc == 1500);
        CHECK(Near(r.first_gap_us, 850.0)); // t(1), never empty
        CHECK(Near(r.empty_us, 200.0));     // t(2) only; t(3) = 0
        CHECK(Near(r.min_margin_us, 0.0));  // arrivals at a free queue
        CHECK(Near(r.wait_block_us, 0.0));
    }

    // Pieces at a busy queue: every submission queues behind earlier work.
    void TestBusyQueue()
    {
        const Rec r = Frame2(2000, [](H& h) {
            const Sub a = h.Submit(1000);
            const Sub b = h.Submit(1100);
            const Sub c = h.Submit(1200);
            h.Stamp(a.done, 2500);
            h.Stamp(b.done, 3000);
            h.Stamp(c.done, 3500);
        });
        CHECK(r.valid == kAll);
        CHECK(Near(r.empty_us, 0.0));
        CHECK(Near(r.first_gap_us, 0.0));
        CHECK(r.first_start_qpc == 2000);
        CHECK(Near(r.min_margin_us, 1000.0)); // A: 2000 - 1000; B: 1400; C: 1800
        CHECK(r.last_done_qpc == 3500);
    }

    // done(0) is the previous frame's last done: the gap before a frame's first submission.
    void TestDoneCarriedAcrossFrames()
    {
        H h;
        h.Start(150);
        const Sub a = h.Submit(1000);
        h.Stamp(a.done, 1100);
        h.Boundary(1, 2);
        const Sub b = h.Submit(1800);
        const Sub c = h.Submit(2000);
        h.Stamp(b.done, 2000);
        h.Stamp(c.done, 2100);
        h.Boundary(1, 3);
        const std::vector<Rec> all = h.PopAll();
        const Rec* f3 = Find(all, 3);
        CHECK(f3 != nullptr);
        if (f3 == nullptr) return;
        CHECK(f3->valid == kAll);
        CHECK(Near(f3->first_gap_us, 700.0)); // 1800 - 1100
        CHECK(Near(f3->empty_us, 0.0));       // C arrives exactly when B finishes
        CHECK(f3->first_start_qpc == 1800);
    }

    // Timeline model: previous done 1100, a Wait enqueued at 1200 and released at 1250, the next
    // submission arriving at 1300. Empty 1100-1200 and 1250-1300; the Wait was pending with
    // nothing queued behind it for 50.
    void TestWaitReleasedBeforeArrival()
    {
        const Rec r = Frame2(150, [](H& h) {
            const Sub a = h.Submit(1000);
            const Wd w = h.Wait(1200, 1205);
            const Sub b = h.Submit(1300);
            h.Stamp(a.done, 1100);
            h.Stamp(w.release, 1250);
            h.Stamp(b.done, 1400);
        });
        CHECK(r.valid == kAll);
        CHECK(Near(r.empty_us, 150.0));
        CHECK(Near(r.wait_idle_us, 50.0));
        CHECK(Near(r.wait_block_us, 0.0));
        CHECK(Near(r.first_gap_us, 850.0));     // t(1) only, as before
        CHECK(Near(r.boundary_idle_us, 850.0)); // nothing in front of the first submission
        CHECK(Near(r.min_margin_us, 0.0));
        CHECK(Near(r.hold_block_us, 0.0));
        CHECK(Near(r.cycle_us, 1250.0)); // 1400 - 150
        CHECK(Near(r.stamp_late_us, 0.0));
    }

    // The same Wait released at 2000, after the submission arrived: empty only before the Wait,
    // idle until the submission arrives, the rest is the queue waiting behind the Wait.
    void TestWaitReleasedAfterArrival()
    {
        const Rec r = Frame2(150, [](H& h) {
            const Sub a = h.Submit(1000);
            const Wd w = h.Wait(1200, 1205);
            const Sub b = h.Submit(1300);
            h.Stamp(a.done, 1100);
            h.Stamp(w.release, 2000);
            h.Stamp(b.done, 2300);
        });
        CHECK(r.valid == kAll);
        CHECK(Near(r.empty_us, 100.0));
        CHECK(Near(r.wait_idle_us, 100.0));
        CHECK(Near(r.wait_block_us, 700.0));
        CHECK(Near(r.min_margin_us, 0.0)); // the first submission arrived at a free queue
        CHECK(Near(r.cycle_us, 2150.0));
    }

    // A Wait already released when its fence was signalled: the release is unknown, bounded
    // above by the Signal's issue time, which is used; the doubt shows in stamp_late.
    void TestImmediateWaitRelease()
    {
        const Rec r = Frame2(150, [](H& h) {
            const Sub a = h.Submit(1000);
            const Wd w = h.Wait(1200, 1205);
            const Sub b = h.Submit(1300);
            h.Stamp(a.done, 1100);
            h.Stamp(w.release, 1210); // 5 us after the Signal: already released
            h.Stamp(b.done, 1400);
        });
        CHECK(r.valid == kAll);
        CHECK(Near(r.empty_us, 100.0 + 95.0)); // 1100-1200 and 1205-1300
        CHECK(Near(r.wait_idle_us, 5.0));
        CHECK(Near(r.wait_block_us, 0.0));
        CHECK(Near(r.stamp_late_us, 5.0)); // the Wait could have lasted up to issue - enq
    }

    // guard = max(calibrated p95, 20 us), from p95 and not from the window's maximum.
    void TestGuardFromWakeLatency()
    {
        auto run = [](double p95, double max_us, int64_t stamp_delta) {
            H h;
            h.c->SetWakeLatencyUs(p95, max_us);
            h.Start(150);
            const Sub a = h.Submit(1000);
            const Wd w = h.Wait(1200, 1205);
            const Sub b = h.Submit(1300);
            h.Stamp(a.done, 1100);
            h.Stamp(w.release, 1205 + stamp_delta);
            h.Stamp(b.done, 1900);
            h.Boundary(1, 2);
            const std::vector<Rec> all = h.PopAll();
            const Rec* r = Find(all, 2);
            CHECK(r != nullptr);
            return r != nullptr ? *r : Rec{};
        };
        const Rec at_guard = run(0.0, 0.0, 20); // guard 20: 20 <= 20, immediate
        CHECK(Near(at_guard.wait_idle_us, 5.0));
        CHECK(Near(at_guard.empty_us, 195.0));
        const Rec over = run(5.0, 100.0, 21);   // guard 20, not 100: 21 > 20, released at 1226
        CHECK(Near(over.wait_idle_us, 26.0));
        CHECK(Near(over.empty_us, 100.0 + 74.0));
        const Rec wide = run(60.0, 60.0, 50);   // guard 60: 50 <= 60, immediate
        CHECK(Near(wide.wait_idle_us, 5.0));
        CHECK(Near(wide.empty_us, 195.0));
    }

    // Several Waits in a row: the gap before each, idle while each is pending, the stretch after
    // the last counted once.
    void TestChainedWaits()
    {
        const Rec r = Frame2(150, [](H& h) {
            const Sub a = h.Submit(1000);
            const Wd w1 = h.Wait(1200, 1205);
            const Wd w2 = h.Wait(1260, 1262);
            const Sub b = h.Submit(1300);
            h.Stamp(a.done, 1100);
            h.Stamp(w1.release, 1250);
            h.Stamp(w2.release, 1290);
            h.Stamp(b.done, 1400);
        });
        CHECK(r.valid == kAll);
        // 1100-1200, 1250-1260, 1290-1300
        CHECK(Near(r.empty_us, 100.0 + 10.0 + 10.0));
        CHECK(Near(r.wait_idle_us, 50.0 + 30.0)); // 1200-1250, 1260-1290
        CHECK(Near(r.wait_block_us, 0.0));
    }

    // The Waits in front of a frame's first submission are the boundary Waits: first_gap holds the
    // whole gap, empty gets nothing from them, wait_idle and boundary_idle split it.
    void TestBoundaryWaitsAndFirstSubmission()
    {
        const Rec released = Frame2(150, [](H& h) {
            const Wd w = h.Wait(500, 505);
            const Sub a = h.Submit(1000);
            h.Stamp(w.release, 900);
            h.Stamp(a.done, 1100);
        });
        CHECK(released.valid == kAll);
        CHECK(Near(released.first_gap_us, 850.0));
        CHECK(Near(released.empty_us, 0.0));
        CHECK(Near(released.wait_idle_us, 400.0));     // pending 500-900
        CHECK(Near(released.boundary_idle_us, 100.0)); // free 900-1000
        CHECK(Near(released.wait_block_us, 0.0));
        CHECK(released.first_start_qpc == 1000);

        const Rec held = Frame2(150, [](H& h) {
            const Wd w = h.Wait(500, 505);
            const Sub a = h.Submit(1000);
            h.Stamp(w.release, 1500);
            h.Stamp(a.done, 1600);
        });
        CHECK(held.valid == kAll);
        CHECK(Near(held.first_gap_us, 850.0));
        CHECK(Near(held.empty_us, 0.0));
        CHECK(Near(held.wait_idle_us, 500.0));     // pending 500-1000, then work is queued behind it
        CHECK(Near(held.boundary_idle_us, 0.0));   // released after the submission arrived
        CHECK(Near(held.wait_block_us, 500.0));
        CHECK(Near(held.min_margin_us, 500.0));
        CHECK(held.first_start_qpc == 1500);
    }

    // boundary_idle starts when the previous work and every boundary Wait (the hold, the
    // frame-generation wait, any other) have finished, and ends at the first submission's arrival:
    // a frame-generation Wait released 200 before the arrival leaves 200, not the whole gap, which
    // stays in first_gap.
    void TestBoundaryIdleAfterEveryBoundaryWait()
    {
        const Rec one = Frame2(150, [](H& h) {
            const Wd fg = h.Wait(500, 505, true, 3);
            const Sub a = h.Submit(1000);
            h.Stamp(fg.release, 800);
            h.Stamp(a.done, 1100);
        });
        CHECK(one.valid == kAll);
        CHECK(Near(one.boundary_idle_us, 200.0));
        CHECK(Near(one.first_gap_us, 850.0)); // the whole gap since the previous done
        CHECK(Near(one.empty_us, 0.0));       // never filed twice

        // The hold released at 600, the frame-generation wait queued behind it released at 800.
        const Rec two = Frame2(150, [](H& h) {
            const Wd hold = h.Wait(300, 305, true, 7);
            const Wd fg = h.Wait(650, 655, true, 3);
            const Sub a = h.Submit(1000);
            h.Stamp(hold.release, 600);
            h.Stamp(fg.release, 800);
            h.Stamp(a.done, 1100);
        });
        CHECK(two.valid == kAll);
        CHECK(Near(two.boundary_idle_us, 200.0));
        CHECK(Near(two.wait_idle_us, 300.0 + 150.0)); // 300-600 and 650-800
        CHECK(Near(two.first_gap_us, 850.0));
        CHECK(Near(two.empty_us, 0.0));

        // A boundary Wait released after the arrival: no idle at all.
        const Rec late = Frame2(150, [](H& h) {
            const Wd hold = h.Wait(300, 305, true, 7);
            const Wd fg = h.Wait(650, 655, true, 3);
            const Sub a = h.Submit(1000);
            h.Stamp(hold.release, 600);
            h.Stamp(fg.release, 1200);
            h.Stamp(a.done, 1300);
        });
        CHECK(Near(late.boundary_idle_us, 0.0));
    }

    // The hold: a Hold event matched to its Wait by (fence, value), either arrival order.
    // Submissions queued behind it block until its release; the margin without that time.
    void TestHoldBlockAndMarginFree()
    {
        for (int order = 0; order < 2; ++order) {
            const Rec r = Frame2(150, [order](H& h) {
                if (order == 0) {
                    const Wd w = h.Wait(500, 505, true, 7);
                    h.HoldEvent(7, w.wait_value, 500, 2000);
                    h.Stamp(w.release, 2000);
                } else {
                    h.HoldEvent(7, h.wait_seq + 1, 500, 2000); // the Hold event first
                    const Wd w = h.Wait(500, 505, true, 7);
                    h.Stamp(w.release, 2000);
                }
                const Sub a = h.Submit(1000);
                const Sub b = h.Submit(1100);
                h.Stamp(a.done, 2300);
                h.Stamp(b.done, 2400);
            });
            CHECK(r.valid == kAll);
            CHECK(Near(r.hold_block_us, 1000.0));      // 2000 - max(1000, 500), counted once
            CHECK(Near(r.min_margin_us, 1000.0));      // A: 2000 - 1000; B: 2300 - 1100
            CHECK(Near(r.min_margin_free_us, 0.0));    // A is released the moment the hold is
            CHECK(Near(r.wait_idle_us, 500.0));
            CHECK(Near(r.wait_block_us, 1000.0));
        }

        // No Hold event, or one for another value: an ordinary Wait, nothing attributed to the hold.
        for (int wrong = 0; wrong < 2; ++wrong) {
            const Rec r = Frame2(150, [wrong](H& h) {
                const Wd w = h.Wait(500, 505, true, 7);
                if (wrong != 0) h.HoldEvent(7, w.wait_value + 1, 500, 2000);
                h.Stamp(w.release, 2000);
                const Sub a = h.Submit(1000);
                h.Stamp(a.done, 2300);
            });
            CHECK(r.valid == kAll);
            CHECK(Near(r.hold_block_us, 0.0));
            CHECK(Near(r.min_margin_free_us, 1000.0));
            CHECK(Near(r.min_margin_us, 1000.0));
        }

        // A hold released before the submission arrived blocked nothing (valid, zero).
        const Rec early = Frame2(1200, [](H& h) {
            const Wd w = h.Wait(500, 505, true, 7);
            h.HoldEvent(7, w.wait_value, 500, 900);
            h.Stamp(w.release, 900);
            const Sub a = h.Submit(1000);
            h.Stamp(a.done, 1300);
        });
        CHECK(early.valid == kAll);
        CHECK(Near(early.hold_block_us, 0.0));
        CHECK(Near(early.min_margin_us, 200.0));
        CHECK(Near(early.min_margin_free_us, 200.0));
    }

    // The hold's release event can arrive after the frame's submissions completed: the announced
    // hold defers the frame until it is in, and the values are then the real ones.
    void TestHoldReleaseArrivesAfterCompletion()
    {
        H h;
        h.Start(150);
        h.HoldAnnounce(7, h.wait_seq + 1, 500);
        const Wd w = h.Wait(500, 505, true, 7);
        const Sub a = h.Submit(1000);
        const Sub b = h.Submit(1100);
        h.Stamp(w.release, 2000);
        h.Stamp(a.done, 2300);
        h.Stamp(b.done, 2400);
        h.Boundary(1, 2);
        const Sub c = h.Submit(2600);
        h.Stamp(c.done, 2700);
        h.Drain();
        Rec r;
        while (h.c->PopFrame(&r)) CHECK(r.present_sequence == 1); // frame 2 waits for the release
        h.HoldEvent(7, w.wait_value, 500, 2000);
        h.Drain();
        CHECK(h.c->PopFrame(&r));
        CHECK(r.present_sequence == 2);
        CHECK(r.valid == kAll);
        CHECK(Near(r.hold_block_us, 1000.0));   // 2000 - max(1000, 500)
        CHECK(Near(r.min_margin_free_us, 0.0));
    }

    // A hold release that never arrives: the frame is given up on, accounted without the hold, and
    // the hold fields are invalid (never a zero block or an inflated margin).
    void TestHoldReleaseNeverArrives()
    {
        H h;
        h.Start(150);
        h.HoldAnnounce(7, h.wait_seq + 1, 500);
        const Wd w = h.Wait(500, 505, true, 7);
        const Sub a = h.Submit(1000);
        h.Stamp(w.release, 2000);
        h.Stamp(a.done, 2300);
        h.Boundary(1, 2);
        const Sub c = h.Submit(2600);
        h.Stamp(c.done, 2700);
        h.Boundary(1, 3);
        const std::vector<Rec> all = h.PopAll();
        const Rec* f2 = Find(all, 2);
        const Rec* f3 = Find(all, 3);
        CHECK(f2 != nullptr && f3 != nullptr);
        if (f2 == nullptr || f3 == nullptr) return;
        CHECK(f2->valid == (kAll & ~(Rec::kHoldBlock | Rec::kMinMarginFree)));
        CHECK(f3->valid == kAll); // the chain carried on
        CHECK(Near(f3->first_gap_us, 300.0));

        // The hold evicted while its Wait waits for it: the same, without waiting.
        H g;
        g.Start(150);
        g.HoldAnnounce(7, g.wait_seq + 1, 500);
        const Wd w2 = g.Wait(500, 505, true, 7);
        const Sub a2 = g.Submit(1000);
        g.Stamp(w2.release, 2000);
        g.Stamp(a2.done, 2300);
        g.Boundary(1, 2);
        for (uint64_t i = 0; i < GpuQueueProbeCore::kHoldCapacity; ++i) g.HoldAnnounce(8, 1000 + i, 600);
        g.Drain();
        CHECK(g.c->Statistics().holds_unmatched == 1);
        Rec r;
        bool seen = false;
        g.Submit(2600); // seals frame 2
        g.Drain();
        while (g.c->PopFrame(&r)) {
            if (r.present_sequence != 2) continue;
            seen = true;
            CHECK((r.valid & Rec::kHoldBlock) == 0);
            CHECK((r.valid & Rec::kMinMarginFree) == 0);
            CHECK((r.valid & Rec::kMinMargin) != 0);
        }
        CHECK(seen);
    }

    // An announced hold whose Wait never appeared was satisfied: ordinary frame, bounded pool,
    // evictions counted.
    void TestAnnouncedHoldWithoutWait()
    {
        const Rec r = Frame2(150, [](H& h) {
            h.HoldAnnounce(7, 555, 500);
            const Sub a = h.Submit(1000);
            h.Stamp(a.done, 1100);
        });
        CHECK(r.valid == kAll);
        CHECK(Near(r.hold_block_us, 0.0));

        H h;
        for (uint64_t i = 0; i < GpuQueueProbeCore::kHoldCapacity + 2; ++i) h.HoldAnnounce(7, 100 + i, 500);
        h.Drain();
        CHECK(h.c->Statistics().holds_unmatched == 2);
    }

    // The queue law's hold sequence travels with the announce to the record of the frame queued behind that hold.
    void TestLawHoldSequenceReachesTheFrameRecord()
    {
        // One hold, one frame behind it: the sequence is on the record (the same hold that gives hold_block).
        const Rec one = Frame2(150, [](H& h) {
            h.HoldAnnounce(7, h.wait_seq + 1, 500, 41);
            const Wd w = h.Wait(500, 505, true, 7);
            const Sub a = h.Submit(1000);
            h.HoldEvent(7, w.wait_value, 500, 2000);
            h.Stamp(w.release, 2000);
            h.Stamp(a.done, 2300);
        });
        CHECK(one.valid == kAll && one.law_hold_sequence == 41 && Near(one.hold_block_us, 1000.0));

        // Two holds, two frames: each frame its own sequence.
        H two;
        two.Start(150);
        two.HoldAnnounce(7, two.wait_seq + 1, 500, 41);
        const Wd w1 = two.Wait(500, 505, true, 7);
        const Sub a = two.Submit(1000);
        two.HoldEvent(7, w1.wait_value, 500, 2000);
        two.Stamp(w1.release, 2000);
        two.Stamp(a.done, 2300);
        two.Boundary(1, 2);
        two.HoldAnnounce(7, two.wait_seq + 1, 2500, 42);
        const Wd w2 = two.Wait(2500, 2505, true, 7);
        const Sub b = two.Submit(3000);
        two.HoldEvent(7, w2.wait_value, 2500, 4000);
        two.Stamp(w2.release, 4000);
        two.Stamp(b.done, 4300);
        two.Boundary(1, 3);
        const std::vector<Rec> all = two.PopAll();
        const Rec* f2 = Find(all, 2);
        const Rec* f3 = Find(all, 3);
        CHECK(f2 != nullptr && f3 != nullptr);
        if (f2 != nullptr && f3 != nullptr) CHECK(f2->law_hold_sequence == 41 && f3->law_hold_sequence == 42);

        // No hold at all: 0.
        const Rec none = Frame2(150, [](H& h) {
            const Sub s = h.Submit(1000);
            h.Stamp(s.done, 1100);
        });
        CHECK(none.valid == kAll && none.law_hold_sequence == 0);

        // A hold announced without a sequence (not a law hold, or a zero-length hold): 0, the hold itself still accounted.
        const Rec unnumbered = Frame2(150, [](H& h) {
            h.HoldAnnounce(7, h.wait_seq + 1, 500);
            const Wd w = h.Wait(500, 505, true, 7);
            const Sub s = h.Submit(1000);
            h.HoldEvent(7, w.wait_value, 500, 2000);
            h.Stamp(w.release, 2000);
            h.Stamp(s.done, 2300);
        });
        CHECK(unnumbered.valid == kAll && unnumbered.law_hold_sequence == 0 && Near(unnumbered.hold_block_us, 1000.0));

        // An invalid frame still names its hold (the law settles it): an overlap frame, and a frame invalidated by an
        // irregular boundary event (a boundary id lower than one already seen).
        const Rec overlap = Frame2(150, [](H& h) {
            h.HoldAnnounce(7, h.wait_seq + 1, 500, 43);
            const Wd w = h.Wait(500, 505, true, 7);
            h.overlap_next = true;
            const Sub s = h.Submit(1000);
            h.HoldEvent(7, w.wait_value, 500, 2000);
            h.Stamp(w.release, 2000);
            h.Stamp(s.done, 2300);
        });
        CHECK(overlap.valid == 0 && overlap.law_hold_sequence == 43);

        H irregular;
        irregular.Start(150);
        irregular.HoldAnnounce(7, irregular.wait_seq + 1, 500, 44);
        const Wd w3 = irregular.Wait(500, 505, true, 7);
        const Sub c = irregular.Submit(1000);
        irregular.HoldEvent(7, w3.wait_value, 500, 2000);
        irregular.Stamp(w3.release, 2000);
        irregular.Stamp(c.done, 2300);
        irregular.Boundary(1, 2);
        irregular.Ready(); // the warm-up frame goes first; the hold's frame is not sealed yet
        const Sub d = irregular.Submit(2600);
        irregular.Stamp(d.done, 2700);
        irregular.BoundaryIdent(10, 1, 3);
        const Sub e = irregular.Submit(2800);
        irregular.Stamp(e.done, 2900);
        irregular.BoundaryIdent(9, 1, 4); // lower than 10: irregular, the next emitted frame is invalidated
        const std::vector<Rec> frames = irregular.PopAll();
        const Rec* hold_frame = Find(frames, 2);
        CHECK(hold_frame != nullptr);
        if (hold_frame != nullptr) CHECK(hold_frame->valid == 0 && hold_frame->law_hold_sequence == 44);
        CHECK(irregular.c->Statistics().frames_invalidated_by_irregular_boundary >= 1);
    }

    // The hold blocks nothing before the queue reached it: previous completion 1800, hold enqueued
    // 1200, next submission 1300, hold released 2000 -> 200, not 700.
    void TestHoldBoundedByWhenQueueReachedIt()
    {
        const Rec r = Frame2(1800, [](H& h) {
            const Wd w = h.Wait(1200, 1205, true, 7);
            h.HoldEvent(7, w.wait_value, 1200, 2000);
            h.Stamp(w.release, 2000);
            const Sub a = h.Submit(1300);
            h.Stamp(a.done, 2200);
        });
        CHECK(r.valid == kAll);
        CHECK(Near(r.hold_block_us, 200.0));
        CHECK(Near(r.min_margin_us, 700.0));       // 2000 - 1300: the Wait still blocks that long
        CHECK(Near(r.min_margin_free_us, 0.0));
    }

    // A boundary pushed at a dependency queue's Signal has no identity: the frame is held back until
    // the tag arrives, in either order, and is then emitted with the tag's identity.
    void TestLateTagBoundary()
    {
        H h;
        h.Start(150);
        const Sub a = h.Submit(1000);
        h.Stamp(a.done, 1100);
        h.BoundaryId(900);
        const Sub c = h.Submit(2600); // seals frame 2
        h.Stamp(c.done, 2700);
        std::vector<Rec> ready = h.Ready();
        CHECK(ready.size() == 1 && ready[0].present_sequence == 1); // frame 2 waits for its tag
        h.Tag(900, 1, 2);
        ready = h.Ready();
        CHECK(ready.size() == 1);
        if (ready.size() == 1) {
            CHECK(ready[0].present_sequence == 2 && ready[0].swapchain_generation == 1);
            CHECK(ready[0].valid == kAll);
            CHECK(ready[0].submits == 1);
        }
        // The tag first, the boundary after: attached when the boundary appears.
        h.Tag(901, 1, 3);
        h.BoundaryId(901);
        const Sub d = h.Submit(3000);
        h.Stamp(d.done, 3100);
        ready = h.Ready();
        CHECK(ready.size() == 1 && ready[0].present_sequence == 3);
        const GpuQueueProbeCore::Counters s = h.c->Statistics();
        CHECK(s.tags_unmatched == 0 && s.untagged_boundaries == 0 && s.join_ambiguous == 0);

        // A boundary pushed with its identity (and an id) is not held back.
        H g;
        g.Start(150);
        const Sub e = g.Submit(1000);
        g.Stamp(e.done, 1100);
        g.BoundaryIdent(910, 1, 2);
        const Sub f = g.Submit(2600);
        g.Stamp(f.done, 2700);
        ready = g.Ready();
        CHECK(ready.size() == 2 && ready[1].present_sequence == 2);
    }

    // A boundary whose tag never comes is dropped after four later frames (counted), not emitted
    // and not blocking the frames behind it for longer; Flush drops it at once.
    void TestUntaggedBoundaryDropped()
    {
        H h;
        h.Start(150);
        const Sub a = h.Submit(1000);
        h.Stamp(a.done, 1100);
        h.BoundaryId(900);
        int64_t enq = 2000;
        for (uint64_t k = 3; k <= 6; ++k) {
            const Sub s = h.Submit(enq);
            h.Stamp(s.done, enq + 50);
            h.Boundary(1, k);
            enq += 100;
            const std::vector<Rec> ready = h.Ready();
            if (k < 6) {
                for (const Rec& r : ready) CHECK(r.present_sequence <= 1); // still waiting for the tag
            } else {
                // Four later frames closed: frame 2 is dropped, the frames behind it come out
                // (frame 1 came out on the first pass; the last frame is not sealed yet).
                CHECK(ready.size() == 3);
                for (size_t i = 0; i < ready.size(); ++i) CHECK(ready[i].present_sequence == i + 3);
            }
        }
        GpuQueueProbeCore::Counters s = h.c->Statistics();
        CHECK(s.untagged_boundaries == 1);
        CHECK(s.join_missing == 1);
        CHECK(s.frames_untagged == 0);

        H g;
        g.Start(150);
        const Sub b = g.Submit(1000);
        g.Stamp(b.done, 1100);
        g.BoundaryId(900);
        const std::vector<Rec> all = g.PopAll();
        CHECK(all.size() == 1 && all[0].present_sequence == 1);
        s = g.c->Statistics();
        CHECK(s.untagged_boundaries == 1);
    }

    // A tag whose boundary never appears is counted when it ages out (four frames) or on Flush.
    void TestTagWithoutBoundaryCounted()
    {
        H h;
        h.Start(150);
        h.Tag(777, 1, 9);
        h.Drain();
        int64_t enq = 1000;
        for (uint64_t k = 2; k <= 6; ++k) {
            const Sub s = h.Submit(enq);
            h.Stamp(s.done, enq + 50);
            h.Boundary(1, k);
            enq += 100;
            h.Drain();
            // Parked at frame 2 (open); out of bound once more than four frames closed since.
            CHECK(h.c->Statistics().tags_unmatched == (k == 6 ? 1u : 0u));
        }
        H g;
        g.Start(150);
        g.Tag(778, 1, 9);
        g.PopAll();
        CHECK(g.c->Statistics().tags_unmatched == 1);
    }

    // Two tags for one boundary, a tag for a boundary that already has its identity, two
    // boundaries with one id, and two frames with one identity are ambiguous: counted, and the
    // frame invalid where it is unknown which identity is right.
    void TestAmbiguousJoins()
    {
        {
            H h;
            h.Start(150);
            const Sub a = h.Submit(1000);
            h.Stamp(a.done, 1100);
            h.BoundaryId(900);
            const Sub c = h.Submit(2600);
            h.Stamp(c.done, 2700);
            h.Tag(900, 1, 2);
            h.Tag(900, 1, 5); // a second identity for the same boundary
            const std::vector<Rec> all = h.PopAll();
            const Rec* f2 = Find(all, 2);
            CHECK(f2 != nullptr);
            if (f2 != nullptr) CHECK(f2->valid == 0);
            CHECK(h.c->Statistics().join_ambiguous == 1);
        }
        {
            H h; // the boundary already carried its identity
            h.Start(150);
            const Sub a = h.Submit(1000);
            h.Stamp(a.done, 1100);
            h.BoundaryIdent(910, 1, 2);
            h.Tag(910, 1, 3);
            const std::vector<Rec> all = h.PopAll();
            const Rec* f2 = Find(all, 2);
            CHECK(f2 != nullptr);
            if (f2 != nullptr) CHECK(f2->valid == 0);
            CHECK(h.c->Statistics().join_ambiguous == 1);
        }
        {
            H h; // two boundaries with one id: neither frame can be trusted to take the tag
            h.Start(150);
            const Sub a = h.Submit(1000);
            h.Stamp(a.done, 1100);
            h.BoundaryId(920);
            const Sub b = h.Submit(1300);
            h.Stamp(b.done, 1400);
            h.BoundaryId(920);
            h.Tag(920, 1, 2);
            const std::vector<Rec> all = h.PopAll();
            const Rec* f2 = Find(all, 2);
            CHECK(f2 != nullptr);
            if (f2 != nullptr) CHECK(f2->valid == 0);
            CHECK(h.c->Statistics().join_ambiguous == 1);
        }
        {
            H h; // one identity on two frames: counted, both frames still come out
            h.Start(150);
            const Sub a = h.Submit(1000);
            h.Stamp(a.done, 1100);
            h.Boundary(1, 2);
            const Sub b = h.Submit(1300);
            h.Stamp(b.done, 1400);
            h.Boundary(1, 2);
            const std::vector<Rec> all = h.PopAll();
            CHECK(all.size() == 3);
            CHECK(h.c->Statistics().join_ambiguous == 1);
        }
    }

    // An irregular boundary event invalidates exactly the next emitted frame (its cycle spans the
    // broken frame); a run of events invalidates that many following frames, each counted.
    void TestIrregularBoundaryInvalidatesNextFrame()
    {
        {
            // A boundary expires untagged: nothing is emitted for it, the next emitted frame is
            // invalid, the one after is valid with its own cycle.
            H h;
            h.Start(150);
            const Sub a = h.Submit(1000);
            h.Stamp(a.done, 1100);
            h.BoundaryId(900); // never tagged
            int64_t enq = 2000;
            for (uint64_t k = 3; k <= 6; ++k) {
                const Sub s = h.Submit(enq);
                h.Stamp(s.done, enq + 50);
                h.Boundary(1, k);
                enq += 100;
            }
            const std::vector<Rec> ready = h.Ready();
            CHECK(ready.size() == 4); // frame 1, then frames 3, 4, 5 (frame 2 expired)
            if (ready.size() == 4) {
                CHECK(ready[0].present_sequence == 1 && ready[0].valid != 0); // emitted before the expiry
                CHECK(ready[1].present_sequence == 3 && ready[1].valid == 0);
                CHECK(ready[2].present_sequence == 4 && ready[2].valid == kAll);
                CHECK(Near(ready[2].cycle_us, 100.0));
                CHECK(ready[3].present_sequence == 5 && ready[3].valid == kAll);
            }
            const GpuQueueProbeCore::Counters s = h.c->Statistics();
            CHECK(s.untagged_boundaries == 1);
            CHECK(s.irregular_boundary_events == 1);
            CHECK(s.frames_invalidated_by_irregular_boundary == 1);
        }
        {
            // Two events (boundary ids going backwards) before any frame is emitted: each charges
            // one following emitted frame, not more.
            H h;
            h.Start(150);
            int64_t enq = 1000;
            const uint64_t ids[] = {100, 90, 80, 110};
            for (uint64_t i = 0; i < 4; ++i) {
                const Sub s = h.Submit(enq);
                h.Stamp(s.done, enq + 50);
                h.BoundaryId(ids[i]);
                h.Tag(ids[i], 1, 2 + i);
                enq += 100;
            }
            const Sub e = h.Submit(enq); // seals the last frame
            h.Stamp(e.done, enq + 50);
            h.c->Drain();
            Rec r;
            CHECK(h.c->PopFrame(&r));
            CHECK(r.present_sequence == 1 && r.valid == 0);
            CHECK(h.c->Statistics().irregular_boundary_events == 2);
            CHECK(h.c->Statistics().frames_invalidated_by_irregular_boundary == 1);
            CHECK(h.c->PopFrame(&r));
            CHECK(r.present_sequence == 2 && r.valid == 0);
            CHECK(h.c->Statistics().frames_invalidated_by_irregular_boundary == 2);
            for (uint64_t seq = 3; seq <= 5; ++seq) {
                CHECK(h.c->PopFrame(&r));
                CHECK(r.present_sequence == seq && r.valid == kAll);
            }
            CHECK(h.c->Statistics().frames_invalidated_by_irregular_boundary == 2);
        }
        {
            // A tag for a frame that already finished.
            H h;
            h.Start(150);
            const Sub a = h.Submit(1000);
            h.Stamp(a.done, 1100);
            h.BoundaryId(100);
            h.Tag(100, 1, 2);
            const Sub b = h.Submit(1300);
            h.Stamp(b.done, 1400);
            h.Boundary(1, 3);
            const Sub c = h.Submit(1600);
            h.Stamp(c.done, 1700);
            CHECK(h.Ready().size() == 3);
            h.Tag(100, 1, 9); // its frame is long gone
            h.Drain();
            CHECK(h.c->Statistics().tags_unmatched == 1);
            CHECK(h.c->Statistics().irregular_boundary_events == 1);
            h.Boundary(1, 4);
            const Sub d = h.Submit(1900);
            h.Stamp(d.done, 2000);
            h.Boundary(1, 5);
            const Sub e = h.Submit(2200);
            h.Stamp(e.done, 2300);
            const std::vector<Rec> ready = h.Ready();
            CHECK(ready.size() == 2);
            if (ready.size() == 2) {
                CHECK(ready[0].present_sequence == 4 && ready[0].valid == 0);
                CHECK(ready[1].present_sequence == 5 && ready[1].valid == kAll);
            }
            CHECK(h.c->Statistics().frames_invalidated_by_irregular_boundary == 1);
        }
    }

    // A Wait whose release Signal failed: everything that needs its release is empty, the rest
    // stays, and the chain carries on.
    void TestWaitReleaseUnknown()
    {
        H h;
        h.Start(150);
        const Sub a = h.Submit(1000);
        h.Wait(1200, 1205, false);
        const Sub b = h.Submit(1300);
        h.Stamp(a.done, 1100);
        h.Stamp(b.done, 1400);
        h.Boundary(1, 2);
        const Sub c = h.Submit(1700);
        h.Stamp(c.done, 1800);
        h.Boundary(1, 3);
        const std::vector<Rec> all = h.PopAll();
        const Rec* f2 = Find(all, 2);
        const Rec* f3 = Find(all, 3);
        CHECK(f2 != nullptr && f3 != nullptr);
        if (f2 == nullptr || f3 == nullptr) return;
        const uint32_t needs_release = Rec::kEmpty | Rec::kMinMargin | Rec::kMinMarginFree | Rec::kWaitBlock |
                                       Rec::kWaitIdle;
        CHECK(f2->valid == (kAll & ~needs_release));
        CHECK(f3->valid == kAll); // done(b) is known
        CHECK(Near(f3->first_gap_us, 300.0));
    }

    // A Wait whose release stamp never comes holds its frame until Flush; the stamp is then an orphan.
    void TestWaitStampMissing()
    {
        H h;
        h.Start(150);
        const Sub a = h.Submit(1000);
        const Wd w = h.Wait(1200, 1205);
        const Sub b = h.Submit(1300);
        h.Stamp(a.done, 1100);
        h.Stamp(b.done, 1400);
        h.Boundary(1, 2);
        const std::vector<Rec> all = h.PopAll();
        const Rec* f2 = Find(all, 2);
        CHECK(f2 != nullptr);
        if (f2 == nullptr) return;
        CHECK((f2->valid & Rec::kSubmits) != 0);
        CHECK((f2->valid & Rec::kEmpty) == 0);
        CHECK((f2->valid & Rec::kWaitIdle) == 0);
        CHECK((f2->valid & Rec::kLastDone) == 0);
        h.Stamp(w.release, 1250);
        CHECK(h.c->Statistics().stamps_orphaned == 1);
    }

    // A Wait that raced a boundary or a submission invalidates the frames it may belong to.
    void TestWaitOverlapInvalidatesFrames()
    {
        {
            H h;
            h.Start(150);
            const Sub a = h.Submit(1000);
            h.overlap_next = true;
            const Wd w = h.Wait(1200, 1205);
            const Sub b = h.Submit(1300);
            h.Stamp(a.done, 1100);
            h.Stamp(w.release, 1250);
            h.Stamp(b.done, 1400);
            h.Boundary(1, 2);
            const Sub c = h.Submit(1700);
            h.Stamp(c.done, 1800);
            h.Boundary(1, 3);
            const std::vector<Rec> all = h.PopAll();
            const Rec* f2 = Find(all, 2);
            const Rec* f3 = Find(all, 3);
            CHECK(f2 != nullptr && f3 != nullptr);
            if (f2 == nullptr || f3 == nullptr) return;
            CHECK(f2->valid == 0);
            CHECK(f3->valid == kAll);
            CHECK(h.c->Statistics().frames_overlap == 1);
        }
        {
            // A boundary Wait: the frame just closed and the frame it opens are both in doubt.
            H h;
            h.Start(150);
            const Sub a = h.Submit(1000);
            h.Stamp(a.done, 1100);
            h.Boundary(1, 2);
            h.overlap_next = true;
            const Wd w = h.Wait(1200, 1205);
            const Sub c = h.Submit(1700);
            h.Stamp(w.release, 1250);
            h.Stamp(c.done, 1800);
            h.Boundary(1, 3);
            const Sub d = h.Submit(2000);
            h.Stamp(d.done, 2100);
            h.Boundary(1, 4);
            const std::vector<Rec> all = h.PopAll();
            const Rec* f2 = Find(all, 2);
            const Rec* f3 = Find(all, 3);
            const Rec* f4 = Find(all, 4);
            CHECK(f2 != nullptr && f3 != nullptr && f4 != nullptr);
            if (f2 == nullptr || f3 == nullptr || f4 == nullptr) return;
            CHECK(f2->valid == 0);
            CHECK(f3->valid == 0);
            CHECK(f4->valid == kAll);
            CHECK(h.c->Statistics().frames_overlap == 2);
        }
    }

    // Completions made monotone, the clamp counted, and the clamped value used downstream.
    void TestOutOfOrderStampsClamped()
    {
        H h;
        h.Start(100);
        const Sub a = h.Submit(300);
        const Sub b = h.Submit(320);
        const Sub c = h.Submit(470);
        h.Drain();
        h.Stamp(c.done, 500); // delivered newest first
        h.Stamp(b.done, 400);
        h.Stamp(a.done, 450);
        h.Boundary(1, 2);
        const std::vector<Rec> all = h.PopAll();
        const Rec* r = Find(all, 2);
        CHECK(r != nullptr);
        if (r == nullptr) return;
        CHECK(r->valid == kAll);
        CHECK(r->last_done_qpc == 500);
        CHECK(Near(r->first_gap_us, 200.0));
        CHECK(Near(r->empty_us, 20.0)); // done(B) = max(400, 450) = 450; 470 - 450
        CHECK(h.c->Statistics().stamps_clamped == 1);
    }

    // Stamps that arrive before their event are parked and applied at the next Drain.
    void TestParkedStamps()
    {
        H h;
        h.Start(150);
        h.Drain();
        const uint64_t a_done = h.next_value; // the value the next submission will take
        h.Stamp(a_done, 1100);                // its event is not in the ring yet
        h.Stamp(a_done + 1, 1400);
        CHECK(h.c->Statistics().stamps_orphaned == 0);
        const Sub a = h.Submit(1000);
        const Sub b = h.Submit(1300);
        CHECK(a.done == a_done && b.done == a_done + 1);
        h.Boundary(1, 2);
        const std::vector<Rec> all = h.PopAll();
        const Rec* r = Find(all, 2);
        CHECK(r != nullptr);
        if (r == nullptr) return;
        CHECK(r->valid == kAll);
        CHECK(r->last_done_qpc == 1400);
        CHECK(Near(r->empty_us, 200.0));
        CHECK(h.c->Statistics().stamps_orphaned == 0);
        CHECK(h.c->Statistics().parked_overflow == 0);
    }

    // Parking is bounded; a stamp of an already reported value is an orphan.
    void TestParkedBoundAndOrphans()
    {
        H h;
        for (uint64_t i = 0; i < GpuQueueProbeCore::kParkedCapacity + 1; ++i) h.Stamp(1000 + i, 5);
        CHECK(h.c->Statistics().parked_overflow == 1);
        CHECK(h.c->Statistics().stamps_orphaned == 0);

        H g;
        g.Start(150);
        g.Drain();
        g.Stamp(1, 999); // value 1 was stamped and resolved already
        CHECK(g.c->Statistics().stamps_orphaned == 1);
        g.Stamp(0, 1);   // 0 is "no fence value"
        CHECK(g.c->Statistics().stamps_orphaned == 2);
    }

    // The bounded ring alone: capacity events accepted, the rest dropped and counted, space
    // returns after the consumer drains.
    void TestRingCapacityAndDrop()
    {
        H h;
        for (size_t i = 0; i < GpuQueueProbeCore::kRingCapacity; ++i) CHECK(h.c->Push(H::BoundaryEvent(1, i + 1)));
        CHECK(!h.c->Push(H::BoundaryEvent(1, 5000)));
        CHECK(!h.c->Push(H::BoundaryEvent(1, 5001)));
        GpuQueueProbeCore::Counters s = h.c->Statistics();
        CHECK(s.pushed == GpuQueueProbeCore::kRingCapacity);
        CHECK(s.dropped == 2);
        h.Drain();
        CHECK(h.c->Push(H::BoundaryEvent(1, 6000)));
        s = h.c->Statistics();
        CHECK(s.pushed == GpuQueueProbeCore::kRingCapacity + 1);
    }

    // A lost event marks the open frame incomplete (no valid column) through the drop count its
    // successors carry; the done chain recovers at the next submission's done.
    void TestOverflowMarksFrameIncomplete()
    {
        H h;
        h.Start(150);
        h.Drain();
        const Sub a = h.Submit(1000); // frame (1,2)
        h.Stamp(a.done, 1100);
        for (size_t i = 0; i + 1 < GpuQueueProbeCore::kRingCapacity; ++i) h.Boundary(1, 2);
        // The ring is full now: the next submission is lost.
        GpuQueueEvent lost;
        lost.kind = GpuQueueEvent::Kind::Submit;
        lost.enq_qpc = 3000;
        lost.done_value = h.next_value++;
        CHECK(!h.c->Push(lost));
        CHECK(h.c->Statistics().dropped == 1);
        h.Drain();
        const Sub c = h.Submit(5000); // frame (1,3): its event carries drops = 1
        h.Stamp(c.done, 5100);
        h.Boundary(1, 3);
        const Sub d = h.Submit(6000); // frame (1,4)
        h.Stamp(d.done, 6100);
        h.Boundary(1, 4);
        const std::vector<Rec> all = h.PopAll();
        const Rec* f2 = Find(all, 2);
        const Rec* f3 = Find(all, 3);
        const Rec* f4 = Find(all, 4);
        CHECK(f2 != nullptr && f3 != nullptr && f4 != nullptr);
        if (f2 == nullptr || f3 == nullptr || f4 == nullptr) return;
        CHECK(f2->valid == kAll);
        CHECK(f2->submits == 1);
        CHECK(f3->valid == 0);
        CHECK(f4->valid == kAll); // done(C) is known again
        CHECK(h.c->Statistics().frames_incomplete >= 2); // frame 1 (unknown done(0)) and frame 3
    }

    // A loss reported outside the ring (a submission that could not take its turn).
    void TestNoteLostMarksFrame()
    {
        H h;
        h.Start(150);
        const Sub a = h.Submit(1000);
        h.Stamp(a.done, 1100);
        h.c->NoteLost();
        const Sub b = h.Submit(1300);
        h.Stamp(b.done, 1400);
        h.Boundary(1, 2);
        const std::vector<Rec> all = h.PopAll();
        const Rec* r = Find(all, 2);
        CHECK(r != nullptr);
        if (r == nullptr) return;
        CHECK(r->valid == 0);
        CHECK(h.c->Statistics().dropped == 1);
    }

    // Consecutive boundaries collapse; a tagged one tags the collapsed boundary (the first tag
    // wins); no empty frame is produced by the second boundary.
    void TestCollapsedBoundariesAndTagMerge()
    {
        H h;
        h.Start(150);
        const Sub a = h.Submit(1000);
        h.Stamp(a.done, 1100);
        h.Boundary(0, 0); // the hold Wait: untagged
        h.Boundary(1, 2); // the readiness site: tags the collapsed boundary
        h.Boundary(1, 3); // a second tag does not replace it
        const Sub b = h.Submit(1500);
        h.Stamp(b.done, 1600);
        h.Boundary(1, 4);
        h.Boundary(0, 0);
        const std::vector<Rec> all = h.PopAll();
        const Rec* f2 = Find(all, 2);
        const Rec* f4 = Find(all, 4);
        CHECK(f2 != nullptr);
        CHECK(f4 != nullptr);
        CHECK(Find(all, 3) == nullptr);
        CHECK(all.size() == 3); // frames 1, 2, 4
        if (f2 == nullptr || f4 == nullptr) return;
        CHECK(f2->submits == 1 && f2->first_submit_qpc == 1000);
        CHECK(f4->submits == 1 && f4->first_submit_qpc == 1500);
        CHECK(Near(f4->first_gap_us, 400.0)); // 1500 - done(a) = 1100
        CHECK(h.c->Statistics().frames_untagged == 0);
    }

    // A frame is handed out only once a submission followed its boundary: until then a later
    // boundary may still tag it.
    void TestFrameWaitsForSeal()
    {
        H h;
        h.Start(150);
        const Sub a = h.Submit(1000);
        h.Stamp(a.done, 1100);
        h.Boundary(0, 0);
        h.Drain();
        Rec r;
        while (h.c->PopFrame(&r)) CHECK(r.present_sequence == 1); // frame 1 only
        h.Boundary(1, 2);
        h.Drain();
        CHECK(!h.c->PopFrame(&r));
        const Sub b = h.Submit(1500);
        h.Stamp(b.done, 1600);
        h.Drain();
        CHECK(h.c->PopFrame(&r));
        CHECK(r.present_sequence == 2 && r.swapchain_generation == 1);
        CHECK(!h.c->PopFrame(&r));
    }

    PacingFrameKey Key(uint64_t id)
    {
        PacingFrameKey k;
        k.domain = PacingIdentityDomain::NvApiFrame;
        k.producer_epoch = 1;
        k.frame_id = id;
        return k;
    }

    // Under frame generation a boundary knows the real frame's key, not a present_sequence. The
    // key and the (generation, sequence) pair are independent identities: either tags a frame,
    // both are carried, and collapsing boundaries merge the first valid value of each.
    void TestKeyTaggedBoundaries()
    {
        H h;
        h.Start(150);
        int64_t enq = 1000;
        auto frame = [&](auto&& boundaries) {
            const Sub s = h.Submit(enq);
            h.Stamp(s.done, enq + 50);
            enq += 100;
            boundaries();
        };
        frame([&] { h.BoundaryKey(0, 0, Key(77)); });                    // key only
        frame([&] { h.BoundaryKey(1, 5, Key(88)); });                    // key and pair on one boundary
        frame([&] { h.BoundaryKey(0, 0, Key(99)); h.Boundary(2, 6); });  // key-only, then pair-only
        frame([&] {                                                      // pair-only, key-only, later ones ignored
            h.Boundary(2, 7);
            h.BoundaryKey(0, 0, Key(100));
            h.BoundaryKey(3, 8, Key(101));
        });
        frame([&] { h.BoundaryKey(1, 0, PacingFrameKey{}); });           // neither: no identity
        frame([&] { h.Boundary(1, 9); });
        const std::vector<Rec> all = h.PopAll();
        CHECK(all.size() == 6); // frame 1 plus the five identified frames
        if (all.size() != 6) return;
        CHECK(all[1].key == Key(77) && all[1].swapchain_generation == 0 && all[1].present_sequence == 0);
        CHECK(all[2].key == Key(88) && all[2].swapchain_generation == 1 && all[2].present_sequence == 5);
        CHECK(all[3].key == Key(99) && all[3].swapchain_generation == 2 && all[3].present_sequence == 6);
        CHECK(all[4].key == Key(100) && all[4].swapchain_generation == 2 && all[4].present_sequence == 7);
        CHECK(all[5].present_sequence == 9 && !PacingFrameKey_IsValid(all[5].key));
        CHECK(!PacingFrameKey_IsValid(all[0].key));
        for (size_t i = 1; i < 5; ++i) CHECK(all[i].valid != 0); // identified frames keep their columns
        const GpuQueueProbeCore::Counters s = h.c->Statistics();
        CHECK(s.frames_untagged == 1); // only the frame with neither
        CHECK(s.frames == 7);
    }

    // A frame whose boundary stays untagged has no identity: counted, never popped.
    void TestUntaggedFrame()
    {
        H h;
        h.Start(150);
        const Sub a = h.Submit(1000);
        h.Stamp(a.done, 1100);
        h.Boundary(0, 0);
        const Sub b = h.Submit(1500);
        h.Stamp(b.done, 1600);
        h.Boundary(1, 3);
        const std::vector<Rec> all = h.PopAll();
        CHECK(all.size() == 2); // frames 1 and 3
        for (const Rec& r : all) CHECK(r.swapchain_generation != 0);
        CHECK(Find(all, 3) != nullptr);
        const GpuQueueProbeCore::Counters s = h.c->Statistics();
        CHECK(s.frames_untagged == 1);
        CHECK(s.frames == 3);
    }

    // A frame whose stamps are missing waits, is emitted incomplete once four later frames
    // closed, and the chain recovers behind it; a stamp for the given-up value is an orphan.
    void TestIncompleteAfterFourLaterFrames()
    {
        H h;
        h.Boundary(1, 0);
        const Sub a1 = h.Submit(100); // never stamped
        h.Boundary(1, 1);
        Rec r;
        int64_t enq = 200;
        for (uint64_t k = 2; k <= 5; ++k) {
            const Sub s = h.Submit(enq);
            h.Stamp(s.done, enq + 50);
            h.Boundary(1, k);
            enq += 100;
            h.Drain();
            if (k < 5) CHECK(!h.c->PopFrame(&r)); // frames 2..4 closed: three later frames at most
        }
        CHECK(h.c->PopFrame(&r)); // frames 2..5 closed: four later frames
        CHECK(r.present_sequence == 1);
        CHECK((r.valid & Rec::kSubmits) != 0);
        CHECK((r.valid & Rec::kLastDone) == 0);
        CHECK((r.valid & Rec::kFirstStart) == 0);
        CHECK(h.c->PopFrame(&r)); // frame 2: its done(0) is unknown behind the given-up submission
        CHECK(r.present_sequence == 2);
        CHECK((r.valid & Rec::kLastDone) != 0);
        CHECK((r.valid & Rec::kFirstGap) == 0);
        CHECK((r.valid & Rec::kEmpty) == 0);
        CHECK(h.c->PopFrame(&r)); // frame 3: the chain is back
        CHECK(r.present_sequence == 3 && r.valid == kAll);
        CHECK(h.c->Statistics().frames_incomplete == 2);
        h.Stamp(a1.done, 5000); // far too late
        CHECK(h.c->Statistics().stamps_orphaned == 1);
    }

    // Within the four-frame allowance a late stamp completes the frame, and nothing is
    // emitted before it.
    void TestLateStampBeforeAllowanceCompletes()
    {
        H h;
        h.Start(150);
        const Sub a = h.Submit(1000);
        h.Boundary(1, 2);
        const Sub b = h.Submit(1500);
        h.Stamp(b.done, 1600);
        h.Drain();
        Rec r;
        while (h.c->PopFrame(&r)) CHECK(r.present_sequence == 1);
        h.Stamp(a.done, 1100);
        h.Drain();
        CHECK(h.c->PopFrame(&r));
        CHECK(r.present_sequence == 2 && r.valid == kAll);
    }

    // Flush emits a frame that is still missing stamps, incomplete, without waiting.
    void TestFlushEmitsIncomplete()
    {
        H h;
        h.Start(150);
        const Sub a = h.Submit(1000);
        h.Boundary(1, 2);
        h.Drain();
        Rec r;
        while (h.c->PopFrame(&r)) CHECK(r.present_sequence == 1);
        (void)a;
        const std::vector<Rec> all = h.PopAll();
        const Rec* f2 = Find(all, 2);
        CHECK(f2 != nullptr);
        if (f2 == nullptr) return;
        CHECK((f2->valid & Rec::kSubmits) != 0);
        CHECK((f2->valid & Rec::kLastDone) == 0);
        CHECK((f2->valid & Rec::kEmpty) == 0);
        CHECK(h.c->Statistics().frames_incomplete >= 1);
    }

    // Event-awaited stamps are bounded by the calibrated wake latency; a late pickup is bounded
    // by stamp - L with L chained through the previous value.
    void TestLateBoundAndBaseline()
    {
        // Baseline: the calibrated latency is the floor, a larger known bound wins.
        {
            H h;
            h.c->SetWakeLatencyUs(30.0, 30.0);
            h.Start(150);
            const Sub a = h.Submit(1000);
            h.Stamp(a.done, 1100, 5.0);
            h.Boundary(1, 2);
            const std::vector<Rec> all = h.PopAll();
            const Rec* r = Find(all, 2);
            CHECK(r != nullptr);
            if (r != nullptr) {
                CHECK((r->valid & Rec::kStampLate) != 0);
                CHECK(Near(r->stamp_late_us, 30.0));
            }
        }
        {
            H h;
            h.c->SetWakeLatencyUs(30.0, 30.0);
            h.Start(150);
            const Sub a = h.Submit(1000);
            h.Stamp(a.done, 1100, 50.0);
            h.Boundary(1, 2);
            const std::vector<Rec> all = h.PopAll();
            const Rec* r = Find(all, 2);
            CHECK(r != nullptr);
            if (r != nullptr) CHECK(Near(r->stamp_late_us, 50.0));
        }
        // Late pickup: found complete at 5000, the previous value (the warm done, awaited at 150)
        // gives L = max(enq 1000, 150) = 1000.
        {
            H h;
            h.c->SetWakeLatencyUs(30.0, 30.0);
            h.Start(150);
            const Sub a = h.Submit(1000);
            h.Stamp(a.done, 5000, -1.0);
            h.Boundary(1, 2);
            const std::vector<Rec> all = h.PopAll();
            const Rec* r = Find(all, 2);
            CHECK(r != nullptr);
            if (r != nullptr) CHECK(Near(r->stamp_late_us, 4000.0));
        }
        // Chain: A awaited at 3000 (completion lower bound 3000 - 30 = 2970); B enq 1500 found
        // complete at 5000: L = max(1500, 2970), so the bound is 2030.
        {
            H h;
            h.c->SetWakeLatencyUs(30.0, 30.0);
            h.Start(150);
            const Sub a = h.Submit(1000);
            const Sub b = h.Submit(1500);
            h.Stamp(a.done, 3000, 0.0);
            h.Stamp(b.done, 5000, -1.0);
            h.Boundary(1, 2);
            const std::vector<Rec> all = h.PopAll();
            const Rec* r = Find(all, 2);
            CHECK(r != nullptr);
            if (r != nullptr) CHECK(Near(r->stamp_late_us, 2030.0));
        }
    }

    // The late-pickup bound chains completion lower bounds, never observation times.
    void TestLatePickupClassification()
    {
        // p95 = 30 us, largest calibration sample = 100 us.
        H h;
        h.c->SetWakeLatencyUs(30.0, 100.0);
        h.Start(150); // warm value 1 awaited at 150: L = 50
        const Sub a = h.Submit(1000);
        const Sub b = h.Submit(1500);
        h.Stamp(a.done, 3000, 0.0);   // awaited: L(a) = 3000 - 100 = 2900, not 3000
        h.Stamp(b.done, 5000, -1.0);  // pickup: L = max(1500, 2900): bound 2100, not 2000
        h.Boundary(1, 2);
        const Sub c = h.Submit(1600);
        const Sub d = h.Submit(1700);
        h.Stamp(c.done, 5200, 0.0);   // awaited: L(c) = 5100, never above its own stamp - max
        h.Stamp(d.done, 6000, -1.0);  // pickup: L = max(1700, 5100): bound 900, not 800
        h.Boundary(1, 3);
        const Sub e = h.Submit(1800);
        const Sub f = h.Submit(1900);
        h.Stamp(e.done, 7000, 500.0); // awaited with a larger known bound: L = 7000 - 500
        h.Stamp(f.done, 9000, -1.0);  // pickup: L = max(1900, 6500): bound 2500
        h.Boundary(1, 4);
        const std::vector<Rec> all = h.PopAll();
        const Rec* f2 = Find(all, 2);
        const Rec* f3 = Find(all, 3);
        const Rec* f4 = Find(all, 4);
        CHECK(f2 != nullptr && f3 != nullptr && f4 != nullptr);
        if (f2 == nullptr || f3 == nullptr || f4 == nullptr) return;
        CHECK(Near(f2->stamp_late_us, 2100.0));
        CHECK(Near(f3->stamp_late_us, 900.0));
        CHECK(Near(f4->stamp_late_us, 2500.0));
    }

    // A submission with overlap invalidates its own frame only; the done chain still carries.
    void TestSubmitOverlapInvalidatesItsFrame()
    {
        H h;
        h.Start(150);
        const Sub a = h.Submit(1000);
        h.overlap_next = true; // e.g. a queue Wait raced this submission
        const Sub b = h.Submit(1300);
        h.Stamp(a.done, 1100);
        h.Stamp(b.done, 2300);
        h.Boundary(1, 2);
        const Sub c = h.Submit(2500);
        h.Stamp(c.done, 2600);
        h.Boundary(1, 3);
        const Sub d = h.Submit(2800);
        h.Stamp(d.done, 2900);
        h.Boundary(1, 4);
        const std::vector<Rec> all = h.PopAll();
        CHECK(all.size() == 4);
        for (size_t i = 0; i < all.size(); ++i) CHECK(all[i].present_sequence == i + 1); // order kept
        const Rec* f2 = Find(all, 2);
        const Rec* f3 = Find(all, 3);
        const Rec* f4 = Find(all, 4);
        CHECK(f2 != nullptr && f3 != nullptr && f4 != nullptr);
        if (f2 == nullptr || f3 == nullptr || f4 == nullptr) return;
        CHECK(f2->valid == 0);
        CHECK(f2->swapchain_generation == 1 && f2->submits == 0);
        CHECK(f3->valid == kAll);
        CHECK(Near(f3->first_gap_us, 200.0)); // done(b) = 2300 carried through the invalid frame
        CHECK(f4->valid == kAll);
        const GpuQueueProbeCore::Counters s = h.c->Statistics();
        CHECK(s.frames_overlap == 1);
        CHECK(s.frames_incomplete >= 2); // frame 1 (unknown done(0)) and frame 2
    }

    // A boundary with overlap invalidates both frames it separates, each counted once.
    void TestBoundaryOverlapInvalidatesBothNeighbours()
    {
        H h;
        h.Start(150);
        const Sub a = h.Submit(1000);
        h.Stamp(a.done, 1100);
        h.Boundary(1, 2);
        const Sub b = h.Submit(1300);
        h.Stamp(b.done, 1400);
        h.BoundaryOverlap(1, 3); // a submission was in flight on another thread
        const Sub c = h.Submit(1600);
        h.Stamp(c.done, 1700);
        h.Boundary(1, 4);
        const Sub d = h.Submit(1900);
        h.Stamp(d.done, 2000);
        h.Boundary(1, 5);
        const std::vector<Rec> all = h.PopAll();
        CHECK(all.size() == 5);
        for (size_t i = 0; i < all.size(); ++i) CHECK(all[i].present_sequence == i + 1);
        const Rec* f2 = Find(all, 2);
        const Rec* f3 = Find(all, 3);
        const Rec* f4 = Find(all, 4);
        const Rec* f5 = Find(all, 5);
        CHECK(f2 != nullptr && f3 != nullptr && f4 != nullptr && f5 != nullptr);
        if (f2 == nullptr || f3 == nullptr || f4 == nullptr || f5 == nullptr) return;
        CHECK(f2->valid == kAll);
        CHECK(f3->valid == 0); // the frame the boundary closes
        CHECK(f4->valid == 0); // the frame it opens
        CHECK(f5->valid == kAll);
        CHECK(h.c->Statistics().frames_overlap == 2);

        // A collapsing boundary with overlap: the frame it merges into and the next one.
        H g;
        g.Start(150);
        const Sub e = g.Submit(1000);
        g.Stamp(e.done, 1100);
        g.Boundary(0, 0);
        g.BoundaryOverlap(1, 2); // collapses into the previous boundary, tags it
        const Sub f = g.Submit(1300);
        g.Stamp(f.done, 1400);
        g.Boundary(1, 3);
        const Sub k = g.Submit(1600);
        g.Stamp(k.done, 1700);
        g.Boundary(1, 4);
        const std::vector<Rec> all2 = g.PopAll();
        CHECK(Find(all2, 2) != nullptr && Find(all2, 3) != nullptr && Find(all2, 4) != nullptr);
        if (Find(all2, 2) == nullptr || Find(all2, 3) == nullptr || Find(all2, 4) == nullptr) return;
        CHECK(Find(all2, 2)->valid == 0);
        CHECK(Find(all2, 3)->valid == 0);
        CHECK(Find(all2, 4)->valid == kAll);
        CHECK(g.c->Statistics().frames_overlap == 2);
    }

    // The first frame after a start has no done(0): the columns that need it are empty.
    void TestUnknownDoneZeroValidBits()
    {
        H h;
        h.Boundary(1, 0);
        const Sub a = h.Submit(100);
        h.Stamp(a.done, 150);
        h.Boundary(1, 1);
        const std::vector<Rec> all = h.PopAll();
        const Rec* r = Find(all, 1);
        CHECK(r != nullptr);
        if (r == nullptr) return;
        const uint32_t expected = Rec::kSubmits | Rec::kFirstSubmit | Rec::kLastSubmit | Rec::kLastDone |
                                  Rec::kStampLate | Rec::kWaitBlock | Rec::kWaitIdle | Rec::kHoldBlock;
        CHECK(r->valid == expected);
        CHECK(r->submits == 1 && r->first_submit_qpc == 100 && r->last_submit_qpc == 100);
        CHECK(r->last_done_qpc == 150);
        CHECK(h.c->Statistics().frames_incomplete == 1);
    }

    // A submission whose done Signal failed breaks the chain at that point only.
    void TestSubmissionWithoutDone()
    {
        H h;
        h.Start(150);
        const Sub a = h.Submit(1000);
        h.Submit(1200, false);
        const Sub c = h.Submit(1300);
        h.Stamp(a.done, 1100);
        h.Stamp(c.done, 1500);
        h.Boundary(1, 2);
        const Sub d = h.Submit(1800);
        h.Stamp(d.done, 1900);
        h.Boundary(1, 3);
        const std::vector<Rec> all = h.PopAll();
        const Rec* f2 = Find(all, 2);
        const Rec* f3 = Find(all, 3);
        CHECK(f2 != nullptr && f3 != nullptr);
        if (f2 == nullptr || f3 == nullptr) return;
        CHECK(f2->submits == 3);
        CHECK((f2->valid & Rec::kLastDone) != 0 && f2->last_done_qpc == 1500);
        CHECK((f2->valid & Rec::kFirstGap) != 0);
        CHECK((f2->valid & Rec::kEmpty) == 0);     // C's done(B) is unknown
        CHECK((f2->valid & Rec::kMinMargin) == 0);
        CHECK(f3->valid == kAll);                  // done(C) is known again
        CHECK(Near(f3->first_gap_us, 300.0));
    }

    // Reset drops events, parked stamps and the chain, keeps the counters, and takes the new
    // frequency.
    void TestReset()
    {
        H h;
        h.Start(150);
        const Sub a = h.Submit(1000);
        h.Stamp(a.done, 1100);
        h.Stamp(500, 77); // parked: no such value yet
        h.Boundary(1, 2);
        const uint64_t pushed_before = h.c->Statistics().pushed;
        CHECK(pushed_before > 0);
        h.c->Reset(2000000);
        Rec r;
        h.c->Drain();
        CHECK(!h.c->PopFrame(&r));
        CHECK(h.c->Statistics().pushed == pushed_before);

        h.Boundary(1, 10);
        h.PushRaw(4000, 500); // the parked stamp for 500 must be gone
        h.Boundary(1, 11);
        const std::vector<Rec> all = h.PopAll();
        const Rec* f = Find(all, 11);
        CHECK(f != nullptr);
        if (f == nullptr) return;
        CHECK((f->valid & Rec::kLastDone) == 0); // done(500) never stamped
        CHECK((f->valid & Rec::kFirstGap) == 0); // done(0) unknown after Reset

        // With every stamp in place the new frequency shows: 1000 ticks at 2 MHz = 500 us.
        h.c->Reset(2000000);
        h.Boundary(1, 20);
        const Sub x = h.Submit(1000);
        h.Stamp(x.done, 1100);
        h.Boundary(1, 21);
        const Wd w = h.Wait(2000, 2005);
        const Sub y = h.Submit(3000);
        h.Stamp(w.release, 4000);
        h.Stamp(y.done, 4100);
        h.Boundary(1, 22);
        const std::vector<Rec> all2 = h.PopAll();
        const Rec* g = Find(all2, 22);
        CHECK(g != nullptr);
        if (g == nullptr) return;
        CHECK((g->valid & Rec::kWaitBlock) != 0);
        CHECK(Near(g->wait_block_us, 500.0)); // (4000 - 3000) ticks / 2 MHz
        CHECK(Near(g->wait_idle_us, 500.0));  // (3000 - 2000) ticks / 2 MHz
    }

    // No event lost or duplicated below capacity, drops counted above it, with concurrent producers.
    void TestMultiProducerBelowCapacity()
    {
        constexpr int kThreads = 3;
        constexpr int kPer = 160; // 480 submissions < the pending capacity
        H h;
        h.Boundary(1, 0);
        std::atomic<int> failed{0};
        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&h, &failed, t]() {
                for (int k = 0; k < kPer; ++k) {
                    GpuQueueEvent e;
                    e.kind = GpuQueueEvent::Kind::Submit;
                    e.enq_qpc = 1000 + t * 1000 + k;
                    e.done_value = 1 + static_cast<uint64_t>(t) * kPer + static_cast<uint64_t>(k);
                    if (!h.c->Push(e)) failed.fetch_add(1);
                }
            });
        }
        for (std::thread& th : threads) th.join();
        CHECK(failed.load() == 0);
        h.Boundary(1, 1);
        h.Drain();
        for (uint64_t v = 1; v <= static_cast<uint64_t>(kThreads) * kPer; ++v) h.Stamp(v, 100000 + static_cast<int64_t>(v));
        const std::vector<Rec> all = h.PopAll();
        const Rec* r = Find(all, 1);
        CHECK(r != nullptr);
        if (r != nullptr) CHECK(r->submits == static_cast<uint32_t>(kThreads * kPer));
        const GpuQueueProbeCore::Counters s = h.c->Statistics();
        CHECK(s.dropped == 0);
        CHECK(s.pushed == static_cast<uint64_t>(kThreads * kPer + 2));
        CHECK(s.stamps_orphaned == 0); // every value found its one submission
        CHECK(s.parked_overflow == 0);
    }

    void TestMultiProducerAboveCapacity()
    {
        constexpr int kThreads = 3;
        constexpr int kPer = 500;
        H h;
        std::atomic<int> accepted{0};
        std::atomic<int> refused{0};
        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&h, &accepted, &refused, t]() {
                for (int k = 0; k < kPer; ++k) {
                    if (h.c->Push(H::BoundaryEvent(static_cast<uint64_t>(t) + 1, static_cast<uint64_t>(k) + 1)))
                        accepted.fetch_add(1);
                    else
                        refused.fetch_add(1);
                }
            });
        }
        for (std::thread& th : threads) th.join();
        const GpuQueueProbeCore::Counters s = h.c->Statistics();
        CHECK(accepted.load() + refused.load() == kThreads * kPer);
        CHECK(accepted.load() == static_cast<int>(GpuQueueProbeCore::kRingCapacity));
        CHECK(s.pushed == GpuQueueProbeCore::kRingCapacity);
        CHECK(s.dropped == static_cast<uint64_t>(refused.load()));
        h.Drain();
        CHECK(h.c->Push(H::BoundaryEvent(1, 9999))); // the consumer made room again
    }
}

int main()
{
    TestEmptyQueue();
    TestBusyQueue();
    TestDoneCarriedAcrossFrames();
    TestWaitReleasedBeforeArrival();
    TestWaitReleasedAfterArrival();
    TestImmediateWaitRelease();
    TestGuardFromWakeLatency();
    TestChainedWaits();
    TestBoundaryWaitsAndFirstSubmission();
    TestBoundaryIdleAfterEveryBoundaryWait();
    TestHoldBlockAndMarginFree();
    TestHoldReleaseArrivesAfterCompletion();
    TestLawHoldSequenceReachesTheFrameRecord();
    TestHoldReleaseNeverArrives();
    TestAnnouncedHoldWithoutWait();
    TestHoldBoundedByWhenQueueReachedIt();
    TestWaitReleaseUnknown();
    TestLateTagBoundary();
    TestUntaggedBoundaryDropped();
    TestTagWithoutBoundaryCounted();
    TestAmbiguousJoins();
    TestIrregularBoundaryInvalidatesNextFrame();
    TestWaitStampMissing();
    TestWaitOverlapInvalidatesFrames();
    TestOutOfOrderStampsClamped();
    TestParkedStamps();
    TestParkedBoundAndOrphans();
    TestRingCapacityAndDrop();
    TestOverflowMarksFrameIncomplete();
    TestNoteLostMarksFrame();
    TestCollapsedBoundariesAndTagMerge();
    TestFrameWaitsForSeal();
    TestUntaggedFrame();
    TestKeyTaggedBoundaries();
    TestIncompleteAfterFourLaterFrames();
    TestLateStampBeforeAllowanceCompletes();
    TestFlushEmitsIncomplete();
    TestLateBoundAndBaseline();
    TestLatePickupClassification();
    TestSubmitOverlapInvalidatesItsFrame();
    TestBoundaryOverlapInvalidatesBothNeighbours();
    TestUnknownDoneZeroValidBits();
    TestSubmissionWithoutDone();
    TestReset();
    TestMultiProducerBelowCapacity();
    TestMultiProducerAboveCapacity();
    if (s_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", s_failures);
        return 1;
    }
    std::puts("gpu_queue_probe_core: all checks passed");
    return 0;
}
