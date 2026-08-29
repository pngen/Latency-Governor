// 14_cuda_governed.cpp
// If a CUDA backend is available, run real prefill/decode kernels, feed the
// measured durations into the Governor via observations/predictor, then plan()
// and print the risk/intervention. If CUDA is unavailable, print a message and
// exit 0.
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
    CudaBackend backend;
    if (!backend.available()) {
        std::printf("[14_cuda] CUDA backend unavailable (%s) -> exit 0\n",
                    backend.device_info().c_str());
        return 0;
    }
    std::printf("[14_cuda] CUDA backend available: %s\n", backend.device_info().c_str());

    SystemClock clock;
    Governor gov(GovernorConfig{}, clock);
    std::string err;
    gov.policy_store().add(make_policy(), err);

    RequestDescriptor d = make_req(1, 1, 1, SloClass::STANDARD);
    d.contract.e2e_target = ms(1000);
    d.contract.ttf_target = ms(200);
    d.contract.prefill_target = ms(500);
    d.contract.decode_step_target = ms(20);
    const AdmitResult a = gov.admit(d);

    // Real governed prefill execution.
    std::vector<std::uint8_t> in_prefill(128, 0x5A), out_prefill;
    const ExecResult pre = backend.execute(Workload::PREFILL, in_prefill.size(), in_prefill, out_prefill);
    Observation o1 = obs_for(a, ObservationType::PREFILL_CHUNK_COMPLETED, Phase::PREFILL, pre.duration);
    o1.predictor_key = "PREFILL";
    gov.observe(o1);
    std::printf("prefill  success=%d duration_ns=%lld ticks=%llu\n", (int)pre.success,
                (long long)ns_count(pre.duration), (unsigned long long)pre.ticks);

    // Real governed decode execution.
    std::vector<std::uint8_t> in_decode(96, 0xA5), out_decode;
    const ExecResult dec = backend.execute(Workload::DECODE, in_decode.size(), in_decode, out_decode);
    Observation o2 = obs_for(a, ObservationType::DECODE_STEP, Phase::DECODE, dec.duration);
    o2.predictor_key = "DECODE";
    gov.observe(o2);
    std::printf("decode   success=%d duration_ns=%lld ticks=%llu\n", (int)dec.success,
                (long long)ns_count(dec.duration), (unsigned long long)dec.ticks);

    const RiskAssessment ra = gov.assess(a.request_id);
    const InterventionPlan plan = gov.plan(a.request_id);
    std::printf("risk=%s deadline_risk=%.3f predicted=%lld\n", to_string(ra.state),
                ra.deadline_risk,
                (long long)(ra.prediction ? ns_count(ra.prediction->predicted) : 0));
    std::printf("intervention=%s reason=%s\n",
                to_string(plan.items.front().action), to_string(plan.items.front().reason));

    std::printf("\n[14_cuda] summary: CUDA prefill/decode executed and fed into the governor"
                " -> plan() produced an intervention.\n");
    return 0;
}
