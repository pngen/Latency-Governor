#pragma once

#include "latency_governor/strong_id.hpp"

namespace latency_governor {

// Tag types. Each tag is a distinct empty type so that ids are strongly
// separated at compile time.
struct RequestIdTag {};
struct AttemptIdTag {};
struct DispatchIdTag {};
struct WorkerIdTag {};
struct WorkerBootIdTag {};
struct TenantIdTag {};
struct ModelIdTag {};
struct ModelRevisionTag {};
struct AdapterIdTag {};
struct GenerationTag {};
struct CoordinatorEpochTag {};
struct PolicyIdTag {};
struct ObservationIdTag {};
struct ReservationIdTag {};
struct BackendTag {};

// Strong aliases used throughout the runtime.
using RequestId = StrongId<RequestIdTag>;
using AttemptId = StrongId<AttemptIdTag>;
using DispatchId = StrongId<DispatchIdTag>;
using WorkerId = StrongId<WorkerIdTag>;
using WorkerBootId = StrongId<WorkerBootIdTag>;
using TenantId = StrongId<TenantIdTag>;
using ModelId = StrongId<ModelIdTag>;
using ModelRevision = StrongId<ModelRevisionTag>;
using AdapterId = StrongId<AdapterIdTag>;

// Generations and epochs are monotonic counters, not ids, but are given the
// same strong treatment so a stale value cannot be silently reused.
using Generation = StrongId<GenerationTag>;
using CoordinatorEpoch = StrongId<CoordinatorEpochTag>;
using PolicyId = StrongId<PolicyIdTag>;
using ObservationId = StrongId<ObservationIdTag>;
using ReservationId = StrongId<ReservationIdTag>;
using BackendId = StrongId<BackendTag>;

// A monotonic generation for a given entity. The value zero means "not yet
// assigned". Generations strictly increase across the lifetime of the
// coordinator process. A new coordinator epoch aligns with a fresh generation
// space.
struct GenerationPolicy {
    // A request is assigned a generation at admission. Every state mutation,
    // observation, and completion must carry the generation that the actor
    // observed. If a completion carries an older generation it is rejected.
    static constexpr Generation at_start() noexcept { return Generation(1); }
};

} // namespace latency_governor
