#include "latency_governor/prediction.hpp"

#include <algorithm>
#include <cmath>

namespace latency_governor {

namespace {
[[nodiscard]] double to_us(Duration d) noexcept {
    return static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(d).count());
}
[[nodiscard]] Duration from_us(double us) noexcept {
    const auto clamped = std::clamp(us, -1.0, static_cast<double>(std::numeric_limits<std::int64_t>::max() / 1000.0));
    return std::chrono::microseconds(static_cast<std::int64_t>(clamped));
}
} // namespace

Predictor::Predictor(PredictorConfig cfg) : config_(cfg) {}

const Predictor::Stat* Predictor::find(std::string_view key) const noexcept {
    for (std::size_t i = 0; i < keys_.size(); ++i) {
        if (keys_[i] == key) return &stats_[i];
    }
    return nullptr;
}

Predictor::Stat* Predictor::find_or_create(std::string_view key) {
    for (std::size_t i = 0; i < keys_.size(); ++i) {
        if (keys_[i] == key) return &stats_[i];
    }
    if (keys_.size() >= config_.max_keys) return nullptr;   // bounded
    keys_.emplace_back(key);
    stats_.push_back({});
    return &stats_.back();
}

void Predictor::record(std::string_view key, Duration sample) noexcept {
    if (sample < Duration::zero()) return;   // clock-order anomaly: ignore
    Stat* st = find_or_create(key);
    if (st == nullptr) return;   // bounded
    if (st->samples.size() >= config_.max_window) st->samples.pop_front();
    st->samples.push_back(sample);
    st->count = st->samples.size();
    ++st->version;
}

std::optional<Prediction> Predictor::predict(std::string_view key) const noexcept {
    const Stat* st = find(key);
    if (st == nullptr || st->samples.empty()) return std::nullopt;   // cold start
    double sum = 0.0;
    double sum_sq = 0.0;
    std::size_t n = 0;
    for (auto& d : st->samples) {
        const double v = to_us(d);
        sum += v;
        sum_sq += v * v;
        ++n;
    }
    const double mean = sum / static_cast<double>(n);
    const double variance = (n > 1) ? ((sum_sq - (sum * sum) / static_cast<double>(n)) / static_cast<double>(n - 1)) : 0.0;
    const double sigma = std::sqrt(std::max(0.0, variance));
    const double lo = std::max(0.0, mean - config_.interval_sigma * sigma);
    const double hi = mean + config_.interval_sigma * sigma;

    Prediction p;
    p.predicted = from_us(mean);
    p.lower = from_us(lo);
    p.upper = from_us(hi);
    p.evidence_count = n;
    p.predictor_generation = generation_;
    p.version = st->version;
    if (n >= config_.high_confidence_evidence) {
        p.confidence = ConfidenceClass::HIGH;
    } else if (n >= config_.medium_confidence_evidence) {
        p.confidence = ConfidenceClass::MEDIUM;
    } else {
        p.confidence = ConfidenceClass::LOW;
    }
    p.source = PredictionSource::MEASURED;
    return p;
}

std::size_t Predictor::evidence_count(std::string_view key) const noexcept {
    const Stat* st = find(key);
    return st ? st->samples.size() : 0;
}

std::vector<Duration> Predictor::samples(std::string_view key) const {
    std::vector<Duration> out;
    const Stat* st = find(key);
    if (st == nullptr) return out;
    out.assign(st->samples.begin(), st->samples.end());
    return out;
}

void Predictor::set_config(const PredictorConfig& cfg) noexcept {
    config_ = cfg;
    keys_.clear();
    stats_.clear();
    ++generation_;
}

void Predictor::reset() noexcept {
    keys_.clear();
    stats_.clear();
    ++generation_;
}

} // namespace latency_governor