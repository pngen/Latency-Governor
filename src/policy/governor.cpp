#include "latency_governor/governor.hpp"

#include <algorithm>
#include <bit>
#include <deque>
#include <map>
#include <mutex>
#include <numeric>
#include <sstream>

#include "latency_governor/serialization.hpp"

namespace latency_governor {

using Outcome = Completion::Outcome;

namespace {

[[nodiscard]] double clamp01(double v) noexcept { return std::max(0.0, std::min(1.0, v)); }

const char* phase_key(Phase p) { return to_string(p); }

[[nodiscard]] SloClass effective_class(const RequestDescriptor& d) {
    return d.slo_class;
}

// Build a deterministic budget partition from the class spec and contract.
BudgetPartition build_partition(const Policy& policy, const RequestDescriptor& d) {
    BudgetPartition bp;
    const SloClassSpec* spec = nullptr;
    // Tenant/model overrides first.
    auto tm = policy.model_class.find(d.model_id);
    if (tm != policy.model_class.end()) {
        spec = policy.class_spec(tm->second);
    }
    if (spec == nullptr) spec = policy.class_spec(d.slo_class);
    if (spec == nullptr) {
        // Last resort: first class.
        if (!policy.classes.empty()) spec = &policy.classes.front();
    }
    const Duration e2e = d.contract.e2e_target;
    double scale = 1.0;
    auto ms = policy.model_budget_scale.find(d.model_id);
    if (ms != policy.model_budget_scale.end()) scale = ms->second;
    if (spec != nullptr) {
        for (std::size_t i = 0; i < kPhaseCount; ++i) {
            double w = (spec->phase_weights[i] > 0.0) ? spec->phase_weights[i] : (i == enum_index(Phase::UNCLASSIFIED) ? 0.0 : 0.05);
            bp.allocation[i] = Duration(static_cast<std::int64_t>(static_cast<double>(ns_count(e2e)) * w * scale));
        }
        bp.reserve = Duration(static_cast<std::int64_t>(static_cast<double>(ns_count(e2e)) * spec->reserve_fraction * scale));
    }
    return bp;
}

LatencyContract finalize_contract(const Policy& policy, const RequestDescriptor& d) {
    LatencyContract c = d.contract;
    const SloClassSpec* spec = policy.class_spec(d.slo_class);
    if (spec != nullptr) {
        if (c.e2e_target <= Duration::zero()) c.e2e_target = spec->default_e2e_target;
        if (c.ttf_target <= Duration::zero()) c.ttf_target = spec->default_ttf_target;
        if (c.deadline_risk_threshold == 0.10 && spec->default_deadline_risk_threshold != 0.10)
            c.deadline_risk_threshold = spec->default_deadline_risk_threshold;
    }
    if (!c.hard_deadline) c.hard_deadline = c.e2e_target;
    c.policy_id = policy.generation;
    return c;
}

} // namespace

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------
class Governor::Impl {
public:
    Impl(const GovernorConfig& cfg, Clock& clk)
        : config(cfg), clock(clk), fairness(cfg.max_tenant) {}

    Clock& clock;
    const GovernorConfig& config;
    mutable std::mutex mutex;
    std::map<RequestId, RequestState> requests;
    std::deque<RequestId> completion_order;   // for bounding terminal history
    std::map<WorkerId, WorkerDescriptor> workers;
    PolicyStore policy_store;
    Predictor predictor;
    FairnessTracker fairness;
    MetricsSummary metrics;
    std::deque<EventRecord> events;
    std::uint64_t event_sequence = 0;
    std::uint64_t decision_generation = 0;
    CoordinatorEpoch epoch{1};
    std::uint64_t next_attempt = 1;
    std::uint64_t next_generation = 1;
    std::uint64_t next_observation = 1;
    std::uint64_t next_reservation = 1;

    void push_event(const std::string& kind, const std::string& detail) {
        events.push_back({++event_sequence, clock.now(), kind, detail});
        if (events.size() > config.max_event_queue) events.pop_front();
    }
};

namespace {
[[nodiscard]] RejectionCode check_env(const RequestState& rs,
                                      const CoordinatorEpoch epoch,
                                      const Generation gen,
                                      const AttemptId attempt,
                                      const std::optional<DispatchId>& dispatch,
                                      const std::optional<WorkerId>& worker,
                                      const std::optional<WorkerBootId>& boot,
                                      const CoordinatorEpoch cur_epoch) noexcept {
    if (epoch != cur_epoch) return RejectionCode::EPOCH_MISMATCH;
    if (gen != rs.generation) return RejectionCode::GENERATION_MISMATCH;
    if (attempt != rs.attempt_id) return RejectionCode::ATTEMPT_MISMATCH;
    if (dispatch && rs.dispatch_id && *dispatch != *rs.dispatch_id) return RejectionCode::DISPATCH_MISMATCH;
    if (worker && rs.worker_id && *worker != *rs.worker_id) return RejectionCode::WORKER_MISMATCH;
    if (boot && rs.worker_boot_id && *boot != *rs.worker_boot_id) return RejectionCode::BOOT_MISMATCH;
    return RejectionCode::NONE;
}
} // namespace

Governor::Governor(GovernorConfig cfg, Clock& clock)
    : config_(cfg), clock_(clock), impl_(std::make_unique<Impl>(config_, clock)) {}

Governor::~Governor() = default;

void Governor::set_coordinator_epoch(CoordinatorEpoch epoch) noexcept {
    impl_->epoch = epoch;
}
CoordinatorEpoch Governor::coordinator_epoch() const noexcept { return impl_->epoch; }
std::uint64_t Governor::decision_generation() const noexcept { return impl_->decision_generation; }
PolicyStore& Governor::policy_store() { return impl_->policy_store; }
Predictor& Governor::predictor() { return impl_->predictor; }
FairnessTracker& Governor::fairness() { return impl_->fairness; }
std::size_t Governor::active_count() const noexcept { std::lock_guard g(impl_->mutex); return impl_->requests.size(); }

void Governor::bump_epoch_and_generations() {
    std::lock_guard g(impl_->mutex);
    impl_->epoch = CoordinatorEpoch(impl_->epoch.value() + 1);
    ++impl_->decision_generation;
    for (auto& [id, rs] : impl_->requests) {
        if (rs.is_active()) {
            rs.epoch = impl_->epoch;
            rs.generation = Generation(rs.generation.value() + 1);
            rs.dispatch_id.reset();
            rs.worker_id.reset();
            rs.worker_boot_id.reset();
            rs.current_phase_elapsed = Duration::zero();
            rs.phase_started_at = clock_.now();
        }
    }
    impl_->push_event("epoch_bump", "epoch=" + impl_->epoch.to_string());
}

// --- admission -------------------------------------------------------------
AdmissionAssessment Governor::evaluate_admission(const RequestDescriptor& desc) const {
    std::lock_guard g(impl_->mutex);
    AdmissionAssessment a;
    std::string err;
    if (!desc.validate(err)) { a.verdict = AdmissionVerdict::INFEASIBLE; a.detail = err; return a; }
    const Policy* p = impl_->policy_store.current();
    if (p == nullptr) { a.verdict = AdmissionVerdict::INFEASIBLE; a.detail = "no policy"; return a; }
    const auto now = clock_.now();
    const Duration cap = desc.contract.hard_deadline.value_or(desc.contract.e2e_target);

    // Estimates from the predictor, falling back to class defaults.
    auto est = [&](Phase ph) -> Duration {
        auto pr = impl_->predictor.predict(phase_key(ph));
        if (pr) return pr->predicted;
        const SloClassSpec* spec = p->class_spec(desc.slo_class);
        if (spec) {
            std::size_t i = enum_index(ph);
            if (i < kPhaseCount && spec->phase_weights[i] > 0.0)
                return Duration(static_cast<std::int64_t>(static_cast<double>(ns_count(cap)) * spec->phase_weights[i]));
        }
        return Duration::zero();
    };
    const Duration q = est(Phase::QUEUEING);
    const Duration pf = est(Phase::PREFILL);
    const Duration dc = est(Phase::DECODE);
    a.estimated_queue_wait = q;
    // Token-scaled decode estimate.
    // dc already represents the total decode budget (either measured or the
    // class default scaled to the e2e budget); do not multiply by token count.
    Duration decode = dc;
    a.estimated_total = sat_add(sat_add(q, pf), decode);
    a.deadline_slack = clamp_nonneg(sat_sub(cap, a.estimated_total));
    double risk = 0.0;
    if (cap > Duration::zero()) risk = static_cast<double>(ns_count(a.estimated_total)) / static_cast<double>(ns_count(cap));
    a.completion_probability = std::max(0.0, 1.0 - risk);
    if (ns_count(a.estimated_total) == 0) { a.verdict = AdmissionVerdict::FEASIBLE; a.risk = RiskState::SAFE; }
    else if (a.estimated_total > cap) { a.verdict = AdmissionVerdict::INFEASIBLE; a.risk = RiskState::INFEASIBLE; }
    else if (risk > 0.85) { a.verdict = AdmissionVerdict::DEFERABLE; a.risk = RiskState::CRITICAL; a.deferable = true; }
    else if (risk > 0.60) { a.verdict = AdmissionVerdict::FEASIBLE_WITH_RISK; a.risk = RiskState::AT_RISK; }
    else { a.verdict = AdmissionVerdict::FEASIBLE; a.risk = RiskState::SAFE; }
    a.fail_fast = desc.contract.cancellation_policy == CancellationPolicy::FAIL_FAST;
    return a;
}
// ---------------------------------------------------------------------------
// Lifecycle / accounting helpers.
// ---------------------------------------------------------------------------
namespace {

void apply_lifecycle(RequestState& rs, ObservationType t, TimePoint now) {
    switch (t) {
        case ObservationType::QUEUE_ENTERED:
            rs.lifecycle = LifecycleState::QUEUED; rs.current_phase = Phase::QUEUEING; rs.phase_started_at = now; break;
        case ObservationType::QUEUE_EXITED:
            rs.queue_residence = sat_add(rs.queue_residence, saturating_elapsed(rs.phase_started_at, now));
            rs.phase_started_at = now; break;
        case ObservationType::BATCH_WAIT_STARTED:
            rs.lifecycle = LifecycleState::BATCHING; rs.current_phase = Phase::BATCH_WAIT;
            rs.batch_wait = Duration::zero(); rs.phase_started_at = now; break;
        case ObservationType::BATCH_SEALED:
            rs.current_phase = Phase::SCHEDULING; rs.phase_started_at = now; break;
        case ObservationType::DISPATCH_STARTED:
            rs.lifecycle = LifecycleState::DISPATCH; rs.current_phase = Phase::SCHEDULING;
            rs.phase_started_at = now; break;
        case ObservationType::DISPATCH_COMPLETED:
            rs.current_phase = Phase::TRANSFER; rs.phase_started_at = now; break;
        case ObservationType::TRANSFER_STARTED:
            rs.lifecycle = LifecycleState::TRANSFER; rs.current_phase = Phase::TRANSFER; rs.phase_started_at = now; break;
        case ObservationType::TRANSFER_COMPLETED:
            rs.current_phase = Phase::PREFILL; rs.phase_started_at = now; break;
        case ObservationType::PREFILL_STARTED:
            rs.lifecycle = LifecycleState::PREFILL; rs.current_phase = Phase::PREFILL; rs.phase_started_at = now; break;
        case ObservationType::PREFILL_CHUNK_COMPLETED:
            rs.current_phase = Phase::PREFILL; break;
        case ObservationType::FIRST_TOKEN:
            rs.lifecycle = LifecycleState::DECODE; rs.current_phase = Phase::DECODE; rs.phase_started_at = now; break;
        case ObservationType::DECODE_STEP:
            rs.current_phase = Phase::DECODE; break;
        case ObservationType::SPECULATION_STARTED:
            rs.lifecycle = LifecycleState::SPECULATION; rs.current_phase = Phase::SPEC_PROPOSAL; rs.phase_started_at = now; break;
        case ObservationType::VERIFICATION_COMPLETED:
            rs.current_phase = Phase::SPEC_VERIFY; rs.phase_started_at = now; break;
        case ObservationType::RETRY_SCHEDULED:
            rs.lifecycle = LifecycleState::RETRY; rs.current_phase = Phase::RETRY_BACKOFF; rs.phase_started_at = now; break;
        case ObservationType::RETRY_STARTED:
            rs.lifecycle = LifecycleState::RETRY; rs.phase_started_at = now; break;
        case ObservationType::RECOVERY_STARTED:
            rs.lifecycle = LifecycleState::RECOVERY; rs.current_phase = Phase::RECOVERY; rs.phase_started_at = now; break;
        default: break;
    }
}

// Idempotent terminalization: only transitions from an active state once.
void terminalize(RequestState& rs, LifecycleState term, MetricsSummary& m, RejectionCode reason, TimePoint now) {
    if (!rs.is_active()) return;
    rs.lifecycle = term;
    rs.terminal_reason = reason;
    rs.completed_at = now;
    if (m.active > 0) --m.active;
    if (term == LifecycleState::COMPLETED) { ++m.completed; }
    else if (term == LifecycleState::FAILED) { ++m.failed; }
    else if (term == LifecycleState::CANCELLED) { ++m.cancelled; }
}

[[nodiscard]] InterventionAction pick_action(const RiskAssessment& ra) noexcept {
    // Deterministic primary action by dominant contribution.
    if (ra.state == RiskState::INFEASIBLE) return InterventionAction::FAIL_FAST;
    if (!ra.contributions.empty()) {
        switch (ra.contributions.front().reason) {
            case ReasonCode::TTFT_AT_RISK: return InterventionAction::EXPEDITE;
            case ReasonCode::INTER_TOKEN_AT_RISK: return InterventionAction::PROTECT_DECODE;
            case ReasonCode::BATCH_WAIT_EXCEEDED: return InterventionAction::SEAL_BATCH_NOW;
            case ReasonCode::TRANSFER_COST_EXCEEDED: return InterventionAction::PREFER_LOCAL_EXECUTION;
            case ReasonCode::RETRY_RESERVE_EXHAUSTED: return InterventionAction::CANCEL_RETRY;
            case ReasonCode::SPECULATION_OVERHEAD: return InterventionAction::REDUCE_SPECULATION_DEPTH;
            case ReasonCode::RESOURCE_PRESSURE_HIGH: return InterventionAction::ESCALATE_PRIORITY;
            case ReasonCode::HARD_DEADLINE_EXPIRED: return InterventionAction::CANCEL;
            default: break;
        }
    }
    if (ra.state == RiskState::AT_RISK || ra.state == RiskState::CRITICAL) return InterventionAction::EXPEDITE;
    if (ra.state == RiskState::WATCH) return InterventionAction::CONTINUE;
    return InterventionAction::CONTINUE;
}

} // namespace

// --- admission -------------------------------------------------------------
AdmitResult Governor::admit(const RequestDescriptor& desc) {
    std::lock_guard g(impl_->mutex);
    AdmitResult res;
    std::string err;
    if (!desc.validate(err)) { res.detail = err; res.code = RejectionCode::INVALID_ID; return res; }
    const Policy* p = impl_->policy_store.current();
    if (p == nullptr) { res.detail = "no policy loaded"; res.code = RejectionCode::PROTOCOL_MISMATCH; return res; }
    if (impl_->metrics.active >= impl_->config.max_active) { res.detail = "active request limit"; res.code = RejectionCode::RATE_LIMITED; return res; }
    if (impl_->requests.count(desc.request_id) != 0) { res.detail = "duplicate request id"; res.code = RejectionCode::DUPLICATE; return res; }

    RequestState rs;
    rs.request_id = desc.request_id;
    rs.descriptor = desc;
    rs.descriptor.contract = finalize_contract(*p, desc);
    rs.attempt_id = AttemptId(impl_->next_attempt++);
    rs.generation = Generation(impl_->next_generation++);
    rs.epoch = impl_->epoch;
    rs.admitted_at = clock_.now();
    rs.last_update = rs.admitted_at;
    rs.phase_started_at = rs.admitted_at;
    rs.last_active = rs.admitted_at;
    rs.lifecycle = LifecycleState::ADMITTED;
    rs.current_phase = Phase::ADMISSION;
    rs.phase_budgets.set_partition(build_partition(*p, rs.descriptor));

    impl_->requests.emplace(rs.request_id, std::move(rs));
    ++impl_->metrics.active;
    impl_->metrics.per_class[enum_index(desc.slo_class)]++;
    impl_->metrics.admissions[enum_index(AdmissionVerdict::FEASIBLE)]++;
    impl_->push_event("admission", "req=" + desc.request_id.to_string());

    RequestState& stored = impl_->requests[desc.request_id];
    res.accepted = true; res.code = RejectionCode::NONE;
    res.request_id = desc.request_id;
    res.attempt_id = stored.attempt_id;
    res.generation = stored.generation;
    res.epoch = impl_->epoch;
    return res;
}

// --- observation -----------------------------------------------------------
ObservationResult Governor::observe(const Observation& obs) {
    std::lock_guard g(impl_->mutex);
    ObservationResult r;
    auto it = impl_->requests.find(obs.request_id);
    if (it == impl_->requests.end()) { r.code = RejectionCode::REQUEST_NOT_FOUND; r.detail = "request not found"; return r; }
    RequestState& rs = it->second;
    const RejectionCode code = check_env(rs, obs.epoch, obs.generation, obs.attempt_id, obs.dispatch_id, obs.worker_id, obs.worker_boot_id, impl_->epoch);
    if (code != RejectionCode::NONE) {
        r.code = code; r.detail = "stale authority envelope";
        ++impl_->metrics.stale_rejections[enum_index(code)];
        ++impl_->metrics.observations_rejected;
        return r;
    }
    if (obs.id.value() == 0) { /* id is optional; assign if zero */ }

    const TimePoint now = clock_.now();
    const Duration since_last = saturating_elapsed(rs.last_update, now);
    rs.elapsed_total = sat_add(rs.elapsed_total, since_last);
    rs.active_priority = static_cast<std::uint32_t>(impl_->decision_generation);
    rs.last_update = now;
    if (since_last > Duration::zero()) rs.last_active = now;

    // Account the measured phase elapsed.
    if (obs.elapsed && obs.phase != Phase::UNCLASSIFIED) {
        rs.phase_budgets.attribute(obs.phase, *obs.elapsed);
        rs.current_phase_elapsed = sat_add(rs.current_phase_elapsed, *obs.elapsed);
    }
    // Predictor update: record against the phase key (for governance prediction)
    // and against the explicit caller-supplied key when present.
    if (obs.elapsed) {
        if (obs.phase != Phase::UNCLASSIFIED) {
            impl_->predictor.record(phase_key(obs.phase), *obs.elapsed);
            ++impl_->metrics.predictors_updated;
        }
        if (!obs.predictor_key.empty()) impl_->predictor.record(obs.predictor_key, *obs.elapsed);
    }

    // Speculation acceptance rate is reported on verification completion.
    if (obs.type == ObservationType::VERIFICATION_COMPLETED && obs.value) {
        rs.spec_acceptance_rate = *obs.value;
    }

    // Dispatch binds worker identity.
    if (obs.type == ObservationType::DISPATCH_STARTED) {
        rs.worker_id = obs.worker_id;
        rs.worker_boot_id = obs.worker_boot_id;
        rs.dispatch_id = obs.dispatch_id;
    }

    apply_lifecycle(rs, obs.type, now);

    // Fairness accounting.
    impl_->fairness.account(rs.descriptor.tenant_id, rs.descriptor.slo_class, since_last,
                            now.time_since_epoch(), rs.is_active(), rs.descriptor.contract.fairness_weight);

    // Metrics.
    ++impl_->metrics.observations_received;
    if (obs.elapsed && obs.phase != Phase::UNCLASSIFIED) {
        impl_->metrics.phase_latency[enum_index(obs.phase)].record(*obs.elapsed);
    }
    if (obs.type == ObservationType::DECODE_STEP && obs.elapsed) {
        impl_->metrics.decode_step_latency.record(*obs.elapsed);
    }
    if (obs.type == ObservationType::PREFILL_CHUNK_COMPLETED && obs.elapsed) {
        impl_->metrics.prefill_chunk_latency.record(*obs.elapsed);
    }
    impl_->push_event(to_string(obs.type), "req=" + obs.request_id.to_string());

    // Observations that are terminal by nature.
    if (obs.type == ObservationType::FAILURE) {
        terminalize(rs, LifecycleState::FAILED, impl_->metrics, RejectionCode::BACKEND_UNAVAILABLE, now);
    } else if (obs.type == ObservationType::CANCELLATION) {
        terminalize(rs, LifecycleState::CANCELLED, impl_->metrics, RejectionCode::REQUEST_CANCELLED, now);
    }

    r.accepted = true; r.code = RejectionCode::NONE;
    return r;
}
// ---------------------------------------------------------------------------
// Locked helpers.
// ---------------------------------------------------------------------------
RequestState* Governor::find_locked(const RequestId& id) noexcept {
    auto it = impl_->requests.find(id);
    return it == impl_->requests.end() ? nullptr : &it->second;
}
const RequestState* Governor::find_locked(const RequestId& id) const noexcept {
    auto it = impl_->requests.find(id);
    return it == impl_->requests.end() ? nullptr : &it->second;
}

namespace {
[[nodiscard]] Intervention recommend(const RequestState& rs, const RiskAssessment& ra,
                                     InterventionAction action, ReasonCode reason, TimePoint at) {
    Intervention iv;
    iv.action = action; iv.reason = reason; iv.request_id = rs.request_id; iv.attempt_id = rs.attempt_id;
    iv.phase = rs.current_phase; iv.remaining_budget = rs.remaining_budget();
    iv.predicted_remaining = rs.predicted_remaining ? rs.predicted_remaining->predicted : Duration::zero();
    iv.risk = ra.state; iv.at = at;
    iv.target_worker = rs.worker_id;
    for (const auto& c : ra.contributions) iv.supporting_reasons.push_back(c.reason);
    iv.detail = std::string(to_string(action)) + " due to " + to_string(reason);
    return iv;
}
} // namespace

RiskAssessment Governor::evaluate_locked(RequestState& rs, double rp) noexcept {
    const TimePoint now = clock_.now();
    rs.elapsed_total = sat_add(rs.elapsed_total, saturating_elapsed(rs.last_update, now));
    rs.last_update = now;
    const Policy* p = impl_->policy_store.current();
    const double rpres = (rp < 0.0) ? impl_->config.default_resource_pressure : rp;
    if (p == nullptr) return {};
    // Derive a per-request predicted remaining latency from the predictor.
    // Cold start is explicit: no evidence means no prediction (fallback).
    {
        Duration pred{0};
        std::size_t evidence = 0;
        bool have = false;
        auto pf = impl_->predictor.predict(phase_key(Phase::PREFILL));
        auto dc = impl_->predictor.predict(phase_key(Phase::DECODE));
        if (pf) { pred = sat_add(pred, pf->predicted); evidence += pf->evidence_count; have = true; }
        if (dc) { pred = sat_add(pred, dc->predicted); evidence += dc->evidence_count; have = true; }
        if (have) {
            Prediction pr;
            pr.predicted = pred; pr.lower = pred; pr.upper = pred;
            pr.confidence = (evidence >= impl_->predictor.config().high_confidence_evidence) ? ConfidenceClass::HIGH
                           : (evidence >= impl_->predictor.config().medium_confidence_evidence) ? ConfidenceClass::MEDIUM
                           : ConfidenceClass::LOW;
            pr.source = PredictionSource::MEASURED;
            pr.evidence_count = evidence;
            pr.predictor_generation = impl_->predictor.generation();
            pr.version = 1;
            rs.predicted_remaining = pr;
        }
    }
    RiskAssessment ra = evaluate_risk(rs, *p, rpres);
    rs.risk = ra.state;
    rs.completion_probability = ra.completion_probability;
    if (ra.prediction) rs.predicted_remaining = ra.prediction;
    return ra;
}

InterventionPlan Governor::plan_locked(RequestState& rs, double rp, bool record) noexcept {
    InterventionPlan plan;
    plan.decision_generation = ++impl_->decision_generation;
    RiskAssessment ra = evaluate_locked(rs, rp);
    const Policy* p = impl_->policy_store.current();
    if (p == nullptr) return plan;
    const InterventionAction action = pick_action(ra);
    const ReasonCode reason = ra.contributions.empty() ? ReasonCode::DEADLINE_SLACK : ra.contributions.front().reason;
    Intervention iv = recommend(rs, ra, action, reason, clock_.now());
    iv.policy_generation = p->generation;
    iv.decision_generation = plan.decision_generation;
    plan.items.push_back(iv);
    ++impl_->metrics.decisions;
    if (record) {
        InterventionRecord rec;
        rec.action = iv.action; rec.reason = iv.reason;
        rec.remaining_budget = iv.remaining_budget; rec.predicted_remaining = iv.predicted_remaining;
        rec.risk_before = iv.risk; rec.policy_generation = iv.policy_generation;
        rec.decision_generation = iv.decision_generation; rec.at = iv.at; rec.detail = iv.detail;
        rs.interventions.push_back(rec);
        if (rs.interventions.size() > impl_->config.max_intervention_history) rs.interventions.erase(rs.interventions.begin());
        ++rs.interventions_received;
        ++impl_->metrics.interventions[enum_index(action)];
        impl_->push_event("intervention", std::string(to_string(action)) + ":" + to_string(reason));
    }
    return plan;
}

RiskAssessment Governor::assess(const RequestId& id, double rp) {
    std::lock_guard g(impl_->mutex);
    RequestState* rs = find_locked(id);
    if (rs == nullptr) return {};
    return evaluate_locked(*rs, rp);
}

InterventionPlan Governor::plan(const RequestId& id, double rp) {
    std::lock_guard g(impl_->mutex);
    RequestState* rs = find_locked(id);
    if (rs == nullptr) return {};
    return plan_locked(*rs, rp, true);
}

Explanation Governor::explain(const RequestId& id) const {
    std::lock_guard g(impl_->mutex);
    Explanation x;
    const RequestState* rs = find_locked(id);
    if (rs == nullptr) { x.title = "request not found"; return x; }
    const Policy* p = impl_->policy_store.current();
    x.request_id = id;
    x.decision_generation = impl_->decision_generation;
    x.policy_generation = p ? p->generation : 0;
    const double rpres = impl_->config.default_resource_pressure;
    const RiskAssessment ra = p ? evaluate_risk(*rs, *p, rpres) : RiskAssessment{};
    x.title = "request " + rs->request_id.to_string() + " lifecycle=" + to_string(rs->lifecycle);
    x.add_line("risk=" + std::string(to_string(ra.state)));
    x.add_line("remaining_budget_ns=" + std::to_string(ns_count(ra.remaining_budget)));
    x.add_line("elapsed_total_ns=" + std::to_string(ns_count(rs->elapsed_total)));
    x.add_line("budget_used_fraction=" + std::to_string(ra.budget_used_fraction));
    x.add_line("deadline_risk=" + std::to_string(ra.deadline_risk));
    x.add_line("completion_probability=" + std::to_string(ra.completion_probability));
    x.add_line("predicted_remaining_ns=" + std::to_string(rs->predicted_remaining ? ns_count(rs->predicted_remaining->predicted) : 0));
    x.add_line("policy_generation=" + std::to_string(x.policy_generation));
    for (const auto& c : ra.contributions) {
        x.add_factor(c.reason, c.threshold, c.observed,
                     std::string("weight=") + std::to_string(c.weight));
    }
    return x;
}

// ---------------------------------------------------------------------------
// commit
// ---------------------------------------------------------------------------
bool Governor::commit(const Completion& comp, std::string& error) {
    std::lock_guard g(impl_->mutex);
    RequestState* rs = find_locked(comp.request_id);
    if (rs == nullptr) { error = "request not found"; return false; }
    const RejectionCode code = check_env(*rs, comp.epoch, comp.generation, comp.attempt_id, comp.dispatch_id,
                                         comp.worker_id, comp.worker_boot_id, impl_->epoch);
    if (code != RejectionCode::NONE) {
        error = "stale authority: " + std::string(to_string(code));
        ++impl_->metrics.stale_rejections[enum_index(code)];
        return false;
    }
    if (rs->is_terminal()) { error = "already terminal"; return false; }
    const TimePoint now = clock_.now();
    rs->elapsed_total = sat_add(rs->elapsed_total, saturating_elapsed(rs->last_update, now));
    rs->last_update = now;
    rs->slo_met = comp.slo_met;
    rs->soft_violation = comp.soft_violation;
    rs->hard_violation = comp.hard_violation;

    if (comp.outcome == Outcome::COMPLETED) {
        terminalize(*rs, LifecycleState::COMPLETED, impl_->metrics, RejectionCode::NONE, now);
        if (comp.slo_met) ++impl_->metrics.slo_met;
        if (comp.soft_violation) ++impl_->metrics.soft_violation;
        if (comp.hard_violation) ++impl_->metrics.hard_violation;
    } else if (comp.outcome == Outcome::FAILED) {
        terminalize(*rs, LifecycleState::FAILED, impl_->metrics, RejectionCode::BACKEND_UNAVAILABLE, now);
    } else {
        terminalize(*rs, LifecycleState::CANCELLED, impl_->metrics, RejectionCode::REQUEST_CANCELLED, now);
    }
    // Deterministically release all reservations (idempotent).
    for (auto& res : rs->reservations) res.released = true;

    impl_->completion_order.push_back(rs->request_id);
    if (impl_->completion_order.size() > impl_->config.max_completed_history) {
        const RequestId evict = impl_->completion_order.front();
        impl_->completion_order.pop_front();
        auto eit = impl_->requests.find(evict);
        if (eit != impl_->requests.end() && eit->second.is_terminal()) impl_->requests.erase(eit);
    }
    if (comp.outcome == Outcome::COMPLETED) impl_->push_event("completion", "req=" + comp.request_id.to_string() + " met=" + (comp.slo_met ? "1" : "0"));
    else if (comp.outcome == Outcome::FAILED) impl_->push_event("completion", "req=" + comp.request_id.to_string() + " failed");
    else impl_->push_event("completion", "req=" + comp.request_id.to_string() + " cancelled");

    error.clear();
    return true;
}
// ---------------------------------------------------------------------------
// Governance hooks.
// ---------------------------------------------------------------------------
QueueGovernance Governor::govern_queue(const RequestId& id, double rp) {
    std::lock_guard g(impl_->mutex);
    QueueGovernance q;
    RequestState* rs = find_locked(id);
    if (rs == nullptr) return q;
    const RiskAssessment ra = evaluate_locked(*rs, rp);
    const TimePoint now = clock_.now();
    q.residence = rs->queue_residence;
    q.starvation_age = saturating_elapsed(rs->last_active, now);
    q.deadline_slack = rs->remaining_budget();
    q.predicted_dispatch_delay = rs->predicted_remaining ? rs->predicted_remaining->predicted : Duration::zero();
    if (rs->remaining_budget() > Duration::zero())
        q.urgency = std::min(1.0, static_cast<double>(ns_count(q.predicted_dispatch_delay)) / static_cast<double>(ns_count(rs->remaining_budget())));
    const auto& contract = rs->descriptor.contract;
    const Policy* p = impl_->policy_store.current();
    const std::uint64_t pgen = p ? p->generation : 0;
    const ReasonCode reason = ra.contributions.empty() ? ReasonCode::DEADLINE_SLACK : ra.contributions.front().reason;
    InterventionAction action = InterventionAction::CONTINUE;
    if (contract.max_queue_residence > Duration::zero() && q.residence > contract.max_queue_residence) {
        q.force_dispatch = true; q.bypass_batch = true; q.escalate_priority = true; action = InterventionAction::EXPEDITE;
    } else if (ra.state == RiskState::AT_RISK || ra.state == RiskState::CRITICAL) {
        q.escalate_priority = true; q.force_dispatch = true; action = InterventionAction::ESCALATE_PRIORITY;
    } else if (ra.state == RiskState::INFEASIBLE) {
        q.fail_fast = true; q.defer = true; action = InterventionAction::FAIL_FAST;
    } else {
        action = pick_action(ra);
    }
    q.recommendation = recommend(*rs, ra, action, reason, now);
    q.recommendation.policy_generation = pgen;
    return q;
}

BatchGovernance Governor::govern_batch(const std::vector<RequestId>& batch, Duration current_wait) {
    std::lock_guard g(impl_->mutex);
    BatchGovernance b;
    const Policy* p = impl_->policy_store.current();
    if (p == nullptr) { b.seal_now = false; return b; }
    Duration oldest{0};
    Duration max_target{0}, min_target{0};
    bool have_min = false;
    std::size_t at_risk = 0;
    for (const auto& id : batch) {
        RequestState* rs = find_locked(id);
        if (rs == nullptr) continue;
        const Duration wait = saturating_elapsed(rs->phase_started_at, clock_.now());
        if (wait > oldest) oldest = wait;
        const Duration tl = rs->descriptor.contract.hard_deadline.value_or(rs->descriptor.contract.e2e_target);
        if (tl > max_target) max_target = tl;
        if (!have_min || tl < min_target) { min_target = tl; have_min = true; }
        if (evaluate_locked(*rs, impl_->config.default_resource_pressure).state != RiskState::SAFE) ++at_risk;
    }
    b.oldest_wait = oldest;
    b.deadline_spread = have_min ? clamp_nonneg(sat_sub(max_target, min_target)) : Duration::zero();
    // Estimated batch cost from the predictor (BATCH_WAIT is a proxy for wait cost).
    auto pr = impl_->predictor.predict(phase_key(Phase::SCHEDULING));
    b.predicted_batch_cost = pr ? pr->predicted : Duration::zero();
    const std::size_t n = batch.empty() ? 1 : batch.size();
    b.batch_efficiency = 1.0 - static_cast<double>(at_risk) / static_cast<double>(n);
    const Duration max_wait = p->batch.default_max_batch_wait;
    if (max_wait > Duration::zero() && current_wait >= max_wait) {
        b.seal_now = true; b.max_additional_wait = Duration::zero();
    } else {
        b.max_additional_wait = max_wait > Duration::zero() ? clamp_nonneg(sat_sub(max_wait, current_wait)) : Duration::zero();
    }
    if (at_risk > 0) { b.seal_now = true; b.shrink = true; }
    return b;
}

PrefillGovernance Governor::govern_prefill(const RequestId& id, double rp) {
    std::lock_guard g(impl_->mutex);
    PrefillGovernance pg;
    RequestState* rs = find_locked(id);
    if (rs == nullptr) return pg;
    const RiskAssessment ra = evaluate_locked(*rs, rp);
    const TimePoint now = clock_.now();
    const auto pr = impl_->predictor.predict(phase_key(Phase::PREFILL));
    pg.predicted_chunk_cost = pr ? pr->predicted : Duration::zero();
    const Duration remaining_prefill = clamp_nonneg(sat_sub(rs->descriptor.contract.prefill_target, rs->phase_budgets.consumed(Phase::PREFILL)));
    pg.predicted_remaining_prefill = remaining_prefill;
    const ReasonCode reason = ra.contributions.empty() ? ReasonCode::DEADLINE_SLACK : ra.contributions.front().reason;
    InterventionAction action;
    if (ra.ttf_at_risk) { pg.protect_ttft = true; pg.expedite = true; action = InterventionAction::EXPEDITE; }
    else if (ra.state == RiskState::INFEASIBLE) { pg.fail_fast = true; action = InterventionAction::FAIL_FAST; }
    else if (rs->descriptor.contract.prefill_target > Duration::zero() &&
             rs->phase_budgets.consumed(Phase::PREFILL) >= rs->descriptor.contract.prefill_target) {
        pg.yield_after_chunk = true; action = InterventionAction::YIELD_PREFILL;
    } else { pg.continue_chunk = true; action = InterventionAction::CONTINUE_PREFILL; }
    pg.recommendation = recommend(*rs, ra, action, reason, now);
    return pg;
}

DecodeGovernance Governor::govern_decode(const RequestId& id, double rp) {
    std::lock_guard g(impl_->mutex);
    DecodeGovernance dg;
    RequestState* rs = find_locked(id);
    if (rs == nullptr) return dg;
    const RiskAssessment ra = evaluate_locked(*rs, rp);
    const TimePoint now = clock_.now();
    const auto pr = impl_->predictor.predict(phase_key(Phase::DECODE));
    dg.predicted_step = pr ? pr->predicted : Duration::zero();
    dg.deadline_slack = rs->remaining_budget();
    const ReasonCode reason = ra.contributions.empty() ? ReasonCode::DEADLINE_SLACK : ra.contributions.front().reason;
    InterventionAction action;
    if (rs->hard_deadline_exceeded() || ra.state == RiskState::INFEASIBLE) {
        dg.cancel_if_impossible = true; action = InterventionAction::FAIL_FAST;
    } else if (ra.inter_token_at_risk) {
        dg.protect_sequence = true; dg.expedite_step = true; dg.favor_smaller_batch = true; action = InterventionAction::EXPEDITE;
    } else if (ra.state == RiskState::AT_RISK || ra.state == RiskState::CRITICAL) {
        dg.expedite_step = true; action = InterventionAction::EXPEDITE;
    } else if (ra.state == RiskState::WATCH && rs->descriptor.contract.degradation_allowed) {
        dg.mark_soft_violation = true; action = InterventionAction::MARK_SLO_VIOLATION;
    } else { action = InterventionAction::CONTINUE; }
    dg.recommendation = recommend(*rs, ra, action, reason, now);
    return dg;
}

SpeculationGovernance Governor::govern_speculation(const RequestId& id, double rp) {
    std::lock_guard g(impl_->mutex);
    SpeculationGovernance sg;
    RequestState* rs = find_locked(id);
    if (rs == nullptr) return sg;
    const RiskAssessment ra = evaluate_locked(*rs, rp);
    const TimePoint now = clock_.now();
    const Policy* p = impl_->policy_store.current();
    const SpeculationPolicy spol = p ? p->speculation : SpeculationPolicy{};
    sg.acceptance_rate = rs->spec_acceptance_rate;
    sg.remaining_budget = rs->remaining_budget();
    sg.predicted_overhead = Duration(static_cast<std::int64_t>(static_cast<double>(ns_count(rs->descriptor.contract.max_spec_overhead)) * 0.5));
    const ReasonCode reason = ra.contributions.empty() ? ReasonCode::DEADLINE_SLACK : ra.contributions.front().reason;
    InterventionAction action;
    if (ra.state == RiskState::INFEASIBLE) { sg.disable = true; action = InterventionAction::DISABLE_SPECULATION; }
    else if (rs->spec_depth > spol.default_max_depth ||
             (rs->descriptor.contract.max_spec_overhead > Duration::zero() &&
              rs->phase_budgets.consumed(Phase::SPEC_PROPOSAL) > rs->descriptor.contract.max_spec_overhead)) {
        sg.reduce_depth = true; action = InterventionAction::REDUCE_SPECULATION_DEPTH;
    } else if (spol.min_acceptance_rate > 0.0 && sg.acceptance_rate > 0.0 && sg.acceptance_rate < spol.min_acceptance_rate) {
        sg.disable = true; action = InterventionAction::DISABLE_SPECULATION;
    } else if (ra.state == RiskState::WATCH || ra.state == RiskState::SAFE) {
        sg.preserve = true; action = InterventionAction::PRESERVE_SPECULATION;
    } else { sg.reduce_depth = true; action = InterventionAction::REDUCE_SPECULATION_DEPTH; }
    sg.recommendation = recommend(*rs, ra, action, reason, now);
    if (action == InterventionAction::REDUCE_SPECULATION_DEPTH) rs->speculation_was_reduced = true;
    return sg;
}

TransferGovernance Governor::govern_transfer(const RequestId& id, Bytes size, Duration predicted_transfer) {
    (void)size;
    std::lock_guard g(impl_->mutex);
    TransferGovernance tg;
    RequestState* rs = find_locked(id);
    if (rs == nullptr) return tg;
    const RiskAssessment ra = evaluate_locked(*rs, impl_->config.default_resource_pressure);
    const Policy* p = impl_->policy_store.current();
    tg.predicted_transfer = predicted_transfer;
    tg.remaining_budget = rs->remaining_budget();
    if (rs->remaining_budget() > Duration::zero())
        tg.transfer_fraction_of_budget = static_cast<double>(ns_count(predicted_transfer)) / static_cast<double>(ns_count(rs->remaining_budget()));
    const double max_frac = p ? p->transfer.max_transfer_fraction_of_budget : 0.20;
    const bool risky = tg.transfer_fraction_of_budget > max_frac ||
                       rs->phase_budgets.consumed(Phase::TRANSFER) > rs->descriptor.contract.transfer_allowance;
    const ReasonCode reason = risky ? ReasonCode::TRANSFER_COST_EXCEEDED : ReasonCode::DEADLINE_SLACK;
    if (predicted_transfer > rs->remaining_budget() && rs->remaining_budget() > Duration::zero()) {
        tg.infeasible = true;
        tg.recommendation = recommend(*rs, ra, InterventionAction::PREFER_LOCAL_EXECUTION, ReasonCode::TRANSFER_COST_EXCEEDED, clock_.now());
    } else if (risky) {
        tg.prefer_local = true; tg.expedite = true;
        tg.recommendation = recommend(*rs, ra, InterventionAction::PREFER_LOCAL_EXECUTION, reason, clock_.now());
    } else {
        tg.allowed = true;
        tg.recommendation = recommend(*rs, ra, InterventionAction::ALLOW_TRANSFER, reason, clock_.now());
    }
    return tg;
}

RetryGovernance Governor::govern_retry(const RequestId& id, Duration predicted_retry) {
    std::lock_guard g(impl_->mutex);
    RetryGovernance rg;
    RequestState* rs = find_locked(id);
    if (rs == nullptr) return rg;
    const RiskAssessment ra = evaluate_locked(*rs, impl_->config.default_resource_pressure);
    const Policy* p = impl_->policy_store.current();
    const RetryPolicy rpol = p ? p->retry : RetryPolicy{};
    rg.retry_count = rs->retry_count;
    rg.predicted_retry_duration = predicted_retry;
    rg.remaining_budget = rs->remaining_budget();
    rg.max_cumulative_retry_delay = rpol.max_cumulative_retry_delay;
    const Duration last_cumulative = sat_add(rs->retry_accumulated, predicted_retry);
    if (rs->retry_count >= rpol.max_retries ||
        (rpol.max_cumulative_retry_delay > Duration::zero() && last_cumulative > rpol.max_cumulative_retry_delay) ||
        (rs->descriptor.contract.retry_allowance > Duration::zero() && last_cumulative > rs->descriptor.contract.retry_allowance)) {
        rg.prohibited = true; rg.fail_fast = true;
        rg.recommendation = recommend(*rs, ra, InterventionAction::CANCEL_RETRY, ReasonCode::RETRY_RESERVE_EXHAUSTED, clock_.now());
    } else if (ra.state == RiskState::AT_RISK || ra.state == RiskState::CRITICAL) {
        rg.limited_backoff = true; rg.allowed = true;
        rg.recommendation = recommend(*rs, ra, InterventionAction::LIMIT_RETRY_BACKOFF, ReasonCode::DEADLINE_SLACK, clock_.now());
    } else if (predicted_retry <= rs->remaining_budget()) {
        rg.allowed = true; rg.immediate = rpol.allow_immediate_retry;
        rg.recommendation = recommend(*rs, ra, rpol.allow_immediate_retry ? InterventionAction::ALLOW_RETRY : InterventionAction::CONTINUE,
                                      ReasonCode::DEADLINE_SLACK, clock_.now());
    } else {
        rg.prohibited = true;
        rg.recommendation = recommend(*rs, ra, InterventionAction::CANCEL_RETRY, ReasonCode::BUDGET_EXHAUSTED, clock_.now());
    }
    return rg;
}

// ---------------------------------------------------------------------------
// Reservations / workers.
// ---------------------------------------------------------------------------
bool Governor::reserve(const RequestId& id, BackendId backend, Bytes amount, ReservationId& out_id, std::string& error) {
    std::lock_guard g(impl_->mutex);
    RequestState* rs = find_locked(id);
    if (rs == nullptr) { error = "request not found"; return false; }
    if (rs->is_terminal()) { error = "request is terminal"; return false; }
    ReservationId rid(impl_->next_reservation++);
    rs->reservations.push_back({rid, backend, amount, false});
    out_id = rid;
    error.clear();
    return true;
}

bool Governor::register_worker(const WorkerDescriptor& wd, std::string& error) {
    std::lock_guard g(impl_->mutex);
    if (impl_->workers.size() >= impl_->config.max_workers) { error = "worker limit"; return false; }
    if (impl_->workers.count(wd.id) && impl_->workers[wd.id].boot_id != wd.boot_id) {
        // new incarnation overwrites the old registration.
    }
    impl_->workers[wd.id] = wd;
    impl_->workers[wd.id].alive = true;
    impl_->workers[wd.id].last_heartbeat = clock_.now();
    impl_->push_event("worker_join", wd.id.to_string() + "@" + wd.boot_id.to_string());
    error.clear();
    return true;
}

bool Governor::unregister_worker(WorkerId id, WorkerBootId boot_id, std::string& error) {
    std::lock_guard g(impl_->mutex);
    auto it = impl_->workers.find(id);
    if (it == impl_->workers.end() || it->second.boot_id != boot_id) {
        error = "unknown worker incarnation";
        return false;
    }
    it->second.alive = false;
    impl_->push_event("worker_lost", id.to_string() + "@" + boot_id.to_string());
    error.clear();
    return true;
}

std::size_t Governor::worker_count() const noexcept {
    std::lock_guard g(impl_->mutex);
    return impl_->workers.size();
}

// ---------------------------------------------------------------------------
// Observability.
// ---------------------------------------------------------------------------
MetricsSummary Governor::metrics() const {
    std::lock_guard g(impl_->mutex);
    return impl_->metrics;
}

std::vector<RequestSnapshot> Governor::list_requests() const {
    std::lock_guard g(impl_->mutex);
    std::vector<RequestSnapshot> out;
    for (const auto& [id, rs] : impl_->requests) {
        if (!rs.is_active()) continue;
        if (out.size() >= impl_->config.max_snapshot_requests) break;
        RequestSnapshot r;
        r.request_id = rs.request_id; r.attempt_id = rs.attempt_id;
        r.lifecycle = rs.lifecycle; r.phase = rs.current_phase;
        r.slo_class = rs.descriptor.slo_class; r.tenant_id = rs.descriptor.tenant_id;
        r.risk = rs.risk; r.remaining_budget = rs.remaining_budget();
        r.elapsed_total = rs.elapsed_total; r.generation = rs.generation.value(); r.epoch = rs.epoch.value();
        out.push_back(r);
    }
    return out;
}

namespace {
std::string build_snapshot_json(const Snapshot& s) {
    std::ostringstream os;
    os << "{\"epoch\":" << s.coordinator_epoch
       << ",\"decision_generation\":" << s.decision_generation
       << ",\"event_sequence\":" << s.event_sequence
       << ",\"active\":" << s.summary.active
       << ",\"completed\":" << s.summary.completed
       << ",\"failed\":" << s.summary.failed
       << ",\"cancelled\":" << s.summary.cancelled
       << ",\"slo_met\":" << s.summary.slo_met
       << ",\"soft_violation\":" << s.summary.soft_violation
       << ",\"hard_violation\":" << s.summary.hard_violation
       << ",\"requests\":[";
    for (std::size_t i = 0; i < s.requests.size(); ++i) {
        if (i) os << ",";
        const auto& r = s.requests[i];
        os << "{\"id\":\"" << r.request_id.to_string() << "\",\"risk\":\""
           << to_string(r.risk) << "\",\"lifecycle\":\"" << to_string(r.lifecycle) << "\"}";
    }
    os << "]}";
    return os.str();
}
} // namespace

Snapshot Governor::snapshot() const {
    std::lock_guard g(impl_->mutex);
    Snapshot s;
    s.at = clock_.now();
    s.coordinator_epoch = impl_->epoch.value();
    s.decision_generation = impl_->decision_generation;
    s.event_sequence = impl_->event_sequence;
    s.summary = impl_->metrics;
    for (const auto& [id, rs] : impl_->requests) {
        if (!rs.is_active()) continue;
        if (s.requests.size() >= impl_->config.max_snapshot_requests) break;
        RequestSnapshot r;
        r.request_id = rs.request_id; r.attempt_id = rs.attempt_id;
        r.lifecycle = rs.lifecycle; r.phase = rs.current_phase;
        r.slo_class = rs.descriptor.slo_class; r.tenant_id = rs.descriptor.tenant_id;
        r.risk = rs.risk; r.remaining_budget = rs.remaining_budget();
        r.elapsed_total = rs.elapsed_total; r.generation = rs.generation.value(); r.epoch = rs.epoch.value();
        s.requests.push_back(r);
    }
    // Bounded event tail, oldest first.
    const std::size_t start = impl_->events.size() > impl_->config.max_snapshot_requests
                                  ? impl_->events.size() - impl_->config.max_snapshot_requests : 0;
    for (std::size_t i = start; i < impl_->events.size(); ++i) s.events.push_back(impl_->events[i]);
    // JSON form.
    s.json = build_snapshot_json(s);
    return s;
}


// ---------------------------------------------------------------------------
// Persistence: encode/decode of authoritative governor state.
// ---------------------------------------------------------------------------
namespace {

[[nodiscard]] bool is_finite(double v) noexcept { return std::isfinite(v); }

void write_double(BinaryWriter& w, double v) { w.u64(std::bit_cast<std::uint64_t>(v)); }
bool read_double(BinaryReader& r, double& v) {
    std::uint64_t u; if (!r.u64(u)) return false;
    v = std::bit_cast<double>(u);
    return std::isfinite(v);
}
template <typename E> void write_enum(BinaryWriter& w, E e) { w.enums_raw(static_cast<std::uint32_t>(e)); }
template <typename E> bool read_enum(BinaryReader& r, E& e) {
    std::uint32_t v; if (!r.enums_raw(v)) return false;
    if (v >= enum_count<E>()) return false;
    e = static_cast<E>(v); return true;
}

template <typename T, typename Tag, typename Rep>
void write_id(BinaryWriter& w, const T& id) { w.id(id); }
template <typename Tag, typename Rep>
bool read_id(BinaryReader& r, StrongId<Tag, Rep>& out) { return r.id(out); }

void encode_contract(BinaryWriter& w, const LatencyContract& c) {
    w.u64(c.policy_id);
    w.duration(c.e2e_target);
    write_optional_duration(w, c.hard_deadline);
    w.duration(c.ttf_target); w.duration(c.max_queue_residence); w.duration(c.max_dispatch_delay);
    w.duration(c.prefill_target); w.duration(c.decode_step_target); w.duration(c.transfer_allowance);
    w.duration(c.retry_allowance); w.duration(c.max_retry_delay); w.duration(c.max_spec_overhead);
    write_double(w, c.deadline_risk_threshold); write_double(w, c.min_completion_probability);
    write_double(w, c.intervention_aggressiveness); write_double(w, c.fairness_weight);
    write_enum(w, c.admission_policy); write_enum(w, c.cancellation_policy);
    w.bool_(c.degradation_allowed);
}
bool decode_contract(BinaryReader& r, LatencyContract& c) {
    if (!r.u64(c.policy_id)) return false;
    if (!r.duration(c.e2e_target)) return false;
    if (!read_optional_duration(r, c.hard_deadline)) return false;
    if (!r.duration(c.ttf_target) || !r.duration(c.max_queue_residence) || !r.duration(c.max_dispatch_delay)) return false;
    if (!r.duration(c.prefill_target) || !r.duration(c.decode_step_target) || !r.duration(c.transfer_allowance)) return false;
    if (!r.duration(c.retry_allowance) || !r.duration(c.max_retry_delay) || !r.duration(c.max_spec_overhead)) return false;
    if (!read_double(r, c.deadline_risk_threshold) || !read_double(r, c.min_completion_probability)) return false;
    if (!read_double(r, c.intervention_aggressiveness) || !read_double(r, c.fairness_weight)) return false;
    if (!read_enum(r, c.admission_policy) || !read_enum(r, c.cancellation_policy)) return false;
    if (!r.bool_(c.degradation_allowed)) return false;
    return true;
}

void encode_descriptor(BinaryWriter& w, const RequestDescriptor& d) {
    w.id(d.request_id); w.id(d.tenant_id); w.id(d.model_id); w.id(d.model_revision);
    w.bool_(d.adapter_id.has_value()); if (d.adapter_id) w.id(*d.adapter_id);
    write_enum(w, d.slo_class);
    encode_contract(w, d.contract);
    w.u32(d.prompt_tokens); w.u32(d.max_tokens); w.u32(d.remaining_tokens);
    w.bool_(d.backend_hint.has_value()); if (d.backend_hint) w.id(*d.backend_hint);
    write_enum(w, d.device_hint);
    w.bool_(d.warm_cache_hint);
}
bool decode_descriptor(BinaryReader& r, RequestDescriptor& d) {
    if (!r.id(d.request_id) || !r.id(d.tenant_id) || !r.id(d.model_id) || !r.id(d.model_revision)) return false;
    bool has; if (!r.bool_(has)) return false; d.adapter_id.reset();
    if (has) { AdapterId a; if (!r.id(a)) return false; d.adapter_id = a; }
    if (!read_enum(r, d.slo_class)) return false;
    if (!decode_contract(r, d.contract)) return false;
    if (!r.u32(d.prompt_tokens) || !r.u32(d.max_tokens) || !r.u32(d.remaining_tokens)) return false;
    if (!r.bool_(has)) return false; d.backend_hint.reset();
    if (has) { BackendId b; if (!r.id(b)) return false; d.backend_hint = b; }
    if (!read_enum(r, d.device_hint)) return false;
    if (!r.bool_(d.warm_cache_hint)) return false;
    return true;
}

void encode_policy(BinaryWriter& w, const Policy& p) {
    w.u64(p.id); w.u64(p.generation); w.string(p.name);
    w.u32(static_cast<std::uint32_t>(p.classes.size()));
    for (const auto& c : p.classes) {
        w.string(c.name); write_double(w, c.priority);
        w.duration(c.default_e2e_target); w.duration(c.default_ttf_target);
        write_double(w, c.default_deadline_risk_threshold); write_double(w, c.default_min_completion_probability);
        write_double(w, c.default_fairness_weight); w.bool_(c.default_degradation_allowed);
        for (double x : c.phase_weights) write_double(w, x);
        write_double(w, c.reserve_fraction);
    }
    w.u32(p.retry.max_retries); w.duration(p.retry.max_cumulative_retry_delay);
    write_double(w, p.retry.backoff_base_ms); write_double(w, p.retry.backoff_factor); w.bool_(p.retry.allow_immediate_retry);
    write_double(w, p.transfer.max_transfer_fraction_of_budget); w.bool_(p.transfer.prefer_local_when_risky);
    w.u32(p.speculation.default_max_depth); write_double(w, p.speculation.min_acceptance_rate);
    write_double(w, p.speculation.max_overhead_fraction_of_budget);
    w.u32(p.fairness.max_starvation_ratio); write_double(w, p.fairness.background_min_service_fraction);
    w.bool_(p.fairness.protect_lower_classes_strongly);
    w.duration(p.batch.default_max_batch_wait); write_double(w, p.batch.deadline_spread_ratio); write_double(w, p.batch.min_batch_efficiency);
    write_double(w, p.resource_pressure_sensitivity); w.u64(p.prediction_min_evidence);
    w.bool_(p.fail_fast_on_deadline_exceeded); w.bool_(p.allow_completion_after_soft_violation);
    w.u32(static_cast<std::uint32_t>(p.tenant_class.size()));
    for (auto& kv : p.tenant_class) { w.id(kv.first); write_enum(w, kv.second); }
    w.u32(static_cast<std::uint32_t>(p.tenant_fairness_weight.size()));
    for (auto& kv : p.tenant_fairness_weight) { w.id(kv.first); write_double(w, kv.second); }
    w.u32(static_cast<std::uint32_t>(p.model_class.size()));
    for (auto& kv : p.model_class) { w.id(kv.first); write_enum(w, kv.second); }
    w.u32(static_cast<std::uint32_t>(p.model_budget_scale.size()));
    for (auto& kv : p.model_budget_scale) { w.id(kv.first); write_double(w, kv.second); }
}

bool decode_policy(BinaryReader& r, Policy& p) {
    if (!r.u64(p.id) || !r.u64(p.generation)) return false;
    if (!r.string(p.name)) return false;
    std::uint32_t n; if (!r.u32(n)) return false;
    if (n > 1024) return false;
    p.classes.clear();
    for (std::uint32_t i = 0; i < n; ++i) {
        SloClassSpec c;
        if (!r.string(c.name) || !read_double(r, c.priority)) return false;
        if (!r.duration(c.default_e2e_target) || !r.duration(c.default_ttf_target)) return false;
        if (!read_double(r, c.default_deadline_risk_threshold) || !read_double(r, c.default_min_completion_probability)) return false;
        if (!read_double(r, c.default_fairness_weight) || !r.bool_(c.default_degradation_allowed)) return false;
        for (auto& x : c.phase_weights) if (!read_double(r, x)) return false;
        if (!read_double(r, c.reserve_fraction)) return false;
        p.classes.push_back(c);
    }
    if (!r.u32(p.retry.max_retries) || !r.duration(p.retry.max_cumulative_retry_delay)) return false;
    if (!read_double(r, p.retry.backoff_base_ms) || !read_double(r, p.retry.backoff_factor)) return false;
    if (!r.bool_(p.retry.allow_immediate_retry)) return false;
    if (!read_double(r, p.transfer.max_transfer_fraction_of_budget) || !r.bool_(p.transfer.prefer_local_when_risky)) return false;
    if (!r.u32(p.speculation.default_max_depth) || !read_double(r, p.speculation.min_acceptance_rate)) return false;
    if (!read_double(r, p.speculation.max_overhead_fraction_of_budget)) return false;
    if (!r.u32(p.fairness.max_starvation_ratio) || !read_double(r, p.fairness.background_min_service_fraction)) return false;
    if (!r.bool_(p.fairness.protect_lower_classes_strongly)) return false;
    if (!r.duration(p.batch.default_max_batch_wait) || !read_double(r, p.batch.deadline_spread_ratio)) return false;
    if (!read_double(r, p.batch.min_batch_efficiency)) return false;
    if (!read_double(r, p.resource_pressure_sensitivity) || !r.u64(p.prediction_min_evidence)) return false;
    if (!r.bool_(p.fail_fast_on_deadline_exceeded) || !r.bool_(p.allow_completion_after_soft_violation)) return false;
    if (!r.u32(n)) return false; if (n > 4096) return false;
    p.tenant_class.clear();
    for (std::uint32_t i = 0; i < n; ++i) { TenantId id; SloClass sc; if (!r.id(id) || !read_enum(r, sc)) return false; p.tenant_class[id] = sc; }
    if (!r.u32(n)) return false; if (n > 4096) return false;
    p.tenant_fairness_weight.clear();
    for (std::uint32_t i = 0; i < n; ++i) { TenantId id; double d; if (!r.id(id) || !read_double(r, d)) return false; p.tenant_fairness_weight[id] = d; }
    if (!r.u32(n)) return false; if (n > 4096) return false;
    p.model_class.clear();
    for (std::uint32_t i = 0; i < n; ++i) { ModelId id; SloClass sc; if (!r.id(id) || !read_enum(r, sc)) return false; p.model_class[id] = sc; }
    if (!r.u32(n)) return false; if (n > 4096) return false;
    p.model_budget_scale.clear();
    for (std::uint32_t i = 0; i < n; ++i) { ModelId id; double d; if (!r.id(id) || !read_double(r, d)) return false; p.model_budget_scale[id] = d; }
    return true;
}

void encode_phase_budgets(BinaryWriter& w, const PhaseBudgets& pb) {
    const auto& part = pb.partition();
    for (std::size_t i = 0; i < kPhaseCount; ++i) w.duration(part.allocation[i]);
    w.duration(part.reserve);
    for (std::size_t i = 0; i < kPhaseCount; ++i) w.duration(pb.consumed(static_cast<Phase>(i)));
}
bool decode_phase_budgets(BinaryReader& r, PhaseBudgets& pb) {
    BudgetPartition part;
    for (std::size_t i = 0; i < kPhaseCount; ++i) { if (!r.duration(part.allocation[i])) return false; }
    if (!r.duration(part.reserve)) return false;
    pb.set_partition(part);
    pb.reset_consumed();
    for (std::size_t i = 0; i < kPhaseCount; ++i) {
        Duration d; if (!r.duration(d)) return false;
        if (d < Duration::zero()) return false;
        pb.attribute(static_cast<Phase>(i), d);
    }
    return true;
}

void encode_request(BinaryWriter& w, const RequestState& rs) {
    encode_descriptor(w, rs.descriptor);
    w.id(rs.attempt_id);
    w.bool_(rs.dispatch_id.has_value()); if (rs.dispatch_id) w.id(*rs.dispatch_id);
    w.id(rs.generation); w.id(rs.epoch);
    write_enum(w, rs.lifecycle); write_enum(w, rs.current_phase);
    w.duration(rs.elapsed_total); w.duration(rs.queue_residence); w.duration(rs.batch_wait); w.duration(rs.current_phase_elapsed);
    w.bool_(rs.worker_id.has_value()); if (rs.worker_id) w.id(*rs.worker_id);
    w.bool_(rs.worker_boot_id.has_value()); if (rs.worker_boot_id) w.id(*rs.worker_boot_id);
    w.bool_(rs.backend_id.has_value()); if (rs.backend_id) w.id(*rs.backend_id);
    write_enum(w, rs.device);
    w.u32(rs.spec_depth); w.u32(rs.spec_branches); w.bool_(rs.speculation_enabled);
    write_double(w, rs.spec_acceptance_rate); w.bool_(rs.speculation_was_reduced);
    w.u32(rs.retry_count); w.duration(rs.retry_accumulated);
    write_enum(w, rs.risk); write_double(w, rs.completion_probability);
    w.duration(rs.service_consumed); w.u32(rs.interventions_received); w.duration(rs.starvation_age); w.u32(rs.active_priority);
    w.u32(static_cast<std::uint32_t>(rs.reservations.size()));
    for (const auto& res : rs.reservations) { w.id(res.id); w.id(res.backend); w.u64(res.amount); w.bool_(res.released); }
    w.u32(static_cast<std::uint32_t>(rs.interventions.size()));
    for (const auto& it : rs.interventions) {
        write_enum(w, it.action); write_enum(w, it.reason);
        w.duration(it.remaining_budget); w.duration(it.predicted_remaining);
        write_enum(w, it.risk_before); w.u64(it.policy_generation); w.u64(it.decision_generation);
    }
    w.bool_(rs.terminal_reason.has_value()); if (rs.terminal_reason) write_enum(w, *rs.terminal_reason);
    w.bool_(rs.slo_met); w.bool_(rs.soft_violation); w.bool_(rs.hard_violation);
    encode_phase_budgets(w, rs.phase_budgets);
}
bool decode_request(BinaryReader& r, RequestState& rs) {
    if (!decode_descriptor(r, rs.descriptor)) return false;
    rs.request_id = rs.descriptor.request_id;
    if (!r.id(rs.attempt_id)) return false;
    bool has; if (!r.bool_(has)) return false; rs.dispatch_id.reset();
    if (has) { DispatchId d; if (!r.id(d)) return false; rs.dispatch_id = d; }
    if (!r.id(rs.generation) || !r.id(rs.epoch)) return false;
    if (!read_enum(r, rs.lifecycle) || !read_enum(r, rs.current_phase)) return false;
    if (!r.duration(rs.elapsed_total) || !r.duration(rs.queue_residence) || !r.duration(rs.batch_wait) || !r.duration(rs.current_phase_elapsed)) return false;
    if (!r.bool_(has)) return false; rs.worker_id.reset(); if (has) { WorkerId w; if (!r.id(w)) return false; rs.worker_id = w; }
    if (!r.bool_(has)) return false; rs.worker_boot_id.reset(); if (has) { WorkerBootId w; if (!r.id(w)) return false; rs.worker_boot_id = w; }
    if (!r.bool_(has)) return false; rs.backend_id.reset(); if (has) { BackendId b; if (!r.id(b)) return false; rs.backend_id = b; }
    if (!read_enum(r, rs.device)) return false;
    if (!r.u32(rs.spec_depth) || !r.u32(rs.spec_branches) || !r.bool_(rs.speculation_enabled)) return false;
    if (!read_double(r, rs.spec_acceptance_rate) || !r.bool_(rs.speculation_was_reduced)) return false;
    if (!r.u32(rs.retry_count) || !r.duration(rs.retry_accumulated)) return false;
    if (!read_enum(r, rs.risk) || !read_double(r, rs.completion_probability)) return false;
    if (!r.duration(rs.service_consumed) || !r.u32(rs.interventions_received) || !r.duration(rs.starvation_age) || !r.u32(rs.active_priority)) return false;
    std::uint32_t n; if (!r.u32(n)) return false; if (n > 1024) return false;
    rs.reservations.clear();
    for (std::uint32_t i = 0; i < n; ++i) { Reservation res; if (!r.id(res.id) || !r.id(res.backend) || !r.u64(res.amount) || !r.bool_(res.released)) return false; rs.reservations.push_back(res); }
    if (!r.u32(n)) return false; if (n > 1024) return false;
    rs.interventions.clear();
    for (std::uint32_t i = 0; i < n; ++i) { InterventionRecord it; if (!read_enum(r, it.action) || !read_enum(r, it.reason)) return false; if (!r.duration(it.remaining_budget) || !r.duration(it.predicted_remaining)) return false; if (!read_enum(r, it.risk_before) || !r.u64(it.policy_generation) || !r.u64(it.decision_generation)) return false; rs.interventions.push_back(it); }
    if (!r.bool_(has)) return false; rs.terminal_reason.reset(); if (has) { RejectionCode code; if (!read_enum(r, code)) return false; rs.terminal_reason = code; }
    if (!r.bool_(rs.slo_met) || !r.bool_(rs.soft_violation) || !r.bool_(rs.hard_violation)) return false;
    if (!decode_phase_budgets(r, rs.phase_budgets)) return false;
    return true;
}

void encode_predictor(BinaryWriter& w, const Predictor& pred) {
    const auto& cfg = pred.config();
    w.u64(cfg.max_window); w.u64(cfg.max_keys); w.u64(cfg.high_confidence_evidence); w.u64(cfg.medium_confidence_evidence); write_double(w, cfg.interval_sigma);
    w.u64(pred.generation());
    const auto& keys = pred.keys();
    w.u32(static_cast<std::uint32_t>(keys.size()));
    for (const auto& key : keys) {
        w.string(key);
        const auto smp = pred.samples(key);
        w.u32(static_cast<std::uint32_t>(smp.size()));
        for (const auto& s : smp) w.duration(s);
    }
}
bool decode_predictor(BinaryReader& r, Predictor& pred) {
    PredictorConfig cfg;
    if (!r.u64(cfg.max_window) || !r.u64(cfg.max_keys) || !r.u64(cfg.high_confidence_evidence) || !r.u64(cfg.medium_confidence_evidence)) return false;
    if (!read_double(r, cfg.interval_sigma)) return false;
    if (cfg.max_window == 0 || cfg.max_keys == 0) return false;
    std::uint64_t gen; if (!r.u64(gen)) return false;
    pred.set_config(cfg);
    pred.set_generation(gen);
    std::uint32_t n; if (!r.u32(n)) return false; if (n > cfg.max_keys) return false;
    for (std::uint32_t i = 0; i < n; ++i) {
        std::string key; if (!r.string(key)) return false;
        std::uint32_t m; if (!r.u32(m)) return false; if (m > cfg.max_window) return false;
        for (std::uint32_t j = 0; j < m; ++j) { Duration d; if (!r.duration(d)) return false; if (d < Duration::zero()) return false; pred.record(key, d); }
    }
    return true;
}

void encode_worker(BinaryWriter& w, const WorkerDescriptor& wd) {
    w.id(wd.id); w.id(wd.boot_id); w.string(wd.host); w.u16(wd.port);
    write_enum(w, wd.capabilities.device); w.bool_(wd.capabilities.accelerator);
    w.string(wd.capabilities.backend_name); w.string(wd.capabilities.backend_version);
    w.u64(wd.capabilities.total_memory); w.u64(wd.capabilities.free_memory);
    w.u32(static_cast<std::uint32_t>(wd.capabilities.supported_ops.size()));
    for (const auto& op : wd.capabilities.supported_ops) w.string(op);
    w.u32(wd.protocol_version); w.u32(wd.generation);
}
bool decode_worker(BinaryReader& r, WorkerDescriptor& wd) {
    if (!r.id(wd.id) || !r.id(wd.boot_id) || !r.string(wd.host)) return false;
    if (!r.u16(wd.port)) return false;
    if (!read_enum(r, wd.capabilities.device)) return false;
    if (!r.bool_(wd.capabilities.accelerator)) return false;
    if (!r.string(wd.capabilities.backend_name) || !r.string(wd.capabilities.backend_version)) return false;
    if (!r.u64(wd.capabilities.total_memory) || !r.u64(wd.capabilities.free_memory)) return false;
    std::uint32_t n; if (!r.u32(n)) return false; if (n > 4096) return false;
    wd.capabilities.supported_ops.clear();
    for (std::uint32_t i = 0; i < n; ++i) { std::string op; if (!r.string(op)) return false; wd.capabilities.supported_ops.push_back(op); }
    if (!r.u32(wd.protocol_version) || !r.u32(wd.generation)) return false;
    return true;
}

void encode_metrics(BinaryWriter& w, const MetricsSummary& m) {
    w.u64(m.active); w.u64(m.completed); w.u64(m.failed); w.u64(m.cancelled);
    w.u64(m.slo_met); w.u64(m.soft_violation); w.u64(m.hard_violation);
    for (auto x : m.per_class) w.u64(x);
    for (auto x : m.interventions) w.u64(x);
    for (auto x : m.admissions) w.u64(x);
    for (auto x : m.stale_rejections) w.u64(x);
    w.u64(m.observations_received); w.u64(m.observations_rejected); w.u64(m.predictors_updated);
    w.u64(m.prediction_error_count); write_double(w, m.prediction_error_mean_us); write_double(w, m.prediction_error_var_us);
    w.u64(m.decisions);
}
bool decode_metrics(BinaryReader& r, MetricsSummary& m) {
    if (!r.u64(m.active) || !r.u64(m.completed) || !r.u64(m.failed) || !r.u64(m.cancelled)) return false;
    if (!r.u64(m.slo_met) || !r.u64(m.soft_violation) || !r.u64(m.hard_violation)) return false;
    for (auto& x : m.per_class) { if (!r.u64(x)) return false; }
    for (auto& x : m.interventions) { if (!r.u64(x)) return false; }
    for (auto& x : m.admissions) { if (!r.u64(x)) return false; }
    for (auto& x : m.stale_rejections) { if (!r.u64(x)) return false; }
    if (!r.u64(m.observations_received) || !r.u64(m.observations_rejected) || !r.u64(m.predictors_updated)) return false;
    if (!r.u64(m.prediction_error_count) || !read_double(r, m.prediction_error_mean_us) || !read_double(r, m.prediction_error_var_us)) return false;
    if (!r.u64(m.decisions)) return false;
    return true;
}

} // namespace

std::string Governor::encode_state(std::string& error) const {
    std::lock_guard g(impl_->mutex);
    BinaryWriter body;
    body.u64(impl_->epoch.value());
    body.u64(impl_->decision_generation);
    body.u64(impl_->next_attempt); body.u64(impl_->next_generation);
    body.u64(impl_->next_observation); body.u64(impl_->next_reservation);
    const auto pols = impl_->policy_store.all();
    body.u32(static_cast<std::uint32_t>(pols.size()));
    for (const auto* p : pols) encode_policy(body, *p);
    encode_predictor(body, impl_->predictor);
    body.u32(static_cast<std::uint32_t>(impl_->workers.size()));
    for (const auto& [id, wd] : impl_->workers) { (void)id; encode_worker(body, wd); }
    encode_metrics(body, impl_->metrics);
    std::uint32_t active = 0;
    for (const auto& [id, rs] : impl_->requests) if (rs.is_active()) ++active;
    body.u32(active);
    for (const auto& [id, rs] : impl_->requests) if (rs.is_active()) encode_request(body, rs);
    const std::string& payload = body.data();
    BinaryWriter w;
    w.u64(kPersistenceMagic);
    w.u32(kPersistenceVersion);
    w.u32(static_cast<std::uint32_t>(payload.size()));
    w.bytes(payload.data(), payload.size());
    w.u64(fnv1a(payload));
    error.clear();
    return w.data();
}

bool Governor::decode_state(std::string_view blob, std::string& error) {
    BinaryReader r(blob);
    std::uint64_t magic; std::uint32_t version, len;
    if (!r.u64(magic)) { error = "truncated header"; return false; }
    if (magic != kPersistenceMagic) { error = "bad magic"; return false; }
    if (!r.u32(version)) { error = "truncated version"; return false; }
    if (version != kPersistenceVersion) { error = "unsupported format version"; return false; }
    if (!r.u32(len)) { error = "truncated length"; return false; }
    if (len > kMaxFrameSize) { error = "record too large"; return false; }
    std::string_view payload; if (!r.take(len, payload)) { error = "truncated payload"; return false; }
    std::uint64_t checksum; if (!r.u64(checksum)) { error = "missing checksum"; return false; }
    if (!r.at_end()) { error = "trailing data"; return false; }
    if (fnv1a(payload) != checksum) { error = "checksum mismatch"; return false; }

    BinaryReader pr(payload);
    std::uint64_t epochv, dgen, na, ng, no, nr;
    if (!pr.u64(epochv) || !pr.u64(dgen) || !pr.u64(na) || !pr.u64(ng) || !pr.u64(no) || !pr.u64(nr)) { error = "truncated counters"; return false; }

    std::map<RequestId, RequestState> new_requests;
    std::map<WorkerId, WorkerDescriptor> new_workers;
    std::vector<Policy> new_policies;

    std::uint32_t npol; if (!pr.u32(npol)) { error = "truncated policy count"; return false; }
    if (npol > 1024) { error = "policy count out of range"; return false; }
    for (std::uint32_t i = 0; i < npol; ++i) { Policy p; if (!decode_policy(pr, p)) { error = "malformed policy"; return false; } new_policies.push_back(p); }

    Predictor temp_pred;
    if (!decode_predictor(pr, temp_pred)) { error = "malformed predictor"; return false; }

    std::uint32_t nw; if (!pr.u32(nw)) { error = "truncated worker count"; return false; }
    if (nw > 8192) { error = "worker count out of range"; return false; }
    for (std::uint32_t i = 0; i < nw; ++i) { WorkerDescriptor wd; if (!decode_worker(pr, wd)) { error = "malformed worker"; return false; } new_workers[wd.id] = wd; }

    MetricsSummary m; if (!decode_metrics(pr, m)) { error = "malformed metrics"; return false; }

    std::uint32_t nreq; if (!pr.u32(nreq)) { error = "truncated request count"; return false; }
    if (nreq > 1000000) { error = "request count out of range"; return false; }
    for (std::uint32_t i = 0; i < nreq; ++i) {
        RequestState rs;
        if (!decode_request(pr, rs)) { error = "malformed request"; return false; }
        if (!rs.request_id.is_valid()) { error = "invalid request id"; return false; }
        if (!rs.is_active()) { error = "inactive request persisted"; return false; }
        if (new_requests.count(rs.request_id)) { error = "duplicate request id"; return false; }
        new_requests[rs.request_id] = std::move(rs);
    }
    if (!pr.at_end()) { error = "trailing data in payload"; return false; }

    {
        std::lock_guard g(impl_->mutex);
        impl_->requests = std::move(new_requests);
        impl_->workers = std::move(new_workers);
        impl_->metrics = m;
        impl_->metrics.active = 0;
        for (const auto& [id, rs] : impl_->requests) if (rs.is_active()) ++impl_->metrics.active;
        impl_->epoch = CoordinatorEpoch(epochv);
        impl_->decision_generation = dgen;
        impl_->next_attempt = na; impl_->next_generation = ng;
        impl_->next_observation = no; impl_->next_reservation = nr;
        impl_->policy_store = PolicyStore();
        for (const auto& p : new_policies) { std::string perr; impl_->policy_store.add(p, perr); }
        impl_->predictor = std::move(temp_pred);
        impl_->fairness = FairnessTracker(impl_->config.max_tenant);
        impl_->completion_order.clear();
        impl_->events.clear();
        impl_->event_sequence = impl_->next_observation;
        impl_->push_event("recovery", "loaded state");
    }
    error.clear();
    return true;
}

void Governor::fail_requests_for_worker(WorkerId wid, WorkerBootId bid) {
    std::lock_guard g(impl_->mutex);
    const TimePoint now = clock_.now();
    for (auto& [id, rs] : impl_->requests) {
        if (!rs.is_active()) continue;
        if (rs.worker_id && *rs.worker_id == wid && rs.worker_boot_id && *rs.worker_boot_id == bid) {
            rs.elapsed_total = sat_add(rs.elapsed_total, saturating_elapsed(rs.last_update, now));
            rs.last_update = now;
            terminalize(rs, LifecycleState::FAILED, impl_->metrics, RejectionCode::BACKEND_UNAVAILABLE, now);
            for (auto& res : rs.reservations) res.released = true;
            rs.generation = Generation(rs.generation.value() + 1);
            rs.worker_id.reset(); rs.worker_boot_id.reset(); rs.dispatch_id.reset();
            impl_->push_event("worker_loss_failed", id.to_string());
        }
    }
}

} // namespace latency_governor
