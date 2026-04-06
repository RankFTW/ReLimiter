#pragma once

#include <cstdint>
#include <cmath>
#include <algorithm>
#include <functional>
#include <vector>

// ── TaggedSample: used by stress detector only ──
struct TaggedSample {
    double value;
    bool   during_overload;
};

// ── RollingWindow<T, N> ──
// Fixed-capacity circular buffer with statistics.
//
// For T = double: Push, Mean, StdDev, Percentile (insertion-sort shadow copy),
//                 Size, Clear, TakeLast, Latest, JustWrapped.
// For T = TaggedSample: additionally MeanWhere, Where, MeanAll, PercentileAll, LatestValue.

template<typename T, size_t N>
class RollingWindow {
public:
    void Push(const T& val) {
        data_[head_] = val;
        head_ = (head_ + 1) % N;
        if (count_ < N)
            count_++;
        just_wrapped_ = (head_ == 0 && count_ == N);
    }

    size_t Size() const { return count_; }

    void Clear() {
        head_ = 0;
        count_ = 0;
        just_wrapped_ = false;
    }

    bool JustWrapped() const { return just_wrapped_; }

    // Most recent element
    const T& Latest() const {
        size_t idx = (head_ + N - 1) % N;
        return data_[idx];
    }

    // Return the last `n` elements (oldest first)
    std::vector<T> TakeLast(size_t n) const {
        if (n > count_) n = count_;
        std::vector<T> result;
        result.reserve(n);
        size_t start = (head_ + N - n) % N;
        for (size_t i = 0; i < n; i++)
            result.push_back(data_[(start + i) % N]);
        return result;
    }

    // ── double-valued statistics ──

    double Mean() const {
        if (count_ == 0) return 0.0;
        double sum = 0.0;
        for (size_t i = 0; i < count_; i++)
            sum += ValueOf(data_[Index(i)]);
        return sum / static_cast<double>(count_);
    }

    double StdDev() const {
        if (count_ < 2) return 0.0;
        double m = Mean();
        double sum_sq = 0.0;
        for (size_t i = 0; i < count_; i++) {
            double d = ValueOf(data_[Index(i)]) - m;
            sum_sq += d * d;
        }
        return std::sqrt(sum_sq / static_cast<double>(count_));
    }

    // Percentile via insertion-sort shadow copy
    double Percentile(double p) const {
        if (count_ == 0) return 0.0;
        double sorted[N];
        for (size_t i = 0; i < count_; i++)
            sorted[i] = ValueOf(data_[Index(i)]);
        // insertion sort
        for (size_t i = 1; i < count_; i++) {
            double key = sorted[i];
            size_t j = i;
            while (j > 0 && sorted[j - 1] > key) {
                sorted[j] = sorted[j - 1];
                j--;
            }
            sorted[j] = key;
        }
        size_t idx = static_cast<size_t>(p * static_cast<double>(count_ - 1));
        if (idx >= count_) idx = count_ - 1;
        return sorted[idx];
    }

    // ── TaggedSample-specific methods ──

    // Mean of samples matching predicate
    template<typename Pred>
    double MeanWhere(Pred pred) const {
        double sum = 0.0;
        size_t n = 0;
        for (size_t i = 0; i < count_; i++) {
            const auto& s = data_[Index(i)];
            if (pred(s)) {
                sum += ValueOf(s);
                n++;
            }
        }
        return n > 0 ? sum / static_cast<double>(n) : 0.0;
    }

    // Return filtered subset as a new RollingWindow
    template<typename Pred>
    RollingWindow<T, N> Where(Pred pred) const {
        RollingWindow<T, N> result;
        for (size_t i = 0; i < count_; i++) {
            const auto& s = data_[Index(i)];
            if (pred(s))
                result.Push(s);
        }
        return result;
    }

    // Mean of all samples (ignoring tags)
    double MeanAll() const { return Mean(); }

    // Percentile of all samples (ignoring tags)
    double PercentileAll(double p) const { return Percentile(p); }

    // Latest value (double extraction from T)
    double LatestValue() const {
        if (count_ == 0) return 0.0;
        return ValueOf(Latest());
    }

private:
    T data_[N]{};
    size_t head_ = 0;
    size_t count_ = 0;
    bool just_wrapped_ = false;

    // Map circular index to array index (0 = oldest)
    size_t Index(size_t i) const {
        if (count_ < N)
            return i;
        return (head_ + i) % N;
    }

    // Extract double value from T
    static double ValueOf(const double& v) { return v; }
    static double ValueOf(const TaggedSample& v) { return v.value; }
};

// ── RingBuffer<T, N> ──
// Simple fixed-capacity ring for the correlator. Supports Push, Get by
// absolute sequence index, Clear, and size tracking.

template<typename T, size_t N>
class RingBuffer {
public:
    void Push(const T& val) {
        data_[write_ % N] = val;
        write_++;
    }

    // Get by absolute sequence number
    const T& Get(uint64_t seq) const {
        return data_[seq % N];
    }

    void Clear() {
        write_ = 0;
    }

    uint64_t WriteCount() const { return write_; }

    size_t Capacity() const { return N; }

private:
    T data_[N]{};
    uint64_t write_ = 0;
};
