// 12_distributed.cpp
// A runnable, network-free description of the coordinator/worker architecture,
// plus a live Governor snapshot to show the runtime state it exposes.
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
    std::printf("Latency-Governor coordinator / worker architecture:\n");
    std::printf("  * The coordinator owns a single Governor instance (thread-safe).\n");
    std::printf("  * Workers register with the coordinator (HELLO / REGISTER / REGISTER_ACK)\n");
    std::printf("    over a framed protocol; each worker creates a Backend (cpu or cuda).\n");
    std::printf("  * The coordinator admits request descriptors and emits ADMIT interventions.\n");
    std::printf("  * Workers execute PREFILL/DECODE and send OBSERVATION frames back.\n");
    std::printf("  * The coordinator ingests observations into the Predictor and plans an\n");
    std::printf("    intervention (InterventionPlan) for each request.\n");
    std::printf("  * No network is used in this example; it demonstrates the Governor snapshot.\n\n");

    SystemClock clock;
    Governor gov(GovernorConfig{}, clock);
    std::string err;
    gov.policy_store().add(make_policy(), err);

    RequestDescriptor d1 = make_req(1, 1, 1, SloClass::REALTIME);
    d1.contract.e2e_target = ms(200);
    RequestDescriptor d2 = make_req(2, 2, 1, SloClass::THROUGHPUT);
    d2.contract.e2e_target = ms(600);
    gov.admit(d1);
    gov.admit(d2);

    const Snapshot snap = gov.snapshot();
    std::printf("Governor snapshot:\n");
    std::printf("  epoch=%llu  decision_generation=%llu  event_sequence=%llu\n",
                (unsigned long long)snap.coordinator_epoch,
                (unsigned long long)snap.decision_generation,
                (unsigned long long)snap.event_sequence);
    std::printf("  active=%llu completed=%llu observations=%llu\n",
                (unsigned long long)snap.summary.active,
                (unsigned long long)snap.summary.completed,
                (unsigned long long)snap.summary.observations_received);
    std::printf("  request_snapshots=%zu\n", snap.requests.size());

    std::printf("\n[12_distributed] summary: coordinator/worker architecture + governor snapshot.\n");
    return 0;
}
