#include "hang_report_policy.h"

namespace {

uint64_t Elapsed(uint64_t now, uint64_t then) noexcept { return now > then ? now - then : 0; }

} // namespace

HangDecision HangPolicy_Evaluate(const HangPolicyConfig& config, HangPolicyState* state, const HangPolicyInput& input) noexcept {
    HangDecision none;
    if (!config.enabled || !state || input.frequency == 0) return none;

    // Progress re-arms the standing stall (the count is compared for change; it may wrap).
    if (input.completed != state->seen_completed) {
        state->seen_completed = input.completed;
        state->stage = 0;
    }
    if (state->reports >= kHangMaxReports) return none;

    if (config.test_after_s != 0 && !state->test_done && input.completed != 0 &&
        Elapsed(input.now_qpc, input.first_completed_qpc) >= static_cast<uint64_t>(config.test_after_s) * input.frequency) {
        state->test_done = true;
        HangDecision decision;
        decision.reason = HangReason::ForcedTest;
        decision.index = ++state->reports;
        return decision;
    }

    uint64_t reference = 0;
    if (input.completed != 0) {
        reference = input.last_completed_qpc;
    } else if (input.swapchain_inits != 0 && input.first_swapchain_init_qpc != 0 &&
               Elapsed(input.now_qpc, input.first_swapchain_init_qpc) >=
                   static_cast<uint64_t>(kHangArmAfterInitSeconds) * input.frequency) {
        reference = input.first_swapchain_init_qpc;
    } else {
        return none;  // not armed
    }
    const uint64_t stalled = Elapsed(input.now_qpc, reference);
    HangDecision decision;
    if (state->stage == 0 && stalled >= static_cast<uint64_t>(kHangStallSeconds) * input.frequency) {
        state->stage = 1;
        decision.reason = HangReason::Stall;
    } else if (state->stage == 1 && stalled >= static_cast<uint64_t>(kHangStallAgainSeconds) * input.frequency) {
        state->stage = 2;
        decision.reason = HangReason::StallAgain;
    } else {
        return none;
    }
    decision.index = ++state->reports;
    decision.stalled_ms = stalled * 1000 / input.frequency;
    return decision;
}
