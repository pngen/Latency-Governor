#include "latency_governor/explanation.hpp"

#include <sstream>

namespace latency_governor {

void Explanation::add_factor(ReasonCode reason, double threshold, double observed, std::string desc) {
    factors.push_back({reason, threshold, observed, std::move(desc)});
}

void Explanation::add_line(std::string line) { lines.push_back(std::move(line)); }

std::string Explanation::text() const {
    std::ostringstream os;
    os << title << "\n";
    for (const auto& l : lines) os << "  " << l << "\n";
    size_t i = 0;
    for (const auto& f : factors) {
        os << "  factor[" << i++ << "] reason=" << to_string(f.reason)
           << " threshold=" << f.threshold << " observed=" << f.observed
           << " : " << f.description << "\n";
    }
    return os.str();
}

std::string Explanation::json() const {
    std::ostringstream os;
    os << "{\"request\":" << request_id.to_string()
       << ",\"title\":\"" << title << "\""
       << ",\"decision_generation\":" << decision_generation
       << ",\"policy_generation\":" << policy_generation
       << ",\"lines\":[";
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i) os << ",";
        os << "\"" << lines[i] << "\"";
    }
    os << "],\"factors\":[";
    for (size_t i = 0; i < factors.size(); ++i) {
        if (i) os << ",";
        os << "{\"reason\":\"" << to_string(factors[i].reason)
           << "\",\"threshold\":" << factors[i].threshold
           << ",\"observed\":" << factors[i].observed
           << ",\"description\":\"" << factors[i].description << "\"}";
    }
    os << "]}";
    return os.str();
}

} // namespace latency_governor
