#include "latency_governor/risk.hpp"

#include <algorithm>
#include <cmath>

namespace latency_governor {

namespace {
constexpr double kWaitHigh = 0.6;
constexpr double kRiskHigh = 0.85;
constexpr double kRiskAtRisk = 0.60;
constexpr double kRiskWatch = 0.35;

void add_contrib(RiskAssessment& r, ReasonCode reason, double weight, double threshold, double observed) {
    if (weight < 0.001) return;
    r.contributions.push_back({reason, weight, threshold, observed});
}
} // namespace

RiskAssessment evaluate_risk(const RequestState& req, const Policy& policy, double resource_pressure) noexcept {
    RiskAssessment r;
    const double rp = std::clamp(resource_pressure, 0.0, 1.0);
    r.resource_pressure = rp;

    const Duration cap = req.descriptor.contract.hard_deadline.value_or(req.descriptor.contract.e2e_target);
    const Duration remaining = req.remaining_budget();
    const double consumed = static_cast<double>(ns_count(req.elapsed_total));
    const double cap_ns = static_cast<double>(ns_count(cap));
    r.remaining_budget = remaining;
    r.budget_used_fraction = (cap_ns > 0.0) ? std::clamp(consumed / cap_ns, 0.0, 1.0) : 0.0;
    r.hard_deadline_reached = req.hard_deadline_exceeded();

    // Prediction contribution.
    if (req.predicted_remaining) {
        const auto& p = *req.predicted_remaining;
        r.prediction = p;
        const double pred_ns = static_cast<double>(ns_count(p.predicted));
        const double rem_ns = static_cast<double>(ns_count(remaining));
        r.predicted_vs_budget = (rem_ns > 0.0) ? std::clamp(pred_ns / rem_ns, 0.0, 1.0)
                                               : (pred_ns > 0.0 ? 1.0 : 0.0);
        r.prediction_low_confidence = p.confidence < ConfidenceClass::MEDIUM;
    } else {
        // No evidence: explicit cold-start uncertainty.
        r.prediction_low_confidence = true;
        r.predicted_vs_budget = 0.0;
    }

    // Per-phase flags.
    const auto& b = req.phase_budgets;
    const auto cttf = req.descriptor.contract.ttf_target;
    r.ttf_at_risk = (cttf > Duration::zero()) &&
                    (b.consumed(Phase::PREFILL) > cttf || b.consumed(Phase::QUEUEING) > cttf);
    const auto cds = req.descriptor.contract.decode_step_target;
    r.inter_token_at_risk = (cds > Duration::zero()) && (b.consumed(Phase::DECODE) > cds);
    const auto cqr = req.descriptor.contract.max_queue_residence;
    r.batch_wait_at_risk = (cqr > Duration::zero() && req.batch_wait > cqr) ||
                           (policy.batch.default_max_batch_wait > Duration::zero() &&
                            req.batch_wait > policy.batch.default_max_batch_wait);
    const auto cta = req.descriptor.contract.transfer_allowance;
    r.transfer_risk = (cta > Duration::zero()) && (b.consumed(Phase::TRANSFER) > cta);
    const auto cra = req.descriptor.contract.retry_allowance;
    r.retry_risk = (cra > Duration::zero() && req.retry_accumulated > cra) ||
                   (req.descriptor.contract.max_retry_delay > Duration::zero() &&
                    req.retry_accumulated > req.descriptor.contract.max_retry_delay);
    r.speculation_risk = (policy.speculation.default_max_depth > 0 && req.spec_depth > policy.speculation.default_max_depth);
    r.resource_pressure_high = rp > kWaitHigh;

    // Aggregate deadline-risk score (deterministic, component-based).
    double deadline_risk = 0.0;
    deadline_risk += 0.30 * r.budget_used_fraction;
    deadline_risk += 0.30 * r.predicted_vs_budget;
    deadline_risk += 0.15 * rp;
    deadline_risk += 0.15 * (r.prediction_low_confidence ? 1.0 : 0.0);
    if (r.hard_deadline_reached) deadline_risk += 0.10;
    deadline_risk = std::clamp(deadline_risk, 0.0, 1.0);
    r.deadline_risk = deadline_risk;
    r.uncertainty_penalty = r.prediction_low_confidence ? 0.15 : 0.0;
    r.completion_probability = std::clamp(1.0 - deadline_risk - r.uncertainty_penalty, 0.0, 1.0);

    // State classification.
    if (r.hard_deadline_reached && r.completion_probability <= 0.05) {
        r.state = RiskState::INFEASIBLE;
    } else if (r.hard_deadline_reached || r.budget_used_fraction >= 1.0) {
        r.state = RiskState::CRITICAL;
    } else if (deadline_risk >= kRiskHigh) {
        r.state = RiskState::CRITICAL;
    } else if (deadline_risk >= kRiskAtRisk) {
        r.state = RiskState::AT_RISK;
    } else if (deadline_risk >= kRiskWatch) {
        r.state = RiskState::WATCH;
    } else {
        r.state = RiskState::SAFE;
    }

    // Contributions (why is this request at this risk?).
    add_contrib(r, ReasonCode::DEADLINE_SLACK, 0.30 * r.budget_used_fraction, 1.0, r.budget_used_fraction);
    add_contrib(r, ReasonCode::PREDICTED_LATE, 0.30 * r.predicted_vs_budget, 1.0, r.predicted_vs_budget);
    if (r.hard_deadline_reached) add_contrib(r, ReasonCode::HARD_DEADLINE_EXPIRED, 0.10, 0.0, 1.0);
    if (r.prediction_low_confidence) add_contrib(r, ReasonCode::PREDICTION_LOW_CONFIDENCE, 0.15, 0.0, 1.0);
    add_contrib(r, ReasonCode::RESOURCE_PRESSURE_HIGH, 0.15 * rp, kWaitHigh, rp);
    if (r.batch_wait_at_risk) add_contrib(r, ReasonCode::BATCH_WAIT_EXCEEDED, 0.15,
                                          static_cast<double>(ns_count(req.descriptor.contract.max_queue_residence)),
                                          static_cast<double>(ns_count(req.batch_wait)));
    if (r.ttf_at_risk) add_contrib(r, ReasonCode::TTFT_AT_RISK, 0.15,
                                   static_cast<double>(ns_count(req.descriptor.contract.ttf_target)),
                                   static_cast<double>(ns_count(b.consumed(Phase::PREFILL))));
    if (r.inter_token_at_risk) add_contrib(r, ReasonCode::INTER_TOKEN_AT_RISK, 0.15,
                                           static_cast<double>(ns_count(req.descriptor.contract.decode_step_target)),
                                           static_cast<double>(ns_count(b.consumed(Phase::DECODE))));
    if (r.transfer_risk) add_contrib(r, ReasonCode::TRANSFER_COST_EXCEEDED, 0.15,
                                     static_cast<double>(ns_count(req.descriptor.contract.transfer_allowance)),
                                     static_cast<double>(ns_count(b.consumed(Phase::TRANSFER))));
    if (r.retry_risk) add_contrib(r, ReasonCode::RETRY_RESERVE_EXHAUSTED, 0.15,
                                  static_cast<double>(ns_count(req.descriptor.contract.retry_allowance)),
                                  static_cast<double>(ns_count(req.retry_accumulated)));
    if (r.speculation_risk) add_contrib(r, ReasonCode::SPECULATION_OVERHEAD, 0.10,
                                        static_cast<double>(policy.speculation.default_max_depth),
                                        static_cast<double>(req.spec_depth));

    // Stable ordering: largest weight first; tie-break by reason code.
    std::stable_sort(r.contributions.begin(), r.contributions.end(),
                     [](const auto& a, const auto& b) {
                         if (a.weight != b.weight) return a.weight > b.weight;
                         return static_cast<int>(a.reason) < static_cast<int>(b.reason);
                     });
    return r;
}

} // namespace latency_governor
