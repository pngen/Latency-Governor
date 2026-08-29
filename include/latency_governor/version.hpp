#pragma once

// Latency Governor version. 1.0.0 is the first stable release.
#define LATENCY_GOVERNOR_VERSION_MAJOR 1
#define LATENCY_GOVERNOR_VERSION_MINOR 0
#define LATENCY_GOVERNOR_VERSION_PATCH 0
#define LATENCY_GOVERNOR_VERSION_STRING "1.0.0"

#include <cstdint>
#include <string_view>

namespace latency_governor {

struct RuntimeVersion {
    std::uint32_t major;
    std::uint32_t minor;
    std::uint32_t patch;
    std::uint32_t protocol_version;
    std::uint32_t persistence_version;
    std::uint32_t abi_version;
    [[nodiscard]] constexpr bool operator==(const RuntimeVersion&) const noexcept = default;
    [[nodiscard]] std::string_view string() const noexcept { return LATENCY_GOVERNOR_VERSION_STRING; }
};

[[nodiscard]] constexpr RuntimeVersion current_runtime_version() noexcept {
    return RuntimeVersion{1, 0, 0, 1, 1, 1};
}

} // namespace latency_governor
