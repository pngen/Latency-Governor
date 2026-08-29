#pragma once

#include "latency_governor/clock.hpp"
#include "latency_governor/duration.hpp"
#include "latency_governor/enums.hpp"
#include "latency_governor/identifiers.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace latency_governor {

// A bounded latency histogram (fixed buckets, no unbounded accumulation).
class Histogram {
public:
    static constexpr std::size_t kBuckets = 32;

    void record(Duration d) noexcept;
    [[nodiscard]] std::uint64_t count(std::size_t bucket) const noexcept { return counts_[bucket]; }
    [[nodiscard]] std::uint64_t total() const noexcept { return total_; }
    [[nodiscard]] Duration max_value() const noexcept { return max_value_; }
    void reset() noexcept { counts_.fill(0); total_ = 0; max_value_ = Duration::zero(); }

private:
    std::array<std::uint64_t, kBuckets> counts_{};
    std::uint64_t total_ = 0;
    Duration max_value_{0};
};

// Aggregate, bounded governor counters for one epoch / window.
struct MetricsSummary {
    std::uint64_t active = 0;
    std::uint64_t completed = 0;
    std::uint64_t failed = 0;
    std::uint64_t cancelled = 0;

    std::uint64_t slo_met = 0;
    std::uint64_t soft_violation = 0;
    std::uint64_t hard_violation = 0;

    std::array<std::uint64_t, SloClass_count> per_class{};
    std::array<std::uint64_t, InterventionAction_count> interventions{};
    std::array<std::uint64_t, AdmissionVerdict_count> admissions{};
    std::array<std::uint64_t, RejectionCode_count> stale_rejections{};

    std::uint64_t observations_received = 0;
    std::uint64_t observations_rejected = 0;
    std::uint64_t predictors_updated = 0;
    std::uint64_t prediction_error_count = 0;
    double prediction_error_mean_us = 0.0;
    double prediction_error_var_us = 0.0;
    std::uint64_t decisions = 0;

    Histogram phase_latency[Phase_count];
    Histogram decode_step_latency;
    Histogram prefill_chunk_latency;
};

// A compact snapshot of one active request for observability.
struct RequestSnapshot {
    RequestId request_id;
    AttemptId attempt_id;
    LifecycleState lifecycle = LifecycleState::ADMITTED;
    Phase phase = Phase::ADMISSION;
    SloClass slo_class = SloClass::STANDARD;
    TenantId tenant_id;
    RiskState risk = RiskState::SAFE;
    Duration remaining_budget{0};
    Duration elapsed_total{0};
    std::uint64_t generation = 0;
    std::uint64_t epoch = 0;
};

// A single bounded event in the runtime event stream.
struct EventRecord {
    std::uint64_t sequence = 0;
    TimePoint at{};
    std::string kind;   // e.g. "admission", "intervention", ...
    std::string detail;
};

// A full inspectable snapshot of governor state.
struct Snapshot {
    TimePoint at{};
    std::uint64_t coordinator_epoch = 0;
    std::uint64_t decision_generation = 0;
    std::uint64_t event_sequence = 0;
    MetricsSummary summary;
    std::vector<RequestSnapshot> requests;   // bounded
    std::vector<EventRecord> events;         // bounded
    std::string json;                        // canonical text form
};

} // namespace latency_governor
