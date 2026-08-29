#include "latency_governor/backend.hpp"

#include <cstring>
#include <random>

namespace latency_governor {

namespace {
std::uint64_t fnv(const std::vector<std::uint8_t>& data) {
    std::uint64_t h = 1469598103934665603ULL;
    for (std::uint8_t b : data) { h ^= b; h *= 1099511628211ULL; }
    return h;
}
} // namespace

CpuBackend::CpuBackend(BackendId id, CpuBackendOptions opts) : id_(id), opts_(opts) {}

ExecResult CpuBackend::execute(Workload w, std::size_t size, const std::vector<std::uint8_t>& input,
                               std::vector<std::uint8_t>& output) {
    ExecResult r;
    r.device = DeviceClass::CPU;
    output.resize(size);
    const auto start = mono_now();
    std::uint64_t checksum = 0;
    std::uint64_t acc = 0;
    // A deterministic, bounded compute loop. PREFILL does more work per element
    // (bulk), DECODE does less (per-token), so the two classes behave
    // differently under the governor.
    const std::uint64_t work = (w == Workload::PREFILL) ? (opts_.work_factor * 4) : opts_.work_factor;
    for (std::size_t i = 0; i < size; ++i) {
        std::uint64_t x = static_cast<std::uint64_t>(i) * 2654435761ULL;
        std::uint64_t v = input.empty() ? x : static_cast<std::uint64_t>(input[i % input.size()]);
        for (std::uint64_t k = 0; k < work; ++k) {
            v = (v ^ (v >> 7)) * 0x9E3779B97F4A7C15ULL + k;
            acc += v;
        }
        checksum ^= v;
        output[i] = static_cast<std::uint8_t>(v & 0xFF);
    }
    const auto end = mono_now();
    r.success = true;
    r.duration = saturating_elapsed(start, end);
    r.work_items = size;
    r.checksum = checksum ^ acc;
    r.ticks = static_cast<std::uint64_t>(size) * work;
    r.detail = "cpu prefill/decode bounded workload";
    return r;
}

Duration CpuBackend::calibrate(Workload w, std::size_t size) {
    std::vector<std::uint8_t> in(size, 0x2A), out;
    ExecResult r = execute(w, size, in, out);
    return r.duration;
}

WorkerCapabilities CpuBackend::capabilities() const {
    WorkerCapabilities c;
    c.device = DeviceClass::CPU;
    c.accelerator = false;
    c.backend_name = "cpu";
    c.backend_version = "1.0";
    c.total_memory = 0;
    c.free_memory = 0;
    c.supported_ops = {"prefill", "decode"};
    return c;
}

} // namespace latency_governor
