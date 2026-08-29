#include "latency_governor/backend.hpp"

#include "cuda_kernels.h"

#include <cstring>
#include <sstream>

namespace latency_governor {

CudaBackend::CudaBackend(BackendId id, CudaBackendOptions opts) : id_(id), opts_(opts) {}
CudaBackend::~CudaBackend() { shutdown(); }

bool CudaBackend::available() const {
    const char* info = nullptr;
    return lg_cuda_available(opts_.device, &info) == 0;
}

std::string CudaBackend::device_info() const {
    const char* info = nullptr;
    if (lg_cuda_available(opts_.device, &info) != 0) return "cuda:unavailable";
    return info ? std::string(info) : "cuda:unknown";
}

WorkerCapabilities CudaBackend::capabilities() const {
    WorkerCapabilities c;
    c.device = DeviceClass::CUDA;
    c.accelerator = true;
    c.backend_name = "cuda";
    c.backend_version = "13.1";
    unsigned long long total = 0, free_mem = 0;
    if (lg_cuda_memory(opts_.device, &total, &free_mem) == 0) {
        c.total_memory = (Bytes)total;
        c.free_memory = (Bytes)free_mem;
    }
    c.supported_ops = {"prefill", "decode"};
    return c;
}

ExecResult CudaBackend::execute(Workload w, std::size_t size,
                                const std::vector<std::uint8_t>& input,
                                std::vector<std::uint8_t>& output) {
    ExecResult r;
    r.device = DeviceClass::CUDA;
    const std::size_t n = (size == 0) ? 1 : size;
    std::vector<float> h_in(n), h_out(n);
    for (std::size_t i = 0; i < n; ++i) {
        h_in[i] = input.empty() ? static_cast<float>(i % 251)
                                : static_cast<float>(input[i % input.size()]);
    }
    unsigned long long checksum = 0;
    const std::size_t reps = 200;   // prefill bulk cost
    const std::size_t steps = 100;  // decode iterative cost
    const auto start = mono_now();
    int rc = (w == Workload::PREFILL)
                 ? lg_cuda_run_prefill(h_in.data(), n, reps, h_out.data(), &checksum)
                 : lg_cuda_run_decode(h_in.data(), n, steps, h_out.data(), &checksum);
    const auto end = mono_now();
    if (rc != 0) {
        r.success = false;
        r.detail = "cuda execution failed (rc=" + std::to_string(rc) + ")";
        return r;
    }
    // Numeric fingerprint over the returned buffer.
    std::uint64_t sum = 0, xr = 0;
    for (std::size_t i = 0; i < n; ++i) {
        std::uint64_t v = static_cast<std::uint64_t>(static_cast<unsigned int>(h_out[i] * 1000.0f));
        sum += v; xr ^= v * 0x9E3779B97F4A7C15ULL;
    }
    r.success = true;
    r.duration = saturating_elapsed(start, end);
    r.work_items = n;
    r.checksum = sum ^ xr;
    r.ticks = static_cast<std::uint64_t>(n) * ((w == Workload::PREFILL) ? reps : steps);
    r.detail = "cuda " + std::string(w == Workload::PREFILL ? "prefill" : "decode") + " workload";
    // Copy the float result into the output byte buffer for verification.
    output.resize(n * sizeof(float));
    std::memcpy(output.data(), h_out.data(), n * sizeof(float));
    return r;
}

Duration CudaBackend::calibrate(Workload w, std::size_t size) {
    std::vector<std::uint8_t> in(size, 0x2A), out;
    ExecResult r = execute(w, size, in, out);
    return r.success ? r.duration : Duration(-1);
}

void CudaBackend::shutdown() noexcept {
    // No persistent stream/context is held across calls; the CUDA runtime manages
    // the context. Nothing to tear down beyond clearing the initialized flag.
    initialized_ = false;
}

} // namespace latency_governor