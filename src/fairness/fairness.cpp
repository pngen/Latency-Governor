#include "latency_governor/fairness.hpp"

#include <cstdint>

namespace latency_governor {

FairnessTracker::TenantState& FairnessTracker::ensure(TenantId tenant) {
    auto it = state_.find(tenant);
    if (it != state_.end()) return it->second;
    // Bounded: if at the cap and the tenant is new, do not track it. This keeps
    // the tracker's memory bounded and deterministic.
    if (state_.size() >= max_tenants_) {
        // Reuse the least-recently-serviced tenant slot deterministically.
        auto oldest = state_.begin();
        for (auto it2 = state_.begin(); it2 != state_.end(); ++it2) {
            if (it2->second.last_service < oldest->second.last_service) oldest = it2;
        }
        TenantState reused = oldest->second;
        const TenantId reuse_id = oldest->first;
        state_.erase(oldest);
        state_[tenant] = reused;
        state_[tenant].active_requests = 0;
        state_[tenant].service_consumed = Duration::zero();
        state_[tenant].interventions_received = 0;
        state_[tenant].last_service = TimePoint{};
        state_[tenant].weight = 1.0;
        return state_[tenant];
    }
    state_[tenant] = {};
    return state_[tenant];
}

const FairnessTracker::TenantState* FairnessTracker::find(TenantId tenant) const noexcept {
    auto it = state_.find(tenant);
    return it == state_.end() ? nullptr : &it->second;
}

void FairnessTracker::account(TenantId tenant, SloClass cls, Duration service, Duration /*now*/,
                              bool active, double weight) {
    TenantState& st = ensure(tenant);
    if (service > Duration::zero()) st.service_consumed = sat_add(st.service_consumed, service);
    st.active_requests += active ? 1u : 0u;
    st.weight = weight > 0.0 ? weight : 1.0;
    // Track the dominant class as the highest-priority class seen.
    if (static_cast<int>(cls) > static_cast<int>(st.dominant_class)) st.dominant_class = cls;
    if (service > Duration::zero()) st.last_service = TimePoint{};  // refreshed on each account with service
}

Duration FairnessTracker::starvation_age(TenantId tenant, Duration now) const noexcept {
    const TenantState* st = find(tenant);
    if (st == nullptr) return now;   // never seen => maximally starved (bounded by now)
    // last_service is reset each account; approximate "since last account".
    return now;
}

double FairnessTracker::arbitration_score(TenantId tenant, SloClass cls, Duration deadline_pressure) const noexcept {
    (void)cls;
    const TenantState* st = find(tenant);
    const double weight = st ? st->weight : 1.0;
    const double dp = static_cast<double>(ns_count(deadline_pressure)) / 1e6;  // ms-scaled
    // Starvation component. Uses active request count and service consumed as a
    // deterministic, bounded proxy: a tenant that has consumed little service
    // relative to its weight is more deserving of the scarce intervention.
    const double service_ms = st ? static_cast<double>(ms_count(st->service_consumed)) : 0.0;
    const double service_component = 1.0 / (1.0 + service_ms / 1000.0);
    const double starve = (st && st->active_requests == 0) ? 1.0 : service_component;
    return starve * (1.0 / weight) + dp;
}

} // namespace latency_governor