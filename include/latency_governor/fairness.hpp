#pragma once

#include "latency_governor/clock.hpp"
#include "latency_governor/duration.hpp"
#include "latency_governor/enums.hpp"
#include "latency_governor/identifiers.hpp"
#include "latency_governor/policy.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace latency_governor {

// Bounded multi-tenant fairness. Latency SLO enforcement must not become
// "always privilege whoever has the shortest deadline"; background traffic must
// not be starved indefinitely unless policy explicitly permits it.
class FairnessTracker {
public:
    explicit FairnessTracker(std::size_t max_tenants = 1024) : max_tenants_(max_tenants) {}

    // Update a tenant's service accounting on each observation.
    void account(TenantId tenant, SloClass cls, Duration service, Duration now,
                 bool active, double weight);

    // Staleness / starvation age for a tenant (time since last service).
    [[nodiscard]] Duration starvation_age(TenantId tenant, Duration now) const noexcept;

    // Deterministic arbitration score for a tenant (higher = more starvation).
    // Used to break ties when multiple at-risk requests compete.
    [[nodiscard]] double arbitration_score(TenantId tenant, SloClass cls,
                                           Duration deadline_pressure) const noexcept;

    [[nodiscard]] std::size_t tenant_count() const noexcept { return state_.size(); }
    [[nodiscard]] std::size_t max_tenants() const noexcept { return max_tenants_; }
    void set_max_tenants(std::size_t m) noexcept { max_tenants_ = m == 0 ? 1 : m; }

private:
    struct TenantState {
        SloClass dominant_class = SloClass::STANDARD;
        Duration service_consumed{0};
        std::uint32_t interventions_received = 0;
        std::uint32_t active_requests = 0;
        TimePoint last_service{};
        double weight = 1.0;
    };

    TenantState& ensure(TenantId tenant);
    const TenantState* find(TenantId tenant) const noexcept;

    std::size_t max_tenants_;
    std::map<TenantId, TenantState> state_;   // bounded
};

} // namespace latency_governor
