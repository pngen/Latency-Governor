#include "latency_governor/request.hpp"

#include <algorithm>
#include <string>

namespace latency_governor {

namespace {
constexpr std::size_t kInvalidPhase = kPhaseCount;

[[nodiscard]] Duration budget_cap(const LatencyContract& c) noexcept {
    if (c.hard_deadline) return *c.hard_deadline;
    return c.e2e_target;
}
} // namespace

bool LatencyContract::validate(std::string& error) const {
    auto nonneg = [&](const char* name, Duration d) -> bool {
        if (d < Duration::zero()) {
            error = std::string(name) + " must be non-negative";
            return false;
        }
        return true;
    };
    if (!nonneg("e2e_target", e2e_target)) return false;
    if (!nonneg("ttf_target", ttf_target)) return false;
    if (!nonneg("max_queue_residence", max_queue_residence)) return false;
    if (!nonneg("max_dispatch_delay", max_dispatch_delay)) return false;
    if (!nonneg("prefill_target", prefill_target)) return false;
    if (!nonneg("decode_step_target", decode_step_target)) return false;
    if (!nonneg("transfer_allowance", transfer_allowance)) return false;
    if (!nonneg("retry_allowance", retry_allowance)) return false;
    if (!nonneg("max_retry_delay", max_retry_delay)) return false;
    if (!nonneg("max_spec_overhead", max_spec_overhead)) return false;
    if (hard_deadline && *hard_deadline < Duration::zero()) {
        error = "hard_deadline must be non-negative";
        return false;
    }
    // A hard deadline that is tighter than the soft target is not a contradiction
    // (a hard deadline may be absent), but a deadline strictly before the e2e
    // target is inconsistent with a target SLO and is rejected.
    if (hard_deadline && *hard_deadline > Duration::zero() && e2e_target > *hard_deadline) {
        error = "e2e_target cannot exceed hard_deadline";
        return false;
    }
    const bool r = [&]{
        if (deadline_risk_threshold < 0.0 || deadline_risk_threshold > 1.0) return false;
        if (min_completion_probability < 0.0 || min_completion_probability > 1.0) return false;
        if (intervention_aggressiveness < 0.0 || intervention_aggressiveness > 1.0) return false;
        if (fairness_weight < 0.0) return false;
        return true;
    }();
    if (!r) {
        error = "contract tolerance/weight out of range [0,1]";
        return false;
    }
    return true;
}

bool RequestDescriptor::validate(std::string& error) const {
    if (!request_id.is_valid()) { error = "request_id is null"; return false; }
    if (!tenant_id.is_valid()) { error = "tenant_id is null"; return false; }
    if (!model_id.is_valid()) { error = "model_id is null"; return false; }
    if (prompt_tokens == 0 && max_tokens == 0) { error = "request has no token sizing"; return false; }
    // remaining_tokens is derived at admission, not during validation (validate is const).
    return contract.validate(error);
}

void PhaseBudgets::attribute(Phase phase, Duration elapsed) noexcept {
    const auto idx = enum_index(phase);
    if (idx >= kPhaseCount) return;
    if (elapsed < Duration::zero()) return;   // clock-order anomaly: ignore
    consumed_[idx] = sat_add(consumed_[idx], elapsed);
}

Duration PhaseBudgets::consumed(Phase phase) const noexcept {
    const auto idx = enum_index(phase);
    if (idx >= kPhaseCount) return Duration::zero();
    return consumed_[idx];
}

Duration PhaseBudgets::remaining(Phase phase) const noexcept {
    const auto idx = enum_index(phase);
    if (idx >= kPhaseCount) return Duration::zero();
    return clamp_nonneg(sat_sub(partition_.allocation[idx], consumed_[idx]));
}

Duration PhaseBudgets::total_consumed() const noexcept {
    Duration t{0};
    for (std::size_t i = 0; i < kPhaseCount; ++i) {
        if (i == enum_index(Phase::UNCLASSIFIED)) continue;
        t = sat_add(t, consumed_[i]);
    }
    return t;
}

void PhaseBudgets::donate(Phase from, Phase to, Duration amount) noexcept {
    const auto fi = enum_index(from);
    const auto ti = enum_index(to);
    if (fi >= kPhaseCount || ti >= kPhaseCount) return;
    if (from == to) return;
    if (amount < Duration::zero()) return;
    const auto avail = remaining(from);
    const auto actual = std::min(amount, avail);
    if (actual <= Duration::zero()) return;
    // Move budget: reduce allocation of from by actual (clamped), increase to.
    partition_.allocation[fi] = clamp_nonneg(sat_sub(partition_.allocation[fi], actual));
    partition_.allocation[ti] = sat_add(partition_.allocation[ti], actual);
}

Duration RequestState::remaining_budget() const noexcept {
    const auto cap = budget_cap(descriptor.contract);
    return clamp_nonneg(sat_sub(cap, elapsed_total));
}

bool RequestState::hard_deadline_exceeded() const noexcept {
    return elapsed_total > budget_cap(descriptor.contract);
}

} // namespace latency_governor