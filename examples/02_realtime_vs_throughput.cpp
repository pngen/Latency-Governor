// 02_realtime_vs_throughput.cpp
// Admit a REALTIME and a THROUGHPUT request and show the different risk and
// protection the governor applies to each class.
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
    ManualClock clock;
    Governor gov(GovernorConfig{}, clock);
    std::string err;
    gov.policy_store().add(make_policy(), err);

    // Tight obligation for the realtime class, loose for throughput.
    RequestDescriptor d1 = make_req(1, 1, 1, SloClass::REALTIME);
    d1.contract.e2e_target = ms(8);
    d1.contract.ttf_target = ms(2);
    d1.contract.decode_step_target = ms(1);

    RequestDescriptor d2 = make_req(2, 2, 1, SloClass::THROUGHPUT);
    d2.contract.e2e_target = ms(4000);
    d2.contract.ttf_target = ms(800);
    d2.contract.decode_step_target = ms(200);

    const AdmitResult a1 = gov.admit(d1);
    const AdmitResult a2 = gov.admit(d2);
    std::printf("admitted REALTIME=%s accepted=%d  THROUGHPUT=%s accepted=%d\n",
                a1.request_id.to_string().c_str(), (int)a1.accepted,
                a2.request_id.to_string().c_str(), (int)a2.accepted);

    // Drive the same decode workload under a manually advanced monotonic clock.
    for (int i = 0; i < 10; ++i) {
        clock.advance(ms(1));
        gov.observe(obs_for(a1, ObservationType::DECODE_STEP, Phase::DECODE, ms(1)));
        gov.observe(obs_for(a2, ObservationType::DECODE_STEP, Phase::DECODE, ms(1)));
    }

    const RiskAssessment r1 = gov.assess(a1.request_id);
    const RiskAssessment r2 = gov.assess(a2.request_id);
    std::printf("REALTIME   risk=%s budget_used=%.3f deadline_risk=%.3f\n",
                to_string(r1.state), r1.budget_used_fraction, r1.deadline_risk);
    std::printf("THROUGHPUT risk=%s budget_used=%.3f deadline_risk=%.3f\n",
                to_string(r2.state), r2.budget_used_fraction, r2.deadline_risk);

    // The protection each class is given differs.
    const InterventionPlan p1 = gov.plan(a1.request_id);
    const InterventionPlan p2 = gov.plan(a2.request_id);
    std::printf("REALTIME   intervention=%s reason=%s\n",
                to_string(p1.items.front().action), to_string(p1.items.front().reason));
    std::printf("THROUGHPUT intervention=%s reason=%s\n",
                to_string(p2.items.front().action), to_string(p2.items.front().reason));

    std::printf("\n[02_realtime_vs_throughput] summary: REALTIME is at CRITICAL risk (EXPEDITE),"
                " THROUGHPUT stays SAFE.\n");
    return 0;
}
