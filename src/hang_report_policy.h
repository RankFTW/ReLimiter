#pragma once

#include <cstdint>

// When the stall report is written (hang_report.h): a pure decision over the progress values and the clock, so the
// rules are tested without a process. No Windows, no logger.
//
//  - Armed once a present event has completed in this process, or, with a swapchain initialised and no present event
//    yet, 60 s after that initialisation.
//  - A stall is: no present event completed for 20 s -> report 1. The same stall still standing at 90 s -> report 2.
//    Progress (another completed present event) re-arms it. At most three reports per process.
//  - The developer key hang_report_test_after_s forces ONE report that many seconds after the first completed present
//    event (it counts against the cap), so the writer can be proven in a process that is not hung.
//  - hang_report off: nothing.

inline constexpr uint32_t kHangMaxReports = 3;
inline constexpr uint32_t kHangArmAfterInitSeconds = 60;
inline constexpr uint32_t kHangStallSeconds = 20;
inline constexpr uint32_t kHangStallAgainSeconds = 90;

struct HangPolicyConfig {
    bool enabled = true;
    uint32_t test_after_s = 0;  // 0 = off
};

struct HangPolicyInput {
    uint64_t now_qpc = 0;
    uint64_t frequency = 0;
    uint32_t completed = 0;               // present events completed in this process
    uint64_t first_completed_qpc = 0;
    uint64_t last_completed_qpc = 0;
    uint32_t swapchain_inits = 0;
    uint64_t first_swapchain_init_qpc = 0;
};

// Lives for the process (a reload of the add-on keeps the count of reports written).
struct HangPolicyState {
    uint32_t reports = 0;          // written so far (stall and test)
    uint32_t seen_completed = 0;   // the completed count at the last evaluation
    uint8_t stage = 0;             // 0: the standing stall has no report, 1: report at 20 s written, 2: report at 90 s written
    bool test_done = false;
};

enum class HangReason : uint8_t { None = 0, Stall, StallAgain, ForcedTest };

struct HangDecision {
    HangReason reason = HangReason::None;
    uint32_t index = 0;        // 1-based number of the report to write (file number)
    uint64_t stalled_ms = 0;   // time since the last progress (the init when no present completed), 0 for the test
};

HangDecision HangPolicy_Evaluate(const HangPolicyConfig& config, HangPolicyState* state, const HangPolicyInput& input) noexcept;

inline const char* HangReasonName(HangReason reason) noexcept {
    switch (reason) {
    case HangReason::None: return "none";
    case HangReason::Stall: return "stall";
    case HangReason::StallAgain: return "stall-again";
    case HangReason::ForcedTest: return "forced-test";
    }
    return "unknown";
}
