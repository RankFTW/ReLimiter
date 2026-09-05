#pragma once

#include <cmath>
#include <cstdint>
#include <limits>

// Converts a sequence of microsecond intervals into QPC steps while carrying
// sub-tick remainder. Reset whenever the owning deadline phase reanchors.
class FractionalQpcInterval {
public:
    int64_t Next(double interval_us, int64_t qpc_frequency) noexcept
    {
        if (!std::isfinite(interval_us) || interval_us <= 0.0 ||
            qpc_frequency <= 0) {
            return 0;
        }
        const long double exact_qpc =
            static_cast<long double>(interval_us) *
            static_cast<long double>(qpc_frequency) / 1000000.0L +
            remainder_qpc_;
        if (exact_qpc > static_cast<long double>(
                (std::numeric_limits<int64_t>::max)())) {
            return 0;
        }
        const long double whole_qpc = std::floor(exact_qpc);
        remainder_qpc_ = exact_qpc - whole_qpc;
        return static_cast<int64_t>(whole_qpc);
    }

    void Reset() noexcept { remainder_qpc_ = 0.0L; }
    long double Remainder() const noexcept { return remainder_qpc_; }

private:
    long double remainder_qpc_ = 0.0L;
};
