#pragma once

#include "latency_governor/clock.hpp"
#include "latency_governor/duration.hpp"
#include "latency_governor/enums.hpp"
#include "latency_governor/identifiers.hpp"
#include "latency_governor/risk.hpp"

#include <optional>
#include <string>
#include <vector>

namespace latency_governor {

// A single typed, explainable governor decision.
struct Intervention {
    InterventionAction action = InterventionAction::CONTINUE;
    ReasonCode reason = ReasonCode::DEADLINE_SLACK;
    RequestId request_id;
    AttemptId attempt_id;
    Phase phase = Phase::UNCLASSIFIED;

    Duration remaining_budget{0};
    Duration predicted_remaining{0};
    RiskState risk = RiskState::SAFE;
    double threshold = 0.0;
    double observed = 0.0;

    std::uint64_t policy_generation = 0;
    std::uint64_t decision_generation = 0;
    TimePoint at{};
    std::optional<WorkerId> target_worker;
    std::vector<ReasonCode> supporting_reasons;
    std::string detail;
};

// The set of interventions chosen for a request in one decision pass.
struct InterventionPlan {
    std::vector<Intervention> items;
    std::uint64_t decision_generation = 0;
};

// Admission feasibility assessment.
struct AdmissionAssessment {
    AdmissionVerdict verdict = AdmissionVerdict::INFEASIBLE;
    RiskState risk = RiskState::INFEASIBLE;
    Duration estimated_queue_wait{0};
    Duration estimated_total{0};
    Duration deadline_slack{0};
    double completion_probability = 0.0;
    bool deferable = false;
    bool fail_fast = false;
    std::vector<ReasonCode> reasons;
    std::string detail;
};

} // namespace latency_governor
