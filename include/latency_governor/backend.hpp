#pragma once

#include "latency_governor/clock.hpp"
#include "latency_governor/duration.hpp"
#include "latency_governor/enums.hpp"
#include "latency_governor/identifiers.hpp"
#include "latency_governor/worker.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace latency_governor {

// The two materially-different workload classes the backend must exercise.
enum class Workload { PREFILL, DECODE };

// A completed execution with its measured monotonic duration and a numeric
// correctness fingerprint. The fingerprint is produced by the backend and
// verified by the caller so that "work actually completed" is provable.
struct ExecResult {
    bool success = false;
    Duration duration{0};
    DeviceClass device = DeviceClass::UNKNOWN;
    std::uint64_t work_items = 0;
    std::uint64_t checksum = 0;
    std::uint64_t ticks = 0;
    std::string detail;
};

// A vendor-neutral execution backend. The latency governor core is neutral
// w.r.t. backends; concrete backends plug in here.
class ExecutionBackend {
public:
    virtual ~ExecutionBackend() = default;

    [[nodiscard]] virtual BackendId id() const = 0;
    [[nodiscard]] virtual DeviceClass device_class() const = 0;
    [[nodiscard]] virtual std::string name() const = 0;
    [[nodiscard]] virtual bool available() const = 0;
    [[nodiscard]] virtual std::string device_info() const = 0;

    [[nodiscard]] virtual bool supports(Workload w) const = 0;

    virtual ExecResult execute(Workload w, std::size_t size,
                               const std::vector<std::uint8_t>& input,
                               std::vector<std::uint8_t>& output) = 0;

    virtual Duration calibrate(Workload w, std::size_t size) = 0;

    [[nodiscard]] virtual WorkerCapabilities capabilities() const = 0;
};

// ---- CPU backend (always available; purely host execution) ----------------
struct CpuBackendOptions {
    std::size_t work_factor = 8;
};

class CpuBackend final : public ExecutionBackend {
public:
    explicit CpuBackend(BackendId id = BackendId(1), CpuBackendOptions opts = {});
    [[nodiscard]] BackendId id() const override { return id_; }
    [[nodiscard]] DeviceClass device_class() const override { return DeviceClass::CPU; }
    [[nodiscard]] std::string name() const override { return "cpu"; }
    [[nodiscard]] bool available() const override { return true; }
    [[nodiscard]] std::string device_info() const override { return "cpu:host"; }
    [[nodiscard]] bool supports(Workload w) const override { return w == Workload::PREFILL || w == Workload::DECODE; }
    ExecResult execute(Workload w, std::size_t size, const std::vector<std::uint8_t>& input,
                       std::vector<std::uint8_t>& output) override;
    Duration calibrate(Workload w, std::size_t size) override;
    [[nodiscard]] WorkerCapabilities capabilities() const override;

private:
    BackendId id_;
    CpuBackendOptions opts_;
};

// ---- CUDA backend (validated on NVIDIA RTX 5090, compute capability 12.0) ---
struct CudaBackendOptions {
    int device = 0;
    std::size_t block_size = 256;
    bool enable_verify = true;
};

class CudaBackend final : public ExecutionBackend {
public:
    explicit CudaBackend(BackendId id = BackendId(2), CudaBackendOptions opts = {});
    ~CudaBackend() override;
    [[nodiscard]] BackendId id() const override { return id_; }
    [[nodiscard]] DeviceClass device_class() const override { return DeviceClass::CUDA; }
    [[nodiscard]] std::string name() const override { return "cuda"; }
    [[nodiscard]] bool available() const override;
    [[nodiscard]] std::string device_info() const override;
    [[nodiscard]] bool supports(Workload w) const override { return w == Workload::PREFILL || w == Workload::DECODE; }
    ExecResult execute(Workload w, std::size_t size, const std::vector<std::uint8_t>& input,
                       std::vector<std::uint8_t>& output) override;
    Duration calibrate(Workload w, std::size_t size) override;
    [[nodiscard]] WorkerCapabilities capabilities() const override;
    void shutdown() noexcept;

private:
    BackendId id_;
    CudaBackendOptions opts_;
    bool initialized_ = false;
    std::uint64_t device_memory_ = 0;
};

} // namespace latency_governor
