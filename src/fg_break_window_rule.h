#pragma once
// The placement rule of the render breaks (fg_present_order.h): which submissions of a frame get a break, decided
// from where the GPU was in earlier frames when one of frame generation's later Presents returned. No Windows and no
// threads here: the caller serialises the calls.
//
// A break costs GPU time (about 50 us of frame time, N.104) and each of those Presents arrives at about the same
// submission of the frame every time, so the breaks go to windows of submissions around those places. Frame
// generation presents a frame's outputs one after another (2x: the generated frame, then the real one; 3x and 4x:
// more generated frames in between). The first is ordered in front of the frame's work by present-first ordering;
// every later one arrives amid that work and has a window of its own, placed by one instance of this rule.
//
//   centre      follows the GPU: one submission towards where it was when the Present found it inside the window,
//               half the half-width when it found it before the window's first break or at or past its last.
//   half-width  follows the spread: of every kBlock such Presents, kWiden or more outside the window widen it by
//               one, kNarrow or fewer narrow it by one (N.105: about 87% inside).
//   budget      the windows of a frame together never hold more than one break per kBudgetUs of frame time (nine
//               breaks in a 17 ms frame cost the 2.7% of frame rate the owner accepted, N.105), shared evenly
//               between them. A dense narrow window does more than a wide one with gaps for the same number of
//               breaks (N.106), so the breaks are never thinned.
//   rest        a window the budget cannot widen that most Presents of a block miss does not catch them (two
//               places the Present alternates between, a spread far beyond the budget): it gets no breaks for
//               kRest frames and is then placed again from the start.
#include <cstdint>

struct FgBreakWindowRule {
    static constexpr unsigned kBlock = 128, kWiden = 24, kNarrow = 8;
    static constexpr unsigned kMiss = kBlock / 2;
    static constexpr unsigned kRest = 4 * kBlock;
    static constexpr unsigned kHalfStart = 4, kHalfMost = 32;
    static constexpr unsigned kBudgetUs = 1850;
    enum class Found : uint8_t { Before, Inside, After };
    // Submissions lo..hi of the frame get a break, counted from 1; hi 0: no window.
    struct Window { unsigned lo = 0, hi = 0; };
    // A break that was enqueued: the value its fence was signalled to and the submission it stands in front of.
    struct Point { uint64_t value = 0; unsigned index = 0; };

    unsigned centre = 0;         // 0: not placed yet
    unsigned half = kHalfStart;  // the half-width the spread asks for
    unsigned seen = 0, outside = 0;
    unsigned rest = 0;           // frames this window still gets no breaks

    void Reset() noexcept { *this = FgBreakWindowRule{}; }

    // The half-width the frame time pays for when the frame has `windows` of them.
    static unsigned BudgetHalf(unsigned frame_us, unsigned windows) noexcept {
        const unsigned breaks = frame_us / kBudgetUs / (windows ? windows : 1);
        const unsigned most = breaks > 2 ? (breaks - 1) / 2 : 1;
        return most < kHalfMost ? most : kHalfMost;
    }
    // The half-width in use: `fixed` above 0 is a developer setting and wins; else the spread's within the budget.
    unsigned Half(unsigned frame_us, unsigned windows, unsigned fixed) const noexcept {
        if (fixed) return fixed < kHalfMost ? fixed : kHalfMost;
        const unsigned most = BudgetHalf(frame_us, windows);
        return half < most ? half : most;
    }
    // A frame starts: this output's window. `count`: the submissions of the frame before (0: not counted yet, no
    // window); `place`: where an unplaced centre starts, as a share of that count in 1/256 (the outputs are spread
    // over the frame).
    Window Start(unsigned count, unsigned frame_us, unsigned windows, unsigned fixed, unsigned place = 128) noexcept {
        if (!count) return {};
        if (rest && !fixed) { --rest; return {}; }
        if (!centre) {
            centre = count > 3 ? count * place / 256 : 2;  // the Presents move it from there
            if (centre < 2) centre = 2;
        }
        const unsigned width = Half(frame_us, windows, fixed);
        return {centre > width ? centre - width : 1, centre + width};
    }
    // Does submission `index` of a frame with these windows get a break? Every `every`-th of each window, from its
    // first (`every` at least 1).
    static bool At(const Window* windows, unsigned count, unsigned every, unsigned index) noexcept {
        for (unsigned k = 0; k < count; ++k) {
            const Window& window = windows[k];
            if (window.hi && index >= window.lo && index <= window.hi && (index - window.lo) % every == 0) return true;
        }
        return false;
    }
    // Where the GPU was when a Present returned. `points`: the frame's breaks in the order they were enqueued
    // (rising values); `completed`: the newest break value the GPU has reached; `every` as in At. False: no break of
    // this window was enqueued, so there is nothing to tell. `at`: the submission of the last break of the window
    // the GPU had reached (0 when Before).
    static bool Locate(Window window, unsigned every, const Point* points, unsigned count, uint64_t completed,
        Found* found, unsigned* at) noexcept {
        if (!window.hi) return false;
        bool any = false, reached = false, past = false;
        unsigned last = 0;
        for (unsigned i = 0; i < count; ++i) {
            const Point& point = points[i];
            if (point.index < window.lo) continue;
            if (point.index > window.hi) {
                if (point.value <= completed) past = true;  // a break behind the window: the GPU has left it
                continue;
            }
            any = true;
            if (point.value <= completed) { reached = true; last = point.index; }
        }
        if (!any) return false;
        *at = last;
        if (!reached) { *found = Found::Before; return true; }
        const unsigned last_break = window.lo + (window.hi - window.lo) / every * every;
        *found = past || last >= last_break ? Found::After : Found::Inside;
        return true;
    }
    // This output's Present of a frame with `window` has returned. `found`: where the GPU was; `at`: the submission
    // whose break it had reached last (Inside only); `count`: the submissions of the frame before; the rest as in
    // Start.
    void Observe(Window window, Found found, unsigned at, unsigned count, unsigned frame_us, unsigned windows,
        unsigned fixed) noexcept {
        if (!centre || !window.hi) return;
        const unsigned width = (window.hi - window.lo) / 2;
        const unsigned step = width > 1 ? (width + 1) / 2 : 1;
        if (found == Found::Before) centre = centre > step + 1 ? centre - step : 2;
        else if (found == Found::After) centre += step;
        else if (at > centre) ++centre;
        else if (at < centre && centre > 2) --centre;
        if (count > 2 && centre > count) centre = count;
        if (fixed) return;
        if (found != Found::Inside) ++outside;
        if (++seen < kBlock) return;
        if (outside >= kWiden) {
            // Wider only while the budget would use it: a width the budget holds back is not grown in the dark.
            if (half < BudgetHalf(frame_us, windows)) ++half;
            else if (outside > kMiss) {
                Reset();
                rest = kRest;
                return;
            }
        } else if (outside <= kNarrow && half > 1) {
            --half;
        }
        seen = outside = 0;
    }
};

// How many outputs after the first a frame has: the count two frames in a row agree on (one frame with a Present
// more or less changes nothing). 1 to kMost.
struct FgBreakOutputs {
    static constexpr unsigned kMost = 5;  // frame generation up to 6x
    unsigned windows = 1, candidate = 0;

    void Reset() noexcept { *this = FgBreakOutputs{}; }
    // A frame is over: it had `later` Presents after its first.
    void OnFrame(unsigned later) noexcept {
        const unsigned count = later < 1 ? 1 : later > kMost ? kMost : later;
        if (count == windows) { candidate = 0; return; }
        if (count == candidate) { windows = count; candidate = 0; }
        else candidate = count;
    }
    // Where output `k` of `windows` starts before any Present placed it: evenly over the frame (see Start's place).
    static unsigned Place(unsigned k, unsigned windows) noexcept { return 256 * (k + 1) / (windows + 1); }
};
