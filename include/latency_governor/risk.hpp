#pragma once

#include "latency_governor/clock.hpp"
#include "latency_governor/duration.hpp"
#include "latency_governor/enums.hpp"
#include "latency_governor/prediction.hpp"
#include "latency_governor/request.hpp"
#include "latency_governor/policy.hpp"

#include <optional>
#include <vector>

namespace latency_governor {

// RiskAssessment is the result of evaluating whether a request's latency
// obligation is currently and prospectively honor-able. The state is one of a
// small, stable set; the components make the assessment fully explainable.
struct RiskAssessment {
    RiskState state = RiskState::SAFE;

    double budget_used_fraction = 0.0;
    Duration remaining_budget{0};
    std::optional<Prediction> prediction;
    double predicted_vs_budget = 0.0;
    bool hard_deadline_reached = false;
    bool ttf_at_risk = false;
    bool inter_token_at_risk = false;
    bool batch_wait_at_risk = false;
    bool transfer_risk = false;
    bool retry_risk = false;
    bool speculation_risk = false;
    bool resource_pressure_high = false;
    bool prediction_low_confidence = false;

    double deadline_risk = 0.0;
    double completion_probability = 0.0;
    double resource_pressure = 0.0;
    double uncertainty_penalty = 0.0;

    struct Contribution {
        ReasonCode reason;
        double weight;
        double threshold;
        double observed;
    };
    std::vector<Contribution> contributions;
};

// Deterministic risk evaluation over a request, a policy, and a current
// resource-pressure signal in [0,1]. The contributing components are exposed.
[[nodiscard]] RiskAssessment evaluate_risk(const RequestState& request,
                                           const Policy& policy,
                                           double resource_pressure) noexcept;

} // namespace latency_governor
