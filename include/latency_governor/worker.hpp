#pragma once

#include "latency_governor/clock.hpp"
#include "latency_governor/duration.hpp"
#include "latency_governor/enums.hpp"
#include "latency_governor/identifiers.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace latency_governor {

// What a worker declares it can execute. Governed latency accounting needs
// enough backend metadata to reason about placement and feasibility.
struct WorkerCapabilities {
    DeviceClass device = DeviceClass::UNKNOWN;
    bool accelerator = false;
    std::string backend_name;         // e.g. "cuda"
    std::string backend_version;      // e.g. "13.1"
    Bytes total_memory = 0;
    Bytes free_memory = 0;
    std::vector<std::string> supported_ops;  // e.g. "prefill", "decode"
};

// The complete, authority-bound identity of a registered worker.
struct WorkerDescriptor {
    WorkerId id;
    WorkerBootId boot_id;     // changes on every process incarnation
    std::string host;
    std::uint16_t port = 0;
    WorkerCapabilities capabilities;
    std::uint32_t protocol_version = 0;
    std::uint32_t generation = 0;
    TimePoint registered_at{};
    bool alive = false;       // true while the registration is current
    TimePoint last_heartbeat{};
};

} // namespace latency_governor
