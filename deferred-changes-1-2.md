# Deferred Fixes 1 & 2 — Scheduler Patches

These two patches for `src/scheduler.cpp` were validated during the 3.1.5 FPS cap investigation but deferred pending broader testing. They address secondary failure modes (not the root cause, which was fix 3: Reflex JIT pacer override).

Apply both to `src/scheduler.cpp` on main.

---

## Fix 1: Unconditional predictor re-stamp

**Location**: `OnMarker_VRR`, around line 682

**Find this** (3 lines):
```cpp
    bool did_sleep = (should_sleep && own_sleep_end > 0);
    if (did_sleep) {
        g_predictor.OnEnforcement(frameID, s_last_enforcement_ts);
    }
```

**Replace with**:
```cpp
    bool did_sleep = (should_sleep && own_sleep_end > 0);
    // Always re-stamp the predictor's pending frame to post-enforcement time,
    // even when we didn't sleep. Without this, frames during overload or
    // zero-sleep transitions measure enforcement-to-enforcement (including
    // the previous frame's sleep) instead of pure render time. This inflates
    // predictions after target FPS changes, causing false overload detection
    // that prevents the cap from engaging.
    g_predictor.OnEnforcement(frameID, s_last_enforcement_ts);
```

**Why**: When `did_sleep` is false (overload, FPS change transitions), the predictor's start-of-frame timestamp carries forward from the previous frame. The next prediction includes the prior frame's sleep duration as "render time", inflating the EMA and triggering false overload.

---

## Fix 2: Stall recovery flush

**Location**: `OnMarker_VRR`, around line 854 (after `CSV_Push(row)`)

**Find this**:
```cpp
    // ── Stall detection logging ──
    // Log any frame where driver sleep or total frame time exceeds 50ms.
    // This fires immediately in the log file for real-time diagnosis.
    if (row.driver_sleep_us > 50000.0) {
```

**Replace the entire block** (from the comment through the closing `}` of the `else if`) with:
```cpp
    // ── Stall detection + recovery ──
    // A stall (frame time > 50ms) is a transient discontinuity — loading screen,
    // shader compile, scene transition — not sustained GPU overload. Without
    // recovery, the stall inflates the predictor EMA, which triggers false
    // overload detection that permanently disables the frame cap.
    //
    // Recovery: flush the predictor and reset miss ratio, identical to a target
    // FPS change. This treats the stall as a discontinuity boundary, discarding
    // contaminated samples so the predictor can re-converge from clean data.
    bool stall_detected = (telemetry_ft > 50000.0) || (row.driver_sleep_us > 50000.0);
    if (stall_detected) {
        g_predictor.RequestFlush();
        predictor_warm = false;
        s_last_present_deadline = now;
        s_miss_ratio = 0.0;
        s_ema_non_sleep_us = 0.0;
        s_ema_present_latency_us = 0.0;
        s_prev_frame_deviation_us = 0.0;
        s_last_own_sleep_us = 0.0;
        g_overload_active_flag.store(false, std::memory_order_relaxed);
    }

    // Log stalls for diagnosis.
    if (row.driver_sleep_us > 50000.0) {
        LOG_WARN("STALL: frame=%llu driver_sleep=%.0fus own_sleep=%.0fus "
                 "predicted=%.0f effective=%.0f tier=%d overload=%d warm=%d",
                 frameID, row.driver_sleep_us, row.own_sleep_us,
                 predicted, effective_interval, g_current_tier,
                 overload_active ? 1 : 0, predictor_warm ? 1 : 0);
    } else if (telemetry_ft > 50000.0) {
        LOG_WARN("STALL: frame=%llu actual_ft=%.0fus driver_sleep=%.0fus "
                 "own_sleep=%.0fus deadline_drift=%.0fus",
                 frameID, telemetry_ft, row.driver_sleep_us,
                 row.own_sleep_us, row.deadline_drift_us);
    }
```

**Why**: A single 200ms stall frame shifts the predictor EMA by ~16ms (at alpha=0.08). At 120fps target (8.3ms interval), this triggers permanent overload. The flush treats stalls as discontinuity boundaries, identical to the existing FPS-change flush. The 50ms threshold is 8.5× the target interval at 120fps — no false positives from normal frame variance.
