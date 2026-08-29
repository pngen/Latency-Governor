// bench_core.cpp
// Measures the throughput of the core governor operations. Every operation is
// completed inline before the timing window ends (synchronous, not enqueue-only).
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

[[maybe_unused]] static Observation obs_for(const AdmitResult& a, ObservationType type,
                                            Phase phase, Duration elapsed) {
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

// Run \`ops\` operations and report ops/s. All ops complete synchronously.
template <typename Fn>
static void bench(const char* name, std::uint64_t ops, Fn fn) {
    const TimePoint t0 = mono_now();
    for (std::uint64_t i = 0; i < ops; ++i) fn(i);
    const TimePoint t1 = mono_now();
    const double secs = static_cast<double>(ns_count(saturating_elapsed(t0, t1))) / 1e9;
    const double ops_per_s = (secs > 0.0) ? static_cast<double>(ops) / secs : 0.0;
    std::printf("%-24s ops=%-10llu elapsed_ms=%-9.1f ops/s=%-12.0f completed=yes\n",
                name, (unsigned long long)ops, secs * 1e3, ops_per_s);
}

int main() {
    SystemClock clock;
    Governor gov(GovernorConfig{}, clock);
    std::string err;
    gov.policy_store().add(make_policy(), err);

    // A pool of admitted requests used by the observation / risk / plan benches.
    const std::uint64_t kPool = 1024;
    std::vector<AdmitResult> pool;
    pool.reserve(static_cast<std::size_t>(kPool));
    for (std::uint64_t i = 1; i <= kPool; ++i) {
        RequestDescriptor d = make_req(i, 1, 1, SloClass::STANDARD);
        d.contract.e2e_target = ms(500);
        const AdmitResult a = gov.admit(d);
        if (a.accepted) pool.push_back(a);
    }
    const std::size_t n = pool.size();

    std::printf("bench_core  (pool=%zu active=%zu)  \n", n, (std::size_t)gov.active_count());

    // Admission evaluation (read-only feasibility).
    const RequestDescriptor desc = make_req(9999, 1, 1, SloClass::STANDARD);
    bench("admission_eval", 500000, [&](std::uint64_t) {
        volatile AdmissionAssessment r = gov.evaluate_admission(desc);
        (void)r;
    });

    // Observation ingest (real state mutation + predictor record).
    bench("observation_ingest", 500000, [&](std::uint64_t i) {
        const AdmitResult& a = pool[static_cast<std::size_t>(i % n)];
        gov.observe(obs_for(a, ObservationType::DECODE_STEP, Phase::DECODE, ms(1)));
    });

    // Risk evaluation.
    const RequestId bench_id = pool[0].request_id;
    bench("risk_eval", 500000, [&](std::uint64_t) {
        volatile RiskAssessment r = gov.assess(bench_id);
        (void)r;
    });

    // Intervention planning (records bounded intervention history).
    bench("intervention_planning", 200000, [&](std::uint64_t) {
        volatile InterventionPlan r = gov.plan(bench_id);
        (void)r;
    });

    // Predictor update + query.
    bench("predictor_update", 1000000, [&](std::uint64_t) {
        gov.predictor().record("bench_decode", ms(1));
    });
    bench("predictor_query", 1000000, [&](std::uint64_t) {
        volatile const std::optional<Prediction> p = gov.predictor().predict("bench_decode");
        (void)p;
    });

    // Snapshot generation.
    bench("snapshot", 3000, [&](std::uint64_t) {
        volatile Snapshot s = gov.snapshot();
        (void)s;
    });

    // Explanation generation.
    bench("explain", 200000, [&](std::uint64_t) {
        volatile Explanation x = gov.explain(bench_id);
        (void)x;
    });

    std::printf("\n[bench_core] summary: all core operations ran to completion and were timed.\n");
    return 0;
}
