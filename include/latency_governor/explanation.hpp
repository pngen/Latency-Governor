#pragma once

#include "latency_governor/clock.hpp"
#include "latency_governor/duration.hpp"
#include "latency_governor/enums.hpp"
#include "latency_governor/identifiers.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace latency_governor {

// A deterministic, human- and machine-readable explanation of a governor
// decision. Structured reason codes plus human-readable descriptions; never a
// single opaque magic score.
struct Explanation {
    RequestId request_id;
    std::string title;
    std::size_t decision_generation = 0;
    std::uint64_t policy_generation = 0;

    struct Factor {
        ReasonCode reason;
        double threshold;
        double observed;
        std::string description;
    };
    std::vector<Factor> factors;

    std::vector<std::string> lines;   // canned prose

    [[nodiscard]] std::string text() const;   // plain text rendering
    [[nodiscard]] std::string json() const;   // canonical JSON rendering

    void add_factor(ReasonCode reason, double threshold, double observed, std::string desc);
    void add_line(std::string line);
};

} // namespace latency_governor
