#pragma once

#include "latency_governor/clock.hpp"
#include "latency_governor/duration.hpp"
#include "latency_governor/enums.hpp"
#include "latency_governor/identifiers.hpp"
#include "latency_governor/prediction.hpp"

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace latency_governor {

constexpr std::size_t kPhaseCount = Phase_count;

// ---------------------------------------------------------------------------
// LatencyContract
//
// The explicit latency obligation a request carries. This is deliberately NOT
// a single target timestamp: it distinguishes target latency, soft deadline,
// hard deadline, per-phase budgets, retry allowance, speculation overhead, and
// the tolerances that decide when an intervention is warranted.
// ---------------------------------------------------------------------------
struct LatencyContract {
    std::uint64_t policy_id = 0;

    // End-to-end relative target. This is the primary scalar SLO.
    Duration e2e_target{0};

    // Optional hard deadline, expressed as a duration from arrival.
    std::optional<Duration> hard_deadline;

    // Time-to-first-token target.
    Duration ttf_target{0};

    // Queueing budgets.
    Duration max_queue_residence{0};
    Duration max_dispatch_delay{0};

    // Phase targets.
    Duration prefill_target{0};
    Duration decode_step_target{0};
    Duration transfer_allowance{0};
    Duration retry_allowance{0};
    Duration max_retry_delay{0};
    Duration max_spec_overhead{0};

    // Tolerances.
    double deadline_risk_threshold = 0.10;
    double min_completion_probability = 0.0;
    double intervention_aggressiveness = 0.5;
    double fairness_weight = 1.0;

    // Behavioral flags.
    AdmissionPolicy admission_policy = AdmissionPolicy::STRICT;
    CancellationPolicy cancellation_policy = CancellationPolicy::FAIL_FAST;
    bool degradation_allowed = true;

    [[nodiscard]] bool validate(std::string& error) const;
};

// ---------------------------------------------------------------------------
// BudgetPartition
//
// The explicit or dynamically-allocated division of the end-to-end budget
// across phases. Dynamic allocation must preserve the total-budget invariant.
// ---------------------------------------------------------------------------
struct BudgetPartition {
    std::array<Duration, kPhaseCount> allocation{};
    Duration reserve{0};

    [[nodiscard]] Duration total() const noexcept {
        Duration t{0};
        for (auto& d : allocation) t = sat_add(t, d);
        return sat_add(t, reserve);
    }
};

// ---------------------------------------------------------------------------
// PhaseBudgets
//
// Deterministic latency-budget accounting across request phases, with
// roll-forward / donation preserving total-budget invariants.
// ---------------------------------------------------------------------------
class PhaseBudgets {
public:
    PhaseBudgets() = default;
    explicit PhaseBudgets(const BudgetPartition& p) : partition_(p) {}

    void set_partition(const BudgetPartition& p) { partition_ = p; }
    [[nodiscard]] const BudgetPartition& partition() const noexcept { return partition_; }

    void attribute(Phase phase, Duration elapsed) noexcept;

    [[nodiscard]] Duration consumed(Phase phase) const noexcept;
    [[nodiscard]] Duration allocated(Phase phase) const noexcept {
        return partition_.allocation[enum_index(phase)];
    }
    [[nodiscard]] Duration remaining(Phase phase) const noexcept;
    [[nodiscard]] bool overspent(Phase phase) const noexcept {
        return consumed(phase) > allocated(phase);
    }

    [[nodiscard]] Duration total_consumed() const noexcept;
    [[nodiscard]] Duration total_consumed_with_overhead() const noexcept {
        return sat_add(total_consumed(), consumed(Phase::UNCLASSIFIED));
    }

    void donate(Phase from, Phase to, Duration amount) noexcept;
    void roll_forward(Phase from, Phase to) noexcept {
        const auto rem = remaining(from);
        if (rem > Duration::zero()) donate(from, to, rem);
    }

    void reset_consumed() noexcept { consumed_.fill(Duration::zero()); }

private:
    BudgetPartition partition_;
    std::array<Duration, kPhaseCount> consumed_{};
};

// ---------------------------------------------------------------------------
// Reservation
// ---------------------------------------------------------------------------
struct Reservation {
    ReservationId id;
    BackendId backend;
    Bytes amount = 0;
    bool released = false;
};

// ---------------------------------------------------------------------------
// InterventionRecord
// ---------------------------------------------------------------------------
struct InterventionRecord {
    InterventionAction action = InterventionAction::CONTINUE;
    ReasonCode reason = ReasonCode::DEADLINE_SLACK;
    Duration remaining_budget{0};
    Duration predicted_remaining{0};
    RiskState risk_before = RiskState::SAFE;
    std::uint64_t policy_generation = 0;
    std::uint64_t decision_generation = 0;
    TimePoint at{};
    std::string detail;
};

// ---------------------------------------------------------------------------
// RequestDescriptor
//
// Immutable identity and contract for a request.
// ---------------------------------------------------------------------------
struct RequestDescriptor {
    RequestId request_id;
    TenantId tenant_id;
    ModelId model_id;
    ModelRevision model_revision;
    std::optional<AdapterId> adapter_id;

    SloClass slo_class = SloClass::STANDARD;
    LatencyContract contract;

    std::uint32_t prompt_tokens = 0;
    std::uint32_t max_tokens = 0;
    std::uint32_t remaining_tokens = 0;

    std::optional<BackendId> backend_hint;
    DeviceClass device_hint = DeviceClass::UNKNOWN;

    bool warm_cache_hint = false;
    TimePoint arrival{};

    [[nodiscard]] bool validate(std::string& error) const;
};

// ---------------------------------------------------------------------------
// RequestState
//
// Mutable runtime state governing an active request. This is the only object
// against which authoritative latency state is read and written. A request may
// not mutate this state through stale authority of any kind.
// ---------------------------------------------------------------------------
struct RequestState {
    RequestId request_id;
    RequestDescriptor descriptor;

    // Authority envelope.
    AttemptId attempt_id;
    std::optional<DispatchId> dispatch_id;
    Generation generation{0};
    CoordinatorEpoch epoch{0};

    // Lifecycle.
    LifecycleState lifecycle = LifecycleState::ADMITTED;
    Phase current_phase = Phase::ADMISSION;
    TimePoint admitted_at{};
    TimePoint last_update{};
    TimePoint phase_started_at{};

    // Accounting.
    PhaseBudgets phase_budgets;
    Duration elapsed_total{0};
    Duration queue_residence{0};
    Duration batch_wait{0};
    Duration current_phase_elapsed{0};

    // Execution identity.
    std::optional<WorkerId> worker_id;
    std::optional<WorkerBootId> worker_boot_id;
    std::optional<BackendId> backend_id;
    DeviceClass device{DeviceClass::UNKNOWN};

    // Speculation.
    std::uint32_t spec_depth = 0;
    std::uint32_t spec_branches = 0;
    bool speculation_enabled = true;
    double spec_acceptance_rate = 0.0;
    bool speculation_was_reduced = false;

    // Retry.
    std::uint32_t retry_count = 0;
    Duration retry_accumulated{0};

    // Predictions / risk (last computed decision).
    std::optional<Prediction> predicted_remaining;
    RiskState risk = RiskState::SAFE;
    double completion_probability = 0.0;

    // Fairness.
    Duration service_consumed{0};
    std::uint32_t interventions_received = 0;
    Duration starvation_age{0};
    TimePoint last_active{};
    std::uint32_t active_priority = 0;

    // Resources.
    std::vector<Reservation> reservations;
    std::vector<InterventionRecord> interventions;

    // Terminal outcome.
    std::optional<RejectionCode> terminal_reason;
    TimePoint completed_at{};
    bool slo_met = false;
    bool soft_violation = false;
    bool hard_violation = false;

    [[nodiscard]] bool is_active() const noexcept {
        return lifecycle != LifecycleState::COMPLETED &&
               lifecycle != LifecycleState::FAILED &&
               lifecycle != LifecycleState::CANCELLED;
    }
    [[nodiscard]] bool is_terminal() const noexcept { return !is_active(); }

    // Remaining end-to-end budget = cap - consumed. The cap is the e2e target
    // or the hard deadline, whichever is binding.
    [[nodiscard]] Duration remaining_budget() const noexcept;
    [[nodiscard]] bool hard_deadline_exceeded() const noexcept;
};

} // namespace latency_governor