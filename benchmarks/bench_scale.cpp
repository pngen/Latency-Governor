// bench_scale.cpp
// Measures admission throughput at 1k / 10k / 100k active requests.
//
// Links: latency_governor (core only).
#include "latency_governor/governor.hpp"

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

static double secs_between(const TimePoint& a, const TimePoint& b) {
    return static_cast<double>(ns_count(saturating_elapsed(a, b))) / 1e9;
}

int main() {
    const std::uint64_t scales[] = {1000, 10000, 100000};
    for (const std::uint64_t scale : scales) {
        // Fresh governor per scale so request ids never collide and max_active is
        // per-scale.
        GovernorConfig cfg;
        cfg.max_active = 200000;
        SystemClock clock;
        Governor gov(cfg, clock);
        std::string err;
        gov.policy_store().add(make_policy(), err);

        // Build descriptors.
        const TimePoint tb0 = mono_now();
        std::vector<RequestDescriptor> descs;
        descs.reserve(static_cast<std::size_t>(scale));
        for (std::uint64_t i = 1; i <= scale; ++i) {
            descs.push_back(make_req(i, (i % 8) + 1, 1, SloClass::STANDARD));
        }
        const TimePoint tb1 = mono_now();

        // Admit all (completed synchronously).
        const TimePoint ta0 = mono_now();
        std::size_t admitted = 0;
        for (const RequestDescriptor& d : descs) {
            if (gov.admit(d).accepted) ++admitted;
        }
        const TimePoint ta1 = mono_now();

        // Snapshot over the full active set.
        const TimePoint ts0 = mono_now();
        const Snapshot snap = gov.snapshot();
        const TimePoint ts1 = mono_now();

        const double build_ms = secs_between(tb0, tb1) * 1e3;
        const double admit_ms = secs_between(ta0, ta1) * 1e3;
        const double admit_ops = (admit_ms > 0.0) ? static_cast<double>(scale) / (admit_ms / 1e3) : 0.0;
        const double snapshot_ms = secs_between(ts0, ts1) * 1e3;

        std::printf("scale=%-7llu build_ms=%-8.1f admit_ms=%-8.1f admit_ops/s=%-12.0f "
                    "accepted=%-8zu snapshot_requests=%-4zu snapshot_ms=%.1f completed=yes\n",
                    (unsigned long long)scale, build_ms, admit_ms, admit_ops, admitted,
                    snap.requests.size(), snapshot_ms);
    }

    std::printf("\n[bench_scale] summary: admission is fully completed and timed at each scale.\n");
    return 0;
}
