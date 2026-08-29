// 08_transfer_vs_local.cpp
// govern_transfer with a large (fraction-of-budget exceeding) predicted transfer
// prefers local execution.
//
// Links: latency_governor (core only).
#include "latency_governor/governor.hpp"

#include <cstdio>

using namespace latency_governor;

[[maybe_unused]] static Policy make_policy() {
    Policy p;
    p.name = "default";
    auto add = [&](const char* name, double prio, Duration e2e, Duration ttf, double risk) {
        SloClassSpec s;
        s.name = name;
        s.priority = prio;
        s.default_e2e_target = e2e;
        s.default_ttf_target = ttf;
        s.default_deadline_risk_threshold = risk;
        s.default_min_completion_probability = 0.0;
        s.default_fairness_weight = 1.0;
        s.default_degradation_allowed = true;
        s.phase_weights[enum_index(Phase::QUEUEING)] = 0.05;
        s.phase_weights[enum_index(Phase::BATCH_WAIT)] = 0.05;
        s.phase_weights[enum_index(Phase::PREFILL)] = 0.30;
        s.phase_weights[enum_index(Phase::DECODE)] = 0.50;
        s.phase_weights[enum_index(Phase::TRANSFER)] = 0.05;
        s.reserve_fraction = 0.05;
        p.classes.push_back(s);
    };
    add("REALTIME", 5.0, ms(100), ms(20), 0.05);
    add("INTERACTIVE", 4.0, ms(250), ms(50), 0.10);
    add("STANDARD", 3.0, ms(500), ms(100), 0.10);
    add("THROUGHPUT", 2.0, ms(1000), ms(200), 0.10);
    add("BACKGROUND", 1.0, seconds(2), ms(400), 0.10);
    p.retry.max_retries = 3;
    p.retry.max_cumulative_retry_delay = ms(500);
    p.retry.allow_immediate_retry = true;
    p.transfer.max_transfer_fraction_of_budget = 0.20;
    p.transfer.prefer_local_when_risky = true;
    p.speculation.default_max_depth = 2;
    p.speculation.min_acceptance_rate = 0.10;
    p.speculation.max_overhead_fraction_of_budget = 0.10;
    p.fairness.max_starvation_ratio = 2;
    p.fairness.background_min_service_fraction = 0.05;
    p.fairness.protect_lower_classes_strongly = true;
    p.batch.default_max_batch_wait = ms(50);
    p.batch.deadline_spread_ratio = 0.5;
    p.batch.min_batch_efficiency = 0.5;
    p.resource_pressure_sensitivity = 0.5;
    p.prediction_min_evidence = 4;
    p.fail_fast_on_deadline_exceeded = true;
    p.allow_completion_after_soft_violation = true;
    return p;
}

[[maybe_unused]] static RequestDescriptor make_req(std::uint64_t id, std::uint64_t tenant,
                                  std::uint64_t model, SloClass cls) {
    RequestDescriptor d;
    d.request_id = RequestId(id);
    d.tenant_id = TenantId(tenant);
    d.model_id = ModelId(model);
    d.model_revision = ModelRevision(1);
    d.slo_class = cls;
    d.prompt_tokens = 128;
    d.max_tokens = 256;
    d.remaining_tokens = 256;
    d.device_hint = DeviceClass::UNKNOWN;
    d.arrival = mono_now();
    return d;
}

[[maybe_unused]] static Observation obs_for(const AdmitResult& a, ObservationType type, Phase phase,
                           Duration elapsed) {
    Observation o;
    o.request_id = a.request_id;
    o.attempt_id = a.attempt_id;
    o.generation = a.generation;
    o.epoch = a.epoch;
    o.type = type;
    o.phase = phase;
    o.elapsed = elapsed;
    return o;
}

int main() {
    SystemClock clock;
    Governor gov(GovernorConfig{}, clock);
    std::string err;
    gov.policy_store().add(make_policy(), err);

    RequestDescriptor d = make_req(1, 1, 1, SloClass::STANDARD);
    d.contract.e2e_target = ms(100);
    const AdmitResult a = gov.admit(d);

    // The predicted transfer (50ms) is a large fraction of the 100ms budget,
    // so it crosses the policy transfer fraction limit (0.20).
    const Bytes payload = 1024;
    const Duration predicted_transfer = ms(50);
    const TransferGovernance tg = gov.govern_transfer(a.request_id, payload, predicted_transfer);
    std::printf("remaining_budget_ns        = %lld\n", (long long)ns_count(tg.remaining_budget));
    std::printf("predicted_transfer_ns      = %lld\n", (long long)ns_count(tg.predicted_transfer));
    std::printf("transfer_fraction_of_budget= %.3f\n", tg.transfer_fraction_of_budget);
    std::printf("prefer_local               = %d\n", (int)tg.prefer_local);
    std::printf("allowed                    = %d\n", (int)tg.allowed);
    std::printf("infeasible                 = %d\n", (int)tg.infeasible);
    std::printf("recommended action         = %s (%s)\n",
                to_string(tg.recommendation.action), to_string(tg.recommendation.reason));

    std::printf("\n[08_transfer_vs_local] summary: transfer would consume too much of the budget ->"
                " prefer local execution.\n");
    return 0;
}
