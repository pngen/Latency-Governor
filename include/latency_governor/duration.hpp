#pragma once

#include <chrono>
#include <cstdint>
#include <limits>

namespace latency_governor {

// The canonical representation of latency across the runtime. Every threshold,
// budget, prediction, and measurement is expressed as a signed nanosecond
// duration measured on the monotonic clock.
using Duration = std::chrono::nanoseconds;

// A byte count for request/resource sizing.
using Bytes = std::uint64_t;

// Saturating duration arithmetic. Latency-governance decisions must be robust
// to adversarial or overflow-prone arithmetic; these helpers guarantee that the
// result never wraps and never becomes a nonsensical negative duration in a
// path where non-negativity is a semantic invariant.
[[nodiscard]] inline Duration sat_add(Duration a, Duration b) noexcept {
    const auto x = a.count(), y = b.count();
    if (x > 0 && y > std::numeric_limits<std::int64_t>::max() - x) {
        return Duration(std::numeric_limits<std::int64_t>::max());
    }
    if (x < 0 && y < std::numeric_limits<std::int64_t>::min() - x) {
        return Duration(std::numeric_limits<std::int64_t>::min());
    }
    return Duration(x + y);
}

// Saturating subtraction, clamped at zero for the common latency path where a
// non-negative remaining duration is required.
[[nodiscard]] inline Duration sat_sub(Duration a, Duration b) noexcept {
    const auto x = a.count(), y = b.count();
    if (y < 0 && x > std::numeric_limits<std::int64_t>::max() + y) {
        return Duration(std::numeric_limits<std::int64_t>::max());
    }
    const auto r = x - y;
    return Duration(r < 0 ? 0 : r);
}

// Non-negative clamp.
[[nodiscard]] inline Duration clamp_nonneg(Duration d) noexcept {
    return d < Duration::zero() ? Duration::zero() : d;
}

// Percentage of x relative to y, clamped to [0,1]. Used for budget-consumption
// ratios and risk scoring.
[[nodiscard]] inline double fraction(double x, double y) noexcept {
    if (y <= 0.0) return 0.0;
    const double r = x / y;
    return r < 0.0 ? 0.0 : (r > 1.0 ? 1.0 : r);
}

// A nanosecond duration from an integer count.
[[nodiscard]] constexpr Duration ns(std::int64_t count) noexcept { return Duration(count); }
[[nodiscard]] constexpr Duration us(std::int64_t count) noexcept { return std::chrono::microseconds(count); }
[[nodiscard]] constexpr Duration ms(std::int64_t count) noexcept { return std::chrono::milliseconds(count); }
[[nodiscard]] constexpr Duration seconds(std::int64_t count) noexcept { return std::chrono::seconds(count); }

} // namespace latency_governor
