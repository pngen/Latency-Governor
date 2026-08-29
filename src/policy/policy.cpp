#include "latency_governor/policy.hpp"

#include <algorithm>
#include <string>

namespace latency_governor {

namespace {
constexpr std::size_t kMaxPolicyClasses = 64;
constexpr std::int64_t kMinTargetNs = 1; // 1 ns
} // namespace

bool Policy::class_enabled(const std::string& cls_name) const {
    return class_spec(cls_name) != nullptr;
}

const SloClassSpec* Policy::class_spec(const std::string& cls_name) const {
    for (const auto& c : classes) {
        if (c.name == cls_name) return &c;
    }
    return nullptr;
}

const SloClassSpec* Policy::class_spec(SloClass c) const {
    const char* nm = to_string(c);
    return class_spec(nm);
}

bool Policy::validate(std::string& error) const {
    if (classes.empty()) { error = "policy must define at least one SLO class"; return false; }
    if (classes.size() > kMaxPolicyClasses) { error = "too many SLO classes"; return false; }
    // Unique names.
    for (std::size_t i = 0; i < classes.size(); ++i) {
        const auto& a = classes[i];
        if (a.name.empty()) { error = "SLO class name is empty"; return false; }
        for (std::size_t j = i + 1; j < classes.size(); ++j) {
            if (classes[j].name == a.name) { error = "duplicate SLO class name: " + a.name; return false; }
        }
        if (a.priority < 0.0) { error = "priority must be non-negative"; return false; }
        if (ns_count(a.default_e2e_target) < kMinTargetNs) { error = "default_e2e_target must be positive"; return false; }
        if (a.default_ttf_target < Duration::zero()) { error = "default_ttf_target must be non-negative"; return false; }
        for (double w : a.phase_weights) {
            if (w < 0.0) { error = "phase weight must be non-negative"; return false; }
        }
        if (a.reserve_fraction < 0.0 || a.reserve_fraction >= 1.0) {
            error = "reserve_fraction must be in [0,1)";
            return false;
        }
    }
    if (resource_pressure_sensitivity < 0.0 || resource_pressure_sensitivity > 1.0) {
        error = "resource_pressure_sensitivity must be in [0,1]";
        return false;
    }
    if (fairness.max_starvation_ratio == 0) { error = "max_starvation_ratio must be positive"; return false; }
    if (transfer.max_transfer_fraction_of_budget > 1.0) {
        error = "max_transfer_fraction_of_budget must be <= 1.0";
        return false;
    }
    if (speculation.max_overhead_fraction_of_budget > 1.0) {
        error = "max_spec_overhead_fraction must be <= 1.0";
        return false;
    }
    if (batch.deadline_spread_ratio <= 0.0 || batch.deadline_spread_ratio > 1.0) {
        error = "deadline_spread_ratio must be in (0,1]";
        return false;
    }
    return true;
}

std::optional<std::uint64_t> PolicyStore::add(Policy p, std::string& error) {
    if (!p.validate(error)) return std::nullopt;
    p.id = next_generation_;
    p.generation = next_generation_;
    ++next_generation_;
    // Bound the retained history.
    if (policies_.size() >= max_retained_) {
        // Drop oldest policy (lowest generation).
        auto it = by_generation_.begin();
        if (it != by_generation_.end()) {
            by_generation_.erase(it);
            policies_.erase(policies_.begin());
        }
    }
    const std::size_t idx = policies_.size();
    policies_.push_back(std::move(p));
    by_generation_[policies_.back().generation] = idx;
    return policies_.back().generation;
}

std::optional<std::uint64_t> PolicyStore::current_generation() const noexcept {
    if (by_generation_.empty()) return std::nullopt;
    return by_generation_.rbegin()->first;
}

const Policy* PolicyStore::get(std::uint64_t generation) const noexcept {
    auto it = by_generation_.find(generation);
    if (it == by_generation_.end()) return nullptr;
    return &policies_[it->second];
}

const Policy* PolicyStore::current() const noexcept {
    auto g = current_generation();
    if (!g) return nullptr;
    return get(*g);
}

std::vector<const Policy*> PolicyStore::all() const {
    std::vector<const Policy*> out;
    for (const auto& [gen, idx] : by_generation_) out.push_back(&policies_[idx]);
    return out;
}

void PolicyStore::compact(std::string& error) {
    auto cur = current_generation();
    if (!cur) { error = "no policy to compact"; return; }
    // Retain only the newest policy. This deliberately does not reinterpret
    // historical decisions: the retained generation keeps its identity, and the
    // governor stores the policy generation per request.
    const Policy keep = *get(*cur);
    policies_.clear();
    by_generation_.clear();
    policies_.push_back(keep);
    by_generation_[policies_.back().generation] = 0;
    error.clear();
}

} // namespace latency_governor