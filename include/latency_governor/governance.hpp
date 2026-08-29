#pragma once

#include "latency_governor/clock.hpp"
#include "latency_governor/duration.hpp"
#include "latency_governor/enums.hpp"
#include "latency_governor/intervention.hpp"
#include "latency_governor/request.hpp"

#include <optional>
#include <vector>

namespace latency_governor {

// Typed governance decisions. Each hook answers a precise latency question for
// a sibling runtime (scheduler, batch fabric, prefill/decode runtimes,
// speculation, transfer, retry) without owning that runtime.

struct QueueGovernance {
    Intervention recommendation;
    bool escalate_priority = false;
    bool force_dispatch = false;
    bool defer = false;
    bool fail_fast = false;
    bool bypass_batch = false;
    Duration residence{0};
    Duration predicted_dispatch_delay{0};
    Duration deadline_slack{0};
    Duration starvation_age{0};
    double urgency = 0.0;
};

struct BatchGovernance {
    bool seal_now = false;
    Duration max_additional_wait{0};
    bool bypass = false;
    bool shrink = false;
    Duration oldest_wait{0};
    Duration deadline_spread{0};
    Duration predicted_batch_cost{0};
    double batch_efficiency = 0.0;
};

struct PrefillGovernance {
    Intervention recommendation;
    bool yield_after_chunk = false;
    bool continue_chunk = false;
    bool expedite = false;
    bool protect_ttft = false;
    bool fail_fast = false;
    Duration predicted_chunk_cost{0};
    Duration predicted_remaining_prefill{0};
};

struct DecodeGovernance {
    Intervention recommendation;
    bool protect_sequence = false;
    bool expedite_step = false;
    bool favor_smaller_batch = false;
    bool cancel_if_impossible = false;
    bool mark_soft_violation = false;
    Duration predicted_step{0};
    Duration deadline_slack{0};
};

struct SpeculationGovernance {
    Intervention recommendation;
    bool preserve = false;
    bool reduce_depth = false;
    bool disable = false;
    bool allow_more_branches = false;
    double acceptance_rate = 0.0;
    Duration predicted_overhead{0};
    Duration remaining_budget{0};
};

struct TransferGovernance {
    Intervention recommendation;
    bool allowed = false;
    bool prefer_local = false;
    bool expedite = false;
    bool infeasible = false;
    Duration predicted_transfer{0};
    Duration remaining_budget{0};
    double transfer_fraction_of_budget = 0.0;
};

struct RetryGovernance {
    Intervention recommendation;
    bool allowed = false;
    bool limited_backoff = false;
    bool immediate = false;
    bool prohibited = false;
    bool fail_fast = false;
    Duration predicted_retry_duration{0};
    Duration remaining_budget{0};
    Duration max_cumulative_retry_delay{0};
    std::uint32_t retry_count = 0;
};

} // namespace latency_governor