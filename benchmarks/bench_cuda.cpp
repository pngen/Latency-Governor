// bench_cuda.cpp
// Measures real CUDA governed-execution throughput (completed kernels), feeding
// each measured kernel duration back into the Governor via observations. If the
// CUDA backend is unavailable, prints a message and exits 0.
//
// Links: latency_governor, latency_governor_backends, latency_governor_backends_cuda.
#include "latency_governor/governor.hpp"
#include "latency_governor/backend.hpp"

#include <cstdio>
#include <vector>

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

int main() {
    CudaBackend backend;
    if (!backend.available()) {
        std::printf("[bench_cuda] CUDA backend unavailable (%s) -> exit 0\n", backend.device_info().c_str());
        return 0;
    }
    std::printf("[bench_cuda] CUDA backend: %s\n", backend.device_info().c_str());

    SystemClock clock;
    Governor gov(GovernorConfig{}, clock);
    std::string err;
    gov.policy_store().add(make_policy(), err);
    RequestDescriptor d = make_req(1, 1, 1, SloClass::STANDARD);
    d.contract.e2e_target = ms(2000);
    d.contract.ttf_target = ms(300);
    d.contract.decode_step_target = ms(50);
    const AdmitResult a = gov.admit(d);

    // Warm-up a couple of kernels so the CUDA context is ready before timing.
    std::vector<std::uint8_t> in(256, 0x5A), out;
    backend.execute(Workload::PREFILL, in.size(), in, out);
    backend.execute(Workload::DECODE, in.size(), in, out);

    const std::uint64_t batches = 200;   // 400 completed kernels total.
    const TimePoint t0 = mono_now();
    std::uint64_t kernels = 0, work_items = 0, failed = 0;
    for (std::uint64_t i = 0; i < batches; ++i) {
        const ExecResult pre = backend.execute(Workload::PREFILL, in.size(), in, out);
        if (pre.success) {
            ++kernels; work_items += pre.work_items;
            gov.observe(obs_for(a, ObservationType::PREFILL_CHUNK_COMPLETED, Phase::PREFILL, pre.duration));
        } else { ++failed; }
        const ExecResult dec = backend.execute(Workload::DECODE, in.size(), in, out);
        if (dec.success) {
            ++kernels; work_items += dec.work_items;
            gov.observe(obs_for(a, ObservationType::DECODE_STEP, Phase::DECODE, dec.duration));
        } else { ++failed; }
    }
    const TimePoint t1 = mono_now();

    const RiskAssessment ra = gov.assess(a.request_id);
    const InterventionPlan plan = gov.plan(a.request_id);
    const double secs = static_cast<double>(ns_count(saturating_elapsed(t0, t1))) / 1e9;
    const double kernels_s = (secs > 0.0) ? static_cast<double>(kernels) / secs : 0.0;
    const double work_s = (secs > 0.0) ? static_cast<double>(work_items) / secs : 0.0;

    std::printf("batches=%llu kernels=%llu work_items=%llu failed=%llu elapsed_ms=%.1f\n",
                (unsigned long long)batches, (unsigned long long)kernels,
                (unsigned long long)work_items, (unsigned long long)failed, secs * 1e3);
    std::printf("kernels/s=%.0f work_items/s=%.0f completed=%s\n",
                kernels_s, work_s, (failed == 0) ? "yes" : "no");
    std::printf("governed plan: risk=%s intervention=%s reason=%s\n",
                to_string(ra.state), to_string(plan.items.front().action),
                to_string(plan.items.front().reason));

    std::printf("\n[bench_cuda] summary: real CUDA kernels executed, measured, and fed to the governor.\n");
    return failed == 0 ? 0 : 1;
}
