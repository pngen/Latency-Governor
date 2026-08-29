#pragma once

#include "latency_governor/clock.hpp"
#include "latency_governor/duration.hpp"
#include "latency_governor/enums.hpp"
#include "latency_governor/explanation.hpp"
#include "latency_governor/fairness.hpp"
#include "latency_governor/governance.hpp"
#include "latency_governor/intervention.hpp"
#include "latency_governor/observation.hpp"
#include "latency_governor/policy.hpp"
#include "latency_governor/prediction.hpp"
#include "latency_governor/request.hpp"
#include "latency_governor/risk.hpp"
#include "latency_governor/snapshot.hpp"
#include "latency_governor/worker.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace latency_governor {

// Bounded resource configuration. Everything potentially unbounded is capped.
struct GovernorConfig {
    std::size_t max_active = 16384;
    std::size_t max_completed_history = 4096;
    std::size_t max_workers = 256;
    std::size_t max_event_queue = 8192;
    std::size_t max_snapshot_requests = 512;
    std::size_t max_tenant = 1024;
    std::uint32_t max_retries = 3;
    std::size_t max_intervention_history = 64;
    double default_resource_pressure = 0.0;
};

// A terminal completion or cancellation submitted to the governor.
struct Completion {
    RequestId request_id;
    AttemptId attempt_id;
    Generation generation{0};
    CoordinatorEpoch epoch{0};
    std::optional<DispatchId> dispatch_id;
    std::optional<WorkerId> worker_id;
    std::optional<WorkerBootId> worker_boot_id;

    enum class Outcome { COMPLETED, FAILED, CANCELLED };
    Outcome outcome = Outcome::COMPLETED;
    bool slo_met = false;
    bool soft_violation = false;
    bool hard_violation = false;
    std::uint32_t tokens_generated = 0;
    TimePoint at{};
    std::string detail;
};

// Result of attempting to admit a request.
struct AdmitResult {
    bool accepted = false;
    RejectionCode code = RejectionCode::NONE;
    RequestId request_id;
    AttemptId attempt_id;
    Generation generation{0};
    CoordinatorEpoch epoch{0};
    std::string detail;
};

// The governor is the boundary where measured and predicted latency becomes an
// enforceable runtime obligation. It does not schedule, batch, prefill, decode,
// speculate, transfer, or move memory; it decides whether those actions remain
// compatible with explicit latency obligations and what intervention is
// required when they are not.
//
// Thread safety: the governor is callable from concurrent admission,
// observation, reader, and policy-replacement threads. Slow work (network,
// persistence I/O, CUDA synchronization) never runs under the internal lock.
class Governor {
public:
    Governor(GovernorConfig cfg, Clock& clock);
    Governor(const Governor&) = delete;
    Governor& operator=(const Governor&) = delete;
    ~Governor();

    // --- admission ---------------------------------------------------------
    [[nodiscard]] AdmissionAssessment evaluate_admission(const RequestDescriptor& desc) const;
    AdmitResult admit(const RequestDescriptor& desc);

    // --- observation -------------------------------------------------------
    ObservationResult observe(const Observation& obs);

    // --- risk / intervention ------------------------------------------------
    // These refresh the authoritative elapsed-time/risk cache, so they are
    // intentionally non-const.
    InterventionPlan plan(const RequestId& id, double resource_pressure = -1.0);
    [[nodiscard]] RiskAssessment assess(const RequestId& id, double resource_pressure = -1.0);
    [[nodiscard]] Explanation explain(const RequestId& id) const;
    bool commit(const Completion& comp, std::string& error);

    // --- governance hooks ---------------------------------------------------
    [[nodiscard]] QueueGovernance govern_queue(const RequestId& id, double resource_pressure = -1.0);
    [[nodiscard]] BatchGovernance govern_batch(const std::vector<RequestId>& batch, Duration current_wait);
    [[nodiscard]] PrefillGovernance govern_prefill(const RequestId& id, double resource_pressure = -1.0);
    [[nodiscard]] DecodeGovernance govern_decode(const RequestId& id, double resource_pressure = -1.0);
    [[nodiscard]] SpeculationGovernance govern_speculation(const RequestId& id, double resource_pressure = -1.0);
    [[nodiscard]] TransferGovernance govern_transfer(const RequestId& id, Bytes size, Duration predicted_transfer);
    [[nodiscard]] RetryGovernance govern_retry(const RequestId& id, Duration predicted_retry);

    // --- resource reservations ---------------------------------------------
    bool reserve(const RequestId& id, BackendId backend, Bytes amount, ReservationId& out_id, std::string& error);

    // --- workers -----------------------------------------------------------
    bool register_worker(const WorkerDescriptor& wd, std::string& error);
    bool unregister_worker(WorkerId id, WorkerBootId boot_id, std::string& error);
    // Terminalize (fail) every active request bound to the given worker
    // incarnation and release its reservations. Called by the coordinator on
    // worker loss so that in-flight work never leaks accounting.
    void fail_requests_for_worker(WorkerId id, WorkerBootId boot_id);
    [[nodiscard]] std::size_t worker_count() const noexcept;

    // --- observability -------------------------------------------------------
    [[nodiscard]] Snapshot snapshot() const;
    [[nodiscard]] MetricsSummary metrics() const;
    [[nodiscard]] std::vector<RequestSnapshot> list_requests() const;
    [[nodiscard]] std::size_t active_count() const noexcept;

    // --- epoch / policy / predictor -----------------------------------------
    void set_coordinator_epoch(CoordinatorEpoch epoch) noexcept;
    [[nodiscard]] CoordinatorEpoch coordinator_epoch() const noexcept;
    [[nodiscard]] std::uint64_t decision_generation() const noexcept;
    PolicyStore& policy_store();
    Predictor& predictor();
    FairnessTracker& fairness();

    [[nodiscard]] const GovernorConfig& config() const noexcept { return config_; }
    void bump_epoch_and_generations();

    // --- persistence ---------------------------------------------------------
    // Encode/decode the authoritative governor state (versioned, checksummed).
    [[nodiscard]] std::string encode_state(std::string& error) const;
    bool decode_state(std::string_view blob, std::string& error);

private:
    class Impl;   // pimpl: all mutable state + mutex live here.
    [[nodiscard]] const Impl& impl() const noexcept { return *impl_; }
    [[nodiscard]] Impl& impl() noexcept { return *impl_; }

    // Internal helpers. Callers must hold impl_->mutex.
    RequestState* find_locked(const RequestId& id) noexcept;
    const RequestState* find_locked(const RequestId& id) const noexcept;
    RiskAssessment evaluate_locked(RequestState& rs, double resource_pressure) noexcept;
    InterventionPlan plan_locked(RequestState& rs, double resource_pressure, bool record) noexcept;

    GovernorConfig config_;
    Clock& clock_;
    std::unique_ptr<Impl> impl_;
};

} // namespace latency_governor