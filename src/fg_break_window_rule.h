#pragma once
// The placement rule of the render breaks (fg_present_order.h): which submissions of a frame get a break, decided
// from where the GPU was in earlier frames when the real frame's Present returned. No Windows and no threads here:
// the caller serialises the calls.
//
// A break costs GPU time (about 50 us of frame time, N.104) and the Present arrives at about the same submission of
// the frame every time, so the breaks go to a window of submissions around that place.
//
//   centre      follows the GPU: one submission towards where it was when the Present found it inside the window,
//               half the half-width when it found it before the window's first break or at or past its last.
//   half-width  follows the spread: of every kBlock such Presents, kWiden or more outside the window widen it by
//               one, kNarrow or fewer narrow it by one (N.105: about 87% inside).
//   budget      the half-width never exceeds what one break per kBudgetUs of frame time pays for (nine breaks in a
//               17 ms frame cost the 2.7% of frame rate the owner accepted, N.105). A dense narrow window does
//               more than a wide one with gaps for the same number of breaks, so the breaks are never thinned.
#include <cstdint>

struct FgBreakWindowRule {
    static constexpr unsigned kBlock = 128, kWiden = 24, kNarrow = 8;
    static constexpr unsigned kHalfStart = 4, kHalfMost = 32;
    static constexpr unsigned kBudgetUs = 1850;
    enum class Found : uint8_t { Before, Inside, After };
    // Submissions lo..hi of the frame get a break, counted from 1; hi 0: no window.
    struct Window { unsigned lo = 0, hi = 0; };

    unsigned centre = 0;         // 0: not placed yet
    unsigned half = kHalfStart;  // the half-width the spread asks for
    unsigned seen = 0, outside = 0;

    void Reset() noexcept { *this = FgBreakWindowRule{}; }

    // The half-width the frame time pays for.
    static unsigned BudgetHalf(unsigned frame_us) noexcept {
        const unsigned breaks = frame_us / kBudgetUs;
        const unsigned most = breaks > 2 ? (breaks - 1) / 2 : 1;
        return most < kHalfMost ? most : kHalfMost;
    }
    // The half-width in use: `fixed` above 0 is a developer setting and wins; else the spread's within the budget.
    unsigned Half(unsigned frame_us, unsigned fixed) const noexcept {
        if (fixed) return fixed;
        const unsigned most = BudgetHalf(frame_us);
        return half < most ? half : most;
    }
    // A frame's first submission: its window. `count`: the submissions of the frame before.
    Window Start(unsigned count, unsigned frame_us, unsigned fixed) noexcept {
        if (!centre) centre = count > 3 ? count / 2 : 2;  // the middle; the Presents move it from there
        const unsigned width = Half(frame_us, fixed);
        return {centre > width ? centre - width : 1, centre + width};
    }
    // The real frame's Present of a frame with `window` has returned. `found`: where the GPU was; `at`: the
    // submission whose break it had reached last (Inside only); `count`: the submissions of the frame before;
    // `fixed` as in Start.
    void Observe(Window window, Found found, unsigned at, unsigned count, unsigned frame_us, unsigned fixed) noexcept {
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
        // Wider only while the budget would use it: a width the budget holds back is not grown in the dark.
        if (outside >= kWiden) { if (half < BudgetHalf(frame_us)) ++half; }
        else if (outside <= kNarrow && half > 1) --half;
        seen = outside = 0;
    }
};
