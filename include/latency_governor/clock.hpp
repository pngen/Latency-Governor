#pragma once

#include <chrono>
#include <cstdint>
#include <string_view>

namespace latency_governor {

// All latency-governor runtime measurements use a monotonic clock.
//
// Wall-clock time is not monotonic (it can jump with NTP, DST, or manual
// changes) and must never be used for deadlines, elapsed-duration accounting,
// comparisons, or timeout-equivalent policy decisions. Wall time is only used
// for human-readable telemetry labels.
//
// The runtime uses a nanosecond-based monotonic clock. On Windows this maps to
// a high-resolution monotonic source; on POSIX to CLOCK_MONOTONIC.
using ClockDuration = std::chrono::nanoseconds;
using TimePoint = std::chrono::time_point<std::chrono::steady_clock, ClockDuration>;

// A wall-clock instant, expressed as a count of milliseconds since the Unix
// epoch. This is for telemetry and trace labels only. It is never used for any
// latency-governance arithmetic or comparison, and is never authoritative.
using WallMs = std::int64_t;

// Convert a steady_clock time_point into the nanoseconds-based monotonic
// domain used by the runtime.
[[nodiscard]] inline TimePoint mono_now() noexcept {
    return std::chrono::time_point_cast<ClockDuration>(std::chrono::steady_clock::now());
}

// Compute elapsed nanoseconds between two monotonic time points, saturating at
// zero if the arguments are inverted (a clock-order anomaly supplied by a bad
// caller should never produce a negative elapsed duration that corrupts
// accounting).
[[nodiscard]] inline ClockDuration saturating_elapsed(const TimePoint& start, const TimePoint& end) noexcept {
    const auto d = end - start;
    if (d < ClockDuration::zero()) return ClockDuration::zero();
    return d;
}

// Convert a monotonic duration to a count of nanoseconds.
[[nodiscard]] inline std::int64_t ns_count(const ClockDuration& d) noexcept {
    return d.count();
}

// Convert a monotonic duration to a count of microseconds.
[[nodiscard]] inline std::int64_t us_count(const ClockDuration& d) noexcept {
    return std::chrono::duration_cast<std::chrono::microseconds>(d).count();
}

// Convert a monotonic duration to a count of milliseconds.
[[nodiscard]] inline std::int64_t ms_count(const ClockDuration& d) noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(d).count();
}

// A wall-clock epoch millisecond label for telemetry only.
[[nodiscard]] std::int64_t wall_epoch_ms() noexcept;

// Clock is an injectable abstraction so that tests can drive deterministic
// monotonic time. The default is the real system clock.
class Clock {
public:
    virtual ~Clock() = default;

    // Current monotonic instant.
    [[nodiscard]] virtual TimePoint now() const noexcept = 0;

    // Wall-clock label (telemetry only). Never used for governance.
    [[nodiscard]] virtual std::int64_t wall_ms() const noexcept { return wall_epoch_ms(); }
};

// The real monotonic clock.
class SystemClock final : public Clock {
public:
    [[nodiscard]] TimePoint now() const noexcept override { return mono_now(); }
};

// A deterministic, manually advanced clock for tests and reproducibility.
class ManualClock final : public Clock {
public:
    ManualClock() : now_{} {}
    explicit ManualClock(TimePoint start) : now_(start) {}

    // Advance time by the given duration (or by zero/negative, which clamps to
    // no-op so tests cannot accidentally rewind the monotonic source).
    void advance(ClockDuration d) noexcept {
        if (d > ClockDuration::zero()) now_ += d;
    }
    void advance_us(std::int64_t us) noexcept { advance(std::chrono::microseconds(us)); }
    void advance_ms(std::int64_t ms) noexcept { advance(std::chrono::milliseconds(ms)); }

    // Directly set the instant (useful for sequencing discrete events).
    void set(TimePoint t) noexcept { now_ = t; }

    [[nodiscard]] TimePoint now() const noexcept override { return now_; }

private:
    TimePoint now_;
};

} // namespace latency_governor
