#pragma once

#include "latency_governor/clock.hpp"
#include "latency_governor/duration.hpp"
#include "latency_governor/enums.hpp"
#include "latency_governor/identifiers.hpp"
#include "latency_governor/request.hpp"

#include <optional>
#include <string>

namespace latency_governor {

// A single latency observation supplied to the governor. Observations carry the
// authority envelope observed by the reporter so that stale work can be
// rejected before it mutates request state.
struct Observation {
    ObservationId id;
    ObservationType type = ObservationType::QUEUE_ENTERED;
    RequestId request_id;
    AttemptId attempt_id;
    Generation generation{0};
    CoordinatorEpoch epoch{0};
    std::optional<DispatchId> dispatch_id;
    std::optional<WorkerId> worker_id;
    std::optional<WorkerBootId> worker_boot_id;

    TimePoint at{};
    TimePoint phase_start{};
    std::optional<Duration> elapsed;

    Phase phase = Phase::UNCLASSIFIED;
    std::optional<double> value;
    std::optional<Bytes> bytes;
    std::uint32_t tokens = 0;

    // Predictor hook: when set, the measured elapsed is recorded against this
    // predictor metric key.
    std::string predictor_key;

    std::string detail;
};

// Result of applying an observation to request state.
struct ObservationResult {
    bool accepted = false;
    RejectionCode code = RejectionCode::NONE;
    std::string detail;
};

} // namespace latency_governor
