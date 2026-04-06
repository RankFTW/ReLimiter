#include "pqi.h"
#include <cmath>
#include <algorithm>
#include <cstring>

static constexpr int ROLLING_SIZE = 300;

struct PQIState {
    double frame_times[ROLLING_SIZE];
    double wake_errors[ROLLING_SIZE];
    double targets[ROLLING_SIZE];
    double wake_guards[ROLLING_SIZE];
    int    count = 0;
    int    head = 0;  // circular write position

    // Session accumulators
    double session_jitter_sum = 0.0;
    double session_wake_error_sum = 0.0;
    double session_wake_guard_sum = 0.0;
    int    session_smooth_pairs = 0;
    int    session_total_pairs = 0;
    int    session_count = 0;
    double session_target_sum = 0.0;
    double session_ft_sum = 0.0;
    double prev_frame_time = 0.0;
};

static PQIState s_state;

static double Clamp01(double v) { return (std::max)(0.0, (std::min)(1.0, v)); }

static PQIScores ComputeFromWindow(const double* ft, const double* tgt,
                                    const double* we, const double* wg,
                                    int count, int capacity, int head) {
    PQIScores s = {100.0, 1.0, 1.0, 1.0};
    if (count < 2) return s;

    // ── Cadence: IQR of frame times relative to target ──
    double sorted[ROLLING_SIZE];
    int n = (std::min)(count, capacity);
    for (int i = 0; i < n; i++) sorted[i] = ft[(head - n + i + capacity) % capacity];
    std::sort(sorted, sorted + n);

    double q1 = sorted[n / 4];
    double q3 = sorted[(n * 3) / 4];
    double iqr = q3 - q1;

    // Average target over window
    double avg_target = 0.0;
    for (int i = 0; i < n; i++) avg_target += tgt[(head - n + i + capacity) % capacity];
    avg_target /= n;
    if (avg_target <= 0.0) avg_target = 6944.0; // fallback 144Hz

    s.cadence = 1.0 - Clamp01(iqr / avg_target);

    // ── Stutter: fraction of smooth consecutive pairs ──
    double threshold = avg_target * 0.15;
    int smooth = 0, total = 0;
    for (int i = 1; i < n; i++) {
        int idx_prev = (head - n + i - 1 + capacity) % capacity;
        int idx_curr = (head - n + i + capacity) % capacity;
        double delta = std::abs(ft[idx_curr] - ft[idx_prev]);
        if (delta < threshold) smooth++;
        total++;
    }
    s.stutter = (total > 0) ? static_cast<double>(smooth) / total : 1.0;

    // ── Deadline: 1 - clamp(MAE_wake / wake_guard, 0, 1) ──
    double mae_sum = 0.0, wg_sum = 0.0;
    for (int i = 0; i < n; i++) {
        int idx = (head - n + i + capacity) % capacity;
        mae_sum += std::abs(we[idx]);
        wg_sum += wg[idx];
    }
    double mae = mae_sum / n;
    double avg_wg = (wg_sum > 0.0) ? (wg_sum / n) : 800.0;
    s.deadline = 1.0 - Clamp01(mae / avg_wg);

    // ── Composite ──
    s.pqi = (s.cadence * 0.50 + s.stutter * 0.30 + s.deadline * 0.20) * 100.0;
    return s;
}

void PQI_Push(double frame_time_us, double target_interval_us,
              double wake_error_us, double wake_guard_us) {
    auto& st = s_state;

    st.frame_times[st.head] = frame_time_us;
    st.targets[st.head] = target_interval_us;
    st.wake_errors[st.head] = wake_error_us;
    st.wake_guards[st.head] = wake_guard_us;
    st.head = (st.head + 1) % ROLLING_SIZE;
    if (st.count < ROLLING_SIZE) st.count++;

    // Session accumulators
    st.session_wake_error_sum += std::abs(wake_error_us);
    st.session_wake_guard_sum += wake_guard_us;
    st.session_target_sum += target_interval_us;
    st.session_ft_sum += frame_time_us;
    st.session_count++;

    if (st.prev_frame_time > 0.0) {
        double delta = std::abs(frame_time_us - st.prev_frame_time);
        double threshold = target_interval_us * 0.15;
        st.session_jitter_sum += delta;
        st.session_total_pairs++;
        if (delta < threshold) st.session_smooth_pairs++;
    }
    st.prev_frame_time = frame_time_us;
}

PQIScores PQI_GetRolling() {
    return ComputeFromWindow(s_state.frame_times, s_state.targets,
                             s_state.wake_errors, s_state.wake_guards,
                             s_state.count, ROLLING_SIZE, s_state.head);
}

PQIScores PQI_GetSession() {
    auto& st = s_state;
    PQIScores s = {100.0, 1.0, 1.0, 1.0};
    if (st.session_count < 2) return s;

    // Approximate cadence from jitter ratio (no full sort for session)
    double avg_jitter = st.session_jitter_sum / st.session_total_pairs;
    double avg_target = st.session_target_sum / st.session_count;
    s.cadence = 1.0 - Clamp01(avg_jitter / avg_target);

    s.stutter = (st.session_total_pairs > 0)
        ? static_cast<double>(st.session_smooth_pairs) / st.session_total_pairs : 1.0;

    double mae = st.session_wake_error_sum / st.session_count;
    double avg_wg = st.session_wake_guard_sum / st.session_count;
    s.deadline = 1.0 - Clamp01(mae / ((avg_wg > 0.0) ? avg_wg : 800.0));

    s.pqi = (s.cadence * 0.50 + s.stutter * 0.30 + s.deadline * 0.20) * 100.0;
    return s;
}

void PQI_Reset() {
    memset(&s_state, 0, sizeof(s_state));
}
