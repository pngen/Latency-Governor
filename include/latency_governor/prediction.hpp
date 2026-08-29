#pragma once

#include "latency_governor/clock.hpp"
#include "latency_governor/duration.hpp"
#include "latency_governor/enums.hpp"

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace latency_governor {

// A latency prediction. Predictions are intentionally bounded and explicit
// about their uncertainty and provenance. No prediction is ever presented as
// exact.
struct Prediction {
    Duration predicted{0};              // point estimate of remaining latency
    Duration lower{0};                  // lower bound of the confidence interval
    Duration upper{0};                  // upper bound of the confidence interval
    ConfidenceClass confidence = ConfidenceClass::NONE;
    PredictionSource source = PredictionSource::FALLBACK;
    std::size_t evidence_count = 0;     // number of samples used
    std::uint64_t predictor_generation = 0;  // predictor version at compute time
    std::uint64_t version = 0;          // per-key predictor version

    bool measured() const noexcept { return source == PredictionSource::MEASURED; }
};

// Configuration for the bounded predictor.
struct PredictorConfig {
    std::size_t max_window = 64;            // max samples retained per key
    std::size_t max_keys = 256;             // max distinct metrics tracked
    std::uint64_t high_confidence_evidence = 16;
    std::uint64_t medium_confidence_evidence = 4;
    double interval_sigma = 2.0;            // confidence interval = mean ± sigma * stddev
};

// A bounded, monotonic predictor of per-key latency. It maintains an
// exponentially-weighted statistics window per key, capped in both the number
// of keys and samples per key. Cold start is explicit: a key with no evidence
// yields no prediction, and callers fall back per deterministic policy.
class Predictor {
public:
    explicit Predictor(PredictorConfig cfg = {});
    Predictor(const Predictor&) = delete;
    Predictor& operator=(const Predictor&) = delete;
    Predictor(Predictor&&) noexcept = default;
    Predictor& operator=(Predictor&&) noexcept = default;

    // Record a single latency sample for a metric key.
    void record(std::string_view key, Duration sample) noexcept;

    // Predict remaining latency for a metric key. Returns nullopt when there is
    // insufficient evidence (cold start).
    [[nodiscard]] std::optional<Prediction> predict(std::string_view key) const noexcept;

    [[nodiscard]] std::size_t evidence_count(std::string_view key) const noexcept;
    // A copy of the bounded sample window for a key (for persistence/export).
    [[nodiscard]] std::vector<Duration> samples(std::string_view key) const;
    [[nodiscard]] const std::vector<std::string>& keys() const noexcept { return keys_; }
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
    [[nodiscard]] const PredictorConfig& config() const noexcept { return config_; }

    // Advance the predictor generation after a config change or reset.
    void bump_generation() noexcept { ++generation_; }

    // Replace the config (clears all metrics) and optionally set the generation.
    void set_config(const PredictorConfig& cfg) noexcept;
    void set_generation(std::uint64_t g) noexcept { generation_ = g; }

    // Reset to empty (cold) state.
    void reset() noexcept;

private:
    struct Stat {
        std::deque<Duration> samples;
        std::uint64_t version = 0;
        double mean_us = 0.0;
        double var_us = 0.0;   // EWMA variance (in us^2)
        std::size_t count = 0;
    };

    const Stat* find(std::string_view key) const noexcept;
    Stat* find_or_create(std::string_view key);

    PredictorConfig config_;
    std::vector<std::string> keys_;      // preserves order (bounded)
    std::vector<Stat> stats_;            // parallel to keys_
    std::uint64_t generation_ = 0;
};

} // namespace latency_governor