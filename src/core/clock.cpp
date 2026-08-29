#include "latency_governor/clock.hpp"

#include <chrono>

namespace latency_governor {

std::int64_t wall_epoch_ms() noexcept {
    const auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
}

} // namespace latency_governor
