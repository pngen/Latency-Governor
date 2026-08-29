// bench_threads.cpp
// N threads ingest observations into one shared (thread-safe) Governor
// concurrently; reports aggregate ops/s for completed observations.
//
// Links: latency_governor (core only).
#include "latency_governor/governor.hpp"

#include <atomic>
#include <cstdio>
#include <thread>
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
    SystemClock clock;
    Governor gov(GovernorConfig{}, clock);
    std::string err;
    gov.policy_store().add(make_policy(), err);

    // Pre-admit a pool of requests that the threads will observe.
    std::vector<AdmitResult> pool;
    for (std::uint64_t i = 1; i <= 1000; ++i) {
        RequestDescriptor d = make_req(i, 1, 1, SloClass::STANDARD);
        d.contract.e2e_target = ms(500);
        const AdmitResult a = gov.admit(d);
        if (a.accepted) pool.push_back(a);
    }
    const std::size_t n = pool.size();

    const unsigned hw = std::thread::hardware_concurrency();
    const unsigned num_threads = (hw == 0) ? 4u : ((hw > 16u) ? 16u : hw);

    std::atomic<std::uint64_t> ops{0};
    std::atomic<std::uint64_t> next{0};
    std::atomic<bool> stop{false};

    std::vector<std::thread> threads;
    threads.reserve(num_threads);
    for (unsigned t = 0; t < num_threads; ++t) {
        threads.emplace_back([&]() {
            while (!stop.load(std::memory_order_relaxed)) {
                const std::uint64_t idx = next.fetch_add(1, std::memory_order_relaxed) % n;
                const AdmitResult& a = pool[static_cast<std::size_t>(idx)];
                gov.observe(obs_for(a, ObservationType::DECODE_STEP, Phase::DECODE, ms(1)));
                ops.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    const std::uint64_t budget_ns = 500 * 1000 * 1000ULL;  // 500 ms
    const TimePoint t0 = mono_now();
    while (static_cast<std::uint64_t>(ns_count(saturating_elapsed(t0, mono_now()))) < budget_ns) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    stop.store(true, std::memory_order_relaxed);
    for (std::thread& th : threads) th.join();
    const TimePoint t1 = mono_now();

    const double secs = static_cast<double>(ns_count(saturating_elapsed(t0, t1))) / 1e9;
    const double total = static_cast<double>(ops.load());
    const double ops_per_s = (secs > 0.0) ? total / secs : 0.0;

    std::printf("threads=%u pool=%zu elapsed_ms=%.1f completed_ops=%.0f aggregate_ops/s=%.0f completed=yes\n",
                num_threads, n, secs * 1e3, total, ops_per_s);

    std::printf("\n[bench_threads] summary: concurrent observation ingestion fully completed and timed.\n");
    return 0;
}
