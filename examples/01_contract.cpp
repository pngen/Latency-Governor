// 01_contract.cpp
// Build and validate a LatencyContract, then print the validate result.
//
// Links: latency_governor (core only).
#include "latency_governor/governor.hpp"

#include <cstdio>

using namespace latency_governor;

int main() {
    // A well-formed contract: the primary scalar SLO, an explicit hard
    // deadline, per-phase targets, and the tolerances that decide when an
    // intervention is warranted.
    LatencyContract c;
    c.e2e_target = ms(500);
    c.hard_deadline = ms(600);
    c.ttf_target = ms(50);
    c.max_queue_residence = ms(20);
    c.max_dispatch_delay = ms(10);
    c.prefill_target = ms(150);
    c.decode_step_target = ms(10);
    c.transfer_allowance = ms(40);
    c.retry_allowance = ms(60);
    c.max_retry_delay = ms(30);
    c.max_spec_overhead = ms(25);
    c.deadline_risk_threshold = 0.10;
    c.min_completion_probability = 0.0;
    c.intervention_aggressiveness = 0.5;
    c.fairness_weight = 1.0;
    c.admission_policy = AdmissionPolicy::STRICT;
    c.cancellation_policy = CancellationPolicy::FAIL_FAST;
    c.degradation_allowed = true;

    std::string err;
    const bool ok = c.validate(err);
    std::printf("LatencyContract.validate() = %s\n", ok ? "true" : "false");
    if (!ok) {
        std::printf("  validate error: %s\n", err.c_str());
        return 1;
    }
    std::printf("  e2e_target_ns           = %lld\n", (long long)ns_count(c.e2e_target));
    std::printf("  hard_deadline_ns        = %lld\n", (long long)ns_count(*c.hard_deadline));
    std::printf("  ttf_target_ns           = %lld\n", (long long)ns_count(c.ttf_target));
    std::printf("  max_queue_residence_ns  = %lld\n", (long long)ns_count(c.max_queue_residence));
    std::printf("  prefill_target_ns       = %lld\n", (long long)ns_count(c.prefill_target));
    std::printf("  decode_step_target_ns   = %lld\n", (long long)ns_count(c.decode_step_target));

    // A contract whose hard deadline is tighter than its own soft target is a
    // contradiction and must be rejected (a hard deadline may be absent, but
    // it may never be stricter than the target SLO).
    LatencyContract bad;
    bad.e2e_target = ms(500);
    bad.hard_deadline = ms(100);
    const bool ok2 = bad.validate(err);
    std::printf("\ninvalid contract validate() = %s\n", ok2 ? "true" : "false");
    std::printf("  validate error: %s\n", err.c_str());

    std::printf("\n[01_contract] summary: valid contract accepted, contradictory deadline rejected.\n");
    return 0;
}
