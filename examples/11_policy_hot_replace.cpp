// 11_policy_hot_replace.cpp
// Add two policy generations, admit under the first, then hot-replace. The
// store keeps both generations, current() is the newest, and each request's
// admitted generation is retained (never reinterpreted by the live policy).
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

    // Generation 1: baseline.
    Policy p1 = make_policy();
    p1.name = "baseline";
    p1.classes[0].default_e2e_target = ms(100);
    const auto g1 = gov.policy_store().add(p1, err);
    if (!g1) { std::printf("add gen1 failed: %s\n", err.c_str()); return 1; }

    // Admit a request while generation 1 is authoritative.
    RequestDescriptor d1 = make_req(1, 1, 1, SloClass::REALTIME);
    d1.contract.e2e_target = ms(200);
    const AdmitResult a1 = gov.admit(d1);
    std::printf("gen1 added (id=%llu), current=%llu, request 1 admitted\n",
                (unsigned long long)*g1, (unsigned long long)*gov.policy_store().current_generation());

    // Generation 2: hot-replaced, tuned. Same store, newer generation.
    Policy p2 = make_policy();
    p2.name = "tuned";
    p2.classes[0].default_e2e_target = ms(300);
    const auto g2 = gov.policy_store().add(p2, err);
    if (!g2) { std::printf("add gen2 failed: %s\n", err.c_str()); return 1; }

    // Admit a second request under the new current generation.
    RequestDescriptor d2 = make_req(2, 2, 1, SloClass::REALTIME);
    d2.contract.e2e_target = ms(400);
    const AdmitResult a2 = gov.admit(d2);

    const Policy* cur = gov.policy_store().current();
    const Policy* gen1p = gov.policy_store().get(1);
    const Policy* gen2p = gov.policy_store().get(2);
    std::printf("after hot-replace: current_gen=%llu retained=%zu\n",
                (unsigned long long)*gov.policy_store().current_generation(),
                (unsigned long long)gov.policy_store().count());
    if (cur) std::printf("  current policy name='%s' generation=%llu\n", cur->name.c_str(), (unsigned long long)cur->generation);
    if (gen1p) std::printf("  gen1 retained name='%s' REALTIME e2e_ms=%lld (historical, not reinterpreted)\n",
                           gen1p->name.c_str(), (long long)ns_count(gen1p->class_spec("REALTIME")->default_e2e_target));
    if (gen2p) std::printf("  gen2 retained name='%s' REALTIME e2e_ms=%lld\n",
                           gen2p->name.c_str(), (long long)ns_count(gen2p->class_spec("REALTIME")->default_e2e_target));

    std::printf("  request1 admitted_generation=%llu  request2 admitted_generation=%llu\n",
                (unsigned long long)a1.generation.value(), (unsigned long long)a2.generation.value());

    std::printf("\n[11_policy_hot_replace] summary: both generations coexist; current is gen2;"
                " the request admitted under gen1 is not reinterpreted.\n");
    return 0;
}
