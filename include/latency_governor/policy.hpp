#pragma once

#include "latency_governor/clock.hpp"
#include "latency_governor/duration.hpp"
#include "latency_governor/enums.hpp"
#include "latency_governor/identifiers.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace latency_governor {

// Per-SLO-class characteristics. These are defaults over a generic policy
// representation; a policy may define any class, not only the built-in names.
struct SloClassSpec {
    std::string name;
    double priority = 0.0;
    Duration default_e2e_target{0};
    Duration default_ttf_target{0};
    double default_deadline_risk_threshold = 0.10;
    double default_min_completion_probability = 0.0;
    double default_fairness_weight = 1.0;
    bool default_degradation_allowed = true;

    // Fraction of the e2e target allocated to each phase.
    std::array<double, Phase_count> phase_weights{};
    double reserve_fraction = 0.05;
};

struct RetryPolicy {
    std::uint32_t max_retries = 0;
    Duration max_cumulative_retry_delay{0};
    double backoff_base_ms = 10.0;
    double backoff_factor = 2.0;
    bool allow_immediate_retry = false;
};

struct TransferPolicy {
    double max_transfer_fraction_of_budget = 0.20;
    bool prefer_local_when_risky = true;
};

struct SpeculationPolicy {
    std::uint32_t default_max_depth = 2;
    double min_acceptance_rate = 0.10;
    double max_overhead_fraction_of_budget = 0.10;
};

struct FairnessPolicy {
    unsigned max_starvation_ratio = 2;
    double background_min_service_fraction = 0.05;
    bool protect_lower_classes_strongly = true;
};

struct BatchPolicy {
    Duration default_max_batch_wait{0};
    double deadline_spread_ratio = 0.5;
    double min_batch_efficiency = 0.5;
};

// A validated, versioned, inspectable, deterministic, atomically-replaceable
// policy. A request admitted under one generation retains knowledge of that
// generation; live replacement never reinterprets those historical decisions.
struct Policy {
    std::uint64_t id = 0;
    std::uint64_t generation = 0;
    std::string name;

    std::vector<SloClassSpec> classes;

    RetryPolicy retry;
    TransferPolicy transfer;
    SpeculationPolicy speculation;
    FairnessPolicy fairness;
    BatchPolicy batch;

    double resource_pressure_sensitivity = 0.5;
    std::uint64_t prediction_min_evidence = 4;

    // Fail-fast conditions.
    bool fail_fast_on_deadline_exceeded = true;
    bool allow_completion_after_soft_violation = true;

    // Tenant/model overrides (bounded).
    std::map<TenantId, SloClass> tenant_class;
    std::map<TenantId, double> tenant_fairness_weight;
    std::map<ModelId, SloClass> model_class;
    std::map<ModelId, double> model_budget_scale;

    [[nodiscard]] bool class_enabled(const std::string& name) const;
    [[nodiscard]] const SloClassSpec* class_spec(const std::string& name) const;
    [[nodiscard]] const SloClassSpec* class_spec(SloClass c) const;

    [[nodiscard]] bool validate(std::string& error) const;
};

// PolicyStore is a bounded, versioned store of policies. The most recent
// (highest generation) valid policy is authoritative. Older generations remain
// queryable so historical decisions are never reinterpreted by mutation.
class PolicyStore {
public:
    std::optional<std::uint64_t> add(Policy p, std::string& error);
    [[nodiscard]] std::optional<std::uint64_t> current_generation() const noexcept;
    [[nodiscard]] const Policy* get(std::uint64_t generation) const noexcept;
    [[nodiscard]] const Policy* current() const noexcept;

    [[nodiscard]] std::size_t count() const noexcept { return by_generation_.size(); }
    // All retained policies in generation order (for persistence). Bounded.
    [[nodiscard]] std::vector<const Policy*> all() const;
    [[nodiscard]] std::size_t max_retained() const noexcept { return max_retained_; }
    void set_max_retained(std::size_t m) noexcept { max_retained_ = m == 0 ? 1 : m; }

    void compact(std::string& error);

private:
    std::vector<Policy> policies_;
    std::map<std::uint64_t, std::size_t> by_generation_;
    std::size_t max_retained_ = 32;
    std::uint64_t next_generation_ = 1;
};

} // namespace latency_governor