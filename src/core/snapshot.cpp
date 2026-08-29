#include "latency_governor/snapshot.hpp"

#include <cmath>

namespace latency_governor {

void Histogram::record(Duration d) noexcept {
    if (d < Duration::zero()) return;
    const auto ns = static_cast<double>(ns_count(d));
    if (ns <= 0.0) { counts_[0]++; total_++; return; }
    // Log2 bucket.
    const int idx = static_cast<int>(std::floor(std::log2(ns)));
    const auto bucket = std::min<std::size_t>(std::max(0, idx), kBuckets - 1);
    counts_[bucket]++;
    total_++;
    if (d > max_value_) max_value_ = d;
}

} // namespace latency_governor
