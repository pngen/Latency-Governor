#include "latency_governor/backend.hpp"
#include "latency_governor/protocol.hpp"
#include "latency_governor/clock.hpp"
#include "latency_governor/version.hpp"
#include "latency_governor/worker.hpp"

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace lg = latency_governor;
namespace net = lg::net;

namespace {

constexpr std::size_t kPrefillSize = 128;
constexpr std::size_t kDecodeSize = 96;

bool send_observation(net::SocketHandle sock, const lg::Observation& o) {
    lg::BinaryWriter w;
    net::encode_observation(w, o);
    net::Frame f;
    f.type = lg::MessageType::OBSERVATION;
    f.payload = w.data();
    std::string e;
    return net::send_frame(sock, f, e);
}

bool send_completion(net::SocketHandle sock, const lg::Completion& c) {
    lg::BinaryWriter w;
    net::encode_completion(w, c);
    net::Frame f;
    f.type = lg::MessageType::COMPLETION;
    f.payload = w.data();
    std::string e;
    return net::send_frame(sock, f, e);
}

lg::Observation make_observation(lg::ObservationType type, const lg::Intervention& iv,
                                 lg::CoordinatorEpoch epoch, lg::WorkerId wid,
                                 lg::WorkerBootId bid, lg::DispatchId dispatch,
                                 std::uint64_t& obs_counter, lg::Phase phase,
                                 std::optional<lg::Duration> elapsed,
                                 const std::string& predictor_key,
                                 lg::TimePoint at, lg::TimePoint phase_start) {
    lg::Observation o;
    o.id = lg::ObservationId(++obs_counter);
    o.type = type;
    o.request_id = iv.request_id;
    o.attempt_id = iv.attempt_id;
    // The coordinator transports the admitted request generation in the
    // intervention's decision_generation, so we recover it here.
    o.generation = lg::Generation(iv.decision_generation);
    o.epoch = epoch;
    o.dispatch_id = dispatch;
    o.worker_id = wid;
    o.worker_boot_id = bid;
    o.at = at;
    o.phase_start = phase_start;
    o.elapsed = elapsed;
    o.phase = phase;
    o.predictor_key = predictor_key;
    o.detail = std::string("worker ") + std::string(lg::to_string(type));
    return o;
}

void run_workload(net::SocketHandle sock, const lg::Intervention& iv,
                  lg::CoordinatorEpoch epoch, lg::WorkerId wid, lg::WorkerBootId bid,
                  lg::DispatchId dispatch, std::uint64_t& obs_counter,
                  lg::ExecutionBackend& backend) {
    const lg::TimePoint t_start = lg::mono_now();

    // PREFILL_STARTED marker.
    {
        const lg::Observation o = make_observation(lg::ObservationType::PREFILL_STARTED,
            iv, epoch, wid, bid, dispatch, obs_counter, lg::Phase::PREFILL,
            std::nullopt, std::string(), t_start, t_start);
        send_observation(sock, o);
    }

    std::vector<std::uint8_t> in_prefill(kPrefillSize, 0x5A);
    std::vector<std::uint8_t> out_prefill;
    const lg::ExecResult prefill = backend.execute(lg::Workload::PREFILL, kPrefillSize,
                                                   in_prefill, out_prefill);
    const lg::TimePoint t_prefill_done = lg::mono_now();
    const lg::Duration prefill_elapsed = lg::saturating_elapsed(t_start, t_prefill_done);

    // PREFILL_CHUNK_COMPLETED with the measured prefill duration.
    {
        const lg::Observation o = make_observation(lg::ObservationType::PREFILL_CHUNK_COMPLETED,
            iv, epoch, wid, bid, dispatch, obs_counter, lg::Phase::PREFILL,
            prefill_elapsed, std::string("PREFILL"), t_prefill_done, t_start);
        send_observation(sock, o);
    }

    const lg::TimePoint t_decode_start = lg::mono_now();

    // FIRST_TOKEN marker (prefill done, decode begins).
    {
        const lg::Observation o = make_observation(lg::ObservationType::FIRST_TOKEN,
            iv, epoch, wid, bid, dispatch, obs_counter, lg::Phase::DECODE,
            prefill_elapsed, std::string(), t_decode_start, t_decode_start);
        send_observation(sock, o);
    }

    std::vector<std::uint8_t> in_decode(kDecodeSize, 0xA5);
    std::vector<std::uint8_t> out_decode;
    const lg::ExecResult decode = backend.execute(lg::Workload::DECODE, kDecodeSize,
                                                  in_decode, out_decode);
    const lg::TimePoint t_decode_done = lg::mono_now();
    const lg::Duration decode_elapsed = lg::saturating_elapsed(t_decode_start, t_decode_done);

    // DECODE_STEP with the measured decode duration.
    {
        const lg::Observation o = make_observation(lg::ObservationType::DECODE_STEP,
            iv, epoch, wid, bid, dispatch, obs_counter, lg::Phase::DECODE,
            decode_elapsed, std::string("DECODE"), t_decode_done, t_decode_start);
        send_observation(sock, o);
    }

    // COMPLETION with the same authority envelope.
    lg::Completion c;
    c.request_id = iv.request_id;
    c.attempt_id = iv.attempt_id;
    c.generation = lg::Generation(iv.decision_generation);
    c.epoch = epoch;
    c.dispatch_id = dispatch;
    c.worker_id = wid;
    c.worker_boot_id = bid;
    c.outcome = lg::Completion::Outcome::COMPLETED;
    c.slo_met = true;
    c.soft_violation = false;
    c.hard_violation = false;
    c.tokens_generated = static_cast<std::uint32_t>(decode.work_items);
    c.at = lg::mono_now();
    c.detail = "worker completed " + (prefill.success && decode.success ? std::string("prefill+decode") : std::string("with backend failure"));
    send_completion(sock, c);
}

} // namespace

int main(int argc, char** argv) {
    std::string host;
    std::uint16_t port = 0;
    std::uint64_t worker_id = 0;
    std::uint64_t boot_id = 0;
    std::string backend_name = "cpu";

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--connect" && i + 2 < argc) {
            host = argv[i + 1];
            const int p = std::stoi(argv[i + 2]);
            if (p < 0 || p > 65535) { std::cerr << "invalid port" << std::endl; return 1; }
            port = static_cast<std::uint16_t>(p);
            i += 2;
        } else if (a == "--worker-id" && i + 1 < argc) {
            worker_id = std::stoull(argv[i + 1]);
            ++i;
        } else if (a == "--boot-id" && i + 1 < argc) {
            boot_id = std::stoull(argv[i + 1]);
            ++i;
        } else if (a == "--backend" && i + 1 < argc) {
            backend_name = argv[i + 1];
            ++i;
        }
    }

    if (host.empty() || port == 0 || worker_id == 0) {
        std::cerr << "usage: latency-governor-worker --connect <host> <port> "
                     "--worker-id <N> --boot-id <M> [--backend cpu|cuda]" << std::endl;
        return 1;
    }

    std::unique_ptr<lg::ExecutionBackend> backend;
    lg::WorkerCapabilities caps;
    if (backend_name == "cuda") {
#ifdef LG_HAS_CUDA
        backend = std::make_unique<lg::CudaBackend>();
        caps = backend->capabilities();
        if (!backend->available()) {
            std::cerr << "cuda backend unavailable: " << backend->device_info() << std::endl;
            return 1;
        }
#else
        std::cerr << "cuda backend requested but this build has no CUDA support" << std::endl;
        return 1;
#endif
    } else if (backend_name == "cpu") {
        backend = std::make_unique<lg::CpuBackend>();
        caps = backend->capabilities();
    } else {
        std::cerr << "unknown backend: " << backend_name << std::endl;
        return 1;
    }

    std::string err;
    net::SocketInitGuard init(err);
    if (!init.ok()) { std::cerr << "socket init failed: " << err << std::endl; return 1; }

    net::SocketHandle sock = 0;
    if (!net::tcp_connect(host, port, sock, err)) {
        std::cerr << "connect failed: " << err << std::endl;
        return 1;
    }

    // HELLO -> HELLO_ACK.
    {
        const lg::RuntimeVersion rv = lg::current_runtime_version();
        lg::BinaryWriter w;
        net::encode_hello(w, rv.protocol_version, rv.abi_version);
        net::Frame hello;
        hello.type = lg::MessageType::HELLO;
        hello.payload = w.data();
        if (!net::send_frame(sock, hello, err)) {
            std::cerr << "send hello failed: " << err << std::endl;
            net::tcp_close(sock);
            return 1;
        }
        net::Frame ack;
        if (!net::recv_frame(sock, ack, err) || ack.type != lg::MessageType::HELLO_ACK) {
            std::cerr << "hello ack failed: " << err << std::endl;
            net::tcp_close(sock);
            return 1;
        }
    }

    // REGISTER -> REGISTER_ACK (capture epoch).
    lg::CoordinatorEpoch epoch{0};
    {
        lg::WorkerDescriptor wd;
        wd.id = lg::WorkerId(worker_id);
        wd.boot_id = lg::WorkerBootId(boot_id);
        wd.host = "127.0.0.1";
        wd.port = 0;
        wd.capabilities = caps;
        wd.protocol_version = lg::current_runtime_version().protocol_version;
        wd.generation = 1;

        lg::BinaryWriter w;
        net::encode_register(w, wd);
        net::Frame reg;
        reg.type = lg::MessageType::REGISTER;
        reg.payload = w.data();
        if (!net::send_frame(sock, reg, err)) {
            std::cerr << "send register failed: " << err << std::endl;
            net::tcp_close(sock);
            return 1;
        }
        net::Frame ack;
        if (!net::recv_frame(sock, ack, err) || ack.type != lg::MessageType::REGISTER_ACK) {
            std::cerr << "register ack failed: " << err << std::endl;
            net::tcp_close(sock);
            return 1;
        }
        lg::BinaryReader rd(ack.payload);
        bool accepted = false;
        std::string msg;
        if (!net::decode_register_ack(rd, accepted, epoch, msg)) {
            std::cerr << "register ack decode failed" << std::endl;
            net::tcp_close(sock);
            return 1;
        }
        if (!accepted) {
            std::cerr << "registration rejected: " << msg << std::endl;
            net::tcp_close(sock);
            return 1;
        }
        std::cout << "worker " << worker_id << "@" << boot_id << " registered, epoch "
                  << epoch.value() << std::endl;
    }

    const lg::WorkerId wid(worker_id);
    const lg::WorkerBootId bid(boot_id);
    std::uint64_t obs_counter = 0;
    std::uint64_t dispatch_counter = 0;

    // Main receive loop.
    for (;;) {
        net::Frame f;
        if (!net::recv_frame(sock, f, err)) break;
        if (f.type != lg::MessageType::INTERVENTION) {
            // OBSERVATION_ACK / ERROR / PONG / etc. are control traffic we ignore.
            continue;
        }
        lg::BinaryReader rd(f.payload);
        lg::Intervention iv;
        if (!net::decode_intervention(rd, iv)) continue;
        if (iv.action != lg::InterventionAction::ADMIT) continue;

        const lg::DispatchId dispatch(++dispatch_counter);
        run_workload(sock, iv, epoch, wid, bid, dispatch, obs_counter, *backend);
    }

    net::tcp_close(sock);
    if (!err.empty()) std::cerr << "coordinator closed: " << err << std::endl;
    return 0;
}
