#include "latency_governor/protocol.hpp"
#include "latency_governor/governor.hpp"
#include "latency_governor/persistence.hpp"
#include "latency_governor/policy.hpp"
#include "latency_governor/clock.hpp"
#include "latency_governor/worker.hpp"
#include "latency_governor/request.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace lg = latency_governor;
namespace net = lg::net;

// Shared server state shared across the per-connection worker threads.
struct ServerState {
    lg::Governor* governor;
    lg::SystemClock* clock;
    std::mutex mutex;
    std::map<lg::WorkerId, net::SocketHandle> worker_socks;
};

namespace {

bool file_exists(const std::string& path) {
    std::ifstream f(path);
    return f.good();
}

std::array<double, lg::Phase_count> make_phase_weights(double queueing, double batch_wait,
                                                       double scheduling, double transfer,
                                                       double prefill, double decode,
                                                       double recovery, double completion) {
    std::array<double, lg::Phase_count> w{};
    w[lg::enum_index(lg::Phase::QUEUEING)] = queueing;
    w[lg::enum_index(lg::Phase::BATCH_WAIT)] = batch_wait;
    w[lg::enum_index(lg::Phase::SCHEDULING)] = scheduling;
    w[lg::enum_index(lg::Phase::TRANSFER)] = transfer;
    w[lg::enum_index(lg::Phase::PREFILL)] = prefill;
    w[lg::enum_index(lg::Phase::DECODE)] = decode;
    w[lg::enum_index(lg::Phase::RECOVERY)] = recovery;
    w[lg::enum_index(lg::Phase::COMPLETION)] = completion;
    return w;
}

lg::Policy build_default_policy() {
    lg::Policy p;
    p.name = "default";
    p.resource_pressure_sensitivity = 0.5;
    p.prediction_min_evidence = 4;
    p.fail_fast_on_deadline_exceeded = true;
    p.allow_completion_after_soft_violation = true;

    auto add_class = [&](const char* name, double priority, lg::Duration e2e, lg::Duration ttf,
                         double risk, double reserve, std::array<double, lg::Phase_count> weights) {
        lg::SloClassSpec s;
        s.name = name;
        s.priority = priority;
        s.default_e2e_target = e2e;
        s.default_ttf_target = ttf;
        s.default_deadline_risk_threshold = risk;
        s.phase_weights = weights;
        s.reserve_fraction = reserve;
        p.classes.push_back(std::move(s));
    };

    // Phase weights sum to <= 1 for every class.
    add_class("REALTIME", 5.0, lg::ms(200), lg::ms(50), 0.05, 0.05,
              make_phase_weights(0.05, 0.04, 0.04, 0.05, 0.25, 0.45, 0.04, 0.04));
    add_class("INTERACTIVE", 4.0, lg::ms(500), lg::ms(100), 0.06, 0.06,
              make_phase_weights(0.05, 0.04, 0.04, 0.05, 0.24, 0.44, 0.05, 0.05));
    add_class("STANDARD", 3.0, lg::ms(2000), lg::ms(300), 0.10, 0.08,
              make_phase_weights(0.05, 0.04, 0.04, 0.05, 0.24, 0.44, 0.05, 0.05));
    add_class("THROUGHPUT", 2.0, lg::ms(5000), lg::ms(1000), 0.12, 0.10,
              make_phase_weights(0.05, 0.04, 0.04, 0.05, 0.24, 0.44, 0.05, 0.05));
    add_class("BACKGROUND", 1.0, lg::ms(30000), lg::ms(5000), 0.20, 0.12,
              make_phase_weights(0.05, 0.04, 0.04, 0.05, 0.24, 0.44, 0.05, 0.05));

    p.retry.max_retries = 2;
    p.retry.max_cumulative_retry_delay = lg::ms(500);
    p.retry.backoff_base_ms = 10.0;
    p.retry.backoff_factor = 2.0;
    p.retry.allow_immediate_retry = false;

    p.transfer.max_transfer_fraction_of_budget = 0.20;
    p.transfer.prefer_local_when_risky = true;

    p.speculation.default_max_depth = 2;
    p.speculation.min_acceptance_rate = 0.10;
    p.speculation.max_overhead_fraction_of_budget = 0.10;

    p.fairness.max_starvation_ratio = 2;
    p.fairness.background_min_service_fraction = 0.05;
    p.fairness.protect_lower_classes_strongly = true;

    p.batch.default_max_batch_wait = lg::ms(20);
    p.batch.deadline_spread_ratio = 0.5;
    p.batch.min_batch_efficiency = 0.5;

    return p;
}

void setup_default_policy(lg::Governor& governor) {
    lg::Policy p = build_default_policy();
    std::string err;
    auto gen = governor.policy_store().add(std::move(p), err);
    if (!gen) {
        std::cerr << "failed to install default policy: " << err << std::endl;
    }
}

lg::RejectionCode rejection_code_for(const std::string& err) {
    if (err.find("not found") != std::string::npos) return lg::RejectionCode::REQUEST_NOT_FOUND;
    if (err.find("already terminal") != std::string::npos) return lg::RejectionCode::STATE_INELIGIBLE;
    if (err.find("EPOCH_MISMATCH") != std::string::npos) return lg::RejectionCode::EPOCH_MISMATCH;
    if (err.find("GENERATION_MISMATCH") != std::string::npos) return lg::RejectionCode::GENERATION_MISMATCH;
    if (err.find("ATTEMPT_MISMATCH") != std::string::npos) return lg::RejectionCode::ATTEMPT_MISMATCH;
    if (err.find("DISPATCH_MISMATCH") != std::string::npos) return lg::RejectionCode::DISPATCH_MISMATCH;
    if (err.find("WORKER_MISMATCH") != std::string::npos) return lg::RejectionCode::WORKER_MISMATCH;
    if (err.find("BOOT_MISMATCH") != std::string::npos) return lg::RejectionCode::BOOT_MISMATCH;
    return lg::RejectionCode::CORRUPT;
}

bool send_error_frame(net::SocketHandle sock, lg::RejectionCode code, const std::string& msg) {
    lg::BinaryWriter w;
    net::encode_error(w, code, msg);
    net::Frame out;
    out.type = lg::MessageType::ERROR;
    out.payload = w.data();
    std::string e;
    return net::send_frame(sock, out, e);
}

void dispatch_admit(ServerState& st, const lg::AdmitResult& res) {
    lg::Intervention iv;
    iv.action = lg::InterventionAction::ADMIT;
    iv.reason = lg::ReasonCode::DEADLINE_SLACK;
    iv.request_id = res.request_id;
    iv.attempt_id = res.attempt_id;
    iv.phase = lg::Phase::PREFILL;
    // The Intervention wire format carries no generation, so the coordinator
    // transports the admitted request generation in decision_generation for the
    // worker to echo back in the authority envelope.
    iv.decision_generation = res.generation.value();
    iv.at = st.clock->now();
    iv.detail = "dispatch";

    lg::BinaryWriter w;
    net::encode_intervention(w, iv);
    net::Frame out;
    out.type = lg::MessageType::INTERVENTION;
    out.payload = w.data();

    std::lock_guard g(st.mutex);
    for (const auto& [wid, sock] : st.worker_socks) {
        (void)wid;
        std::string e;
        if (net::send_frame(sock, out, e)) {
            std::cout << "dispatched request " << res.request_id.to_string()
                      << " to worker " << sock << std::endl;
            break;
        }
    }
}

void handle_conn(net::SocketHandle sock, ServerState& st) {
    std::string err;
    net::Frame f;
    // Handshake: HELLO -> HELLO_ACK.
    if (!net::recv_frame(sock, f, err)) { net::tcp_close(sock); return; }
    if (f.type != lg::MessageType::HELLO) { net::tcp_close(sock); return; }
    {
        lg::BinaryReader rd(f.payload);
        std::uint32_t proto = 0, abi = 0;
        net::decode_hello(rd, proto, abi);
        (void)proto;
        (void)abi;
    }
    net::Frame ack;
    ack.type = lg::MessageType::HELLO_ACK;
    if (!net::send_frame(sock, ack, err)) { net::tcp_close(sock); return; }

    bool is_worker = false;
    lg::WorkerId wid;
    lg::WorkerBootId bid;

    for (;;) {
        if (!net::recv_frame(sock, f, err)) break;
        switch (f.type) {
            case lg::MessageType::REGISTER: {
                lg::BinaryReader rd(f.payload);
                lg::WorkerDescriptor wd;
                if (net::decode_register(rd, wd)) {
                    std::string werr;
                    const bool ok = st.governor->register_worker(wd, werr);
                    lg::BinaryWriter w;
                    net::encode_register_ack(w, ok, st.governor->coordinator_epoch(),
                                             ok ? std::string() : werr);
                    net::Frame out;
                    out.type = lg::MessageType::REGISTER_ACK;
                    out.payload = w.data();
                    net::send_frame(sock, out, err);
                    if (ok) {
                        std::lock_guard g(st.mutex);
                        st.worker_socks[wd.id] = sock;
                        is_worker = true;
                        wid = wd.id;
                        bid = wd.boot_id;
                        std::cout << "worker " << wd.id.to_string()
                                  << "@" << wd.boot_id.to_string()
                                  << " registered" << std::endl;
                    }
                } else {
                    send_error_frame(sock, lg::RejectionCode::MALFORMED, "bad register");
                }
                break;
            }
            case lg::MessageType::ADMIT: {
                lg::BinaryReader rd(f.payload);
                lg::RequestDescriptor desc;
                if (net::decode_admit(rd, desc)) {
                    const lg::AdmitResult res = st.governor->admit(desc);
                    lg::BinaryWriter w;
                    net::encode_admit_ack(w, res.accepted, res.code, res.request_id,
                                          res.attempt_id, res.generation, res.epoch, res.detail);
                    net::Frame out;
                    out.type = lg::MessageType::ADMIT_ACK;
                    out.payload = w.data();
                    net::send_frame(sock, out, err);
                    // (auto-dispatch disabled for deterministic control in the restart-fencing proof.)
                } else {
                    send_error_frame(sock, lg::RejectionCode::MALFORMED, "bad admit");
                }
                break;
            }
            case lg::MessageType::OBSERVATION: {
                lg::BinaryReader rd(f.payload);
                lg::Observation obs;
                if (net::decode_observation(rd, obs)) {
                    const lg::ObservationResult rr = st.governor->observe(obs);
                    lg::BinaryWriter w;
                    net::encode_observation_ack(w, rr.accepted, rr.code);
                    net::Frame oack; oack.type = lg::MessageType::OBSERVATION_ACK; oack.payload = w.data();
                    std::string serr; net::send_frame(sock, oack, serr);
                } else {
                    send_error_frame(sock, lg::RejectionCode::MALFORMED, "bad observation");
                }
                break;
            }
            case lg::MessageType::COMPLETION: {
                lg::BinaryReader rd(f.payload);
                lg::Completion comp;
                if (net::decode_completion(rd, comp)) {
                    std::string cerr;
                    const bool ok = st.governor->commit(comp, cerr);
                    const lg::RejectionCode code = ok ? lg::RejectionCode::NONE
                                                      : rejection_code_for(cerr);
                    std::string msg = ok ? "completion accepted" : cerr;
                    if (msg.empty()) msg = "completion rejected";
                    send_error_frame(sock, code, msg);
                } else {
                    send_error_frame(sock, lg::RejectionCode::MALFORMED, "bad completion");
                }
                break;
            }
            case lg::MessageType::SNAPSHOT: {
                const lg::Snapshot s = st.governor->snapshot();
                lg::BinaryWriter w;
                net::encode_snapshot(w, s);
                net::Frame out;
                out.type = lg::MessageType::SNAPSHOT;
                out.payload = w.data();
                net::send_frame(sock, out, err);
                break;
            }
            case lg::MessageType::PING: {
                net::Frame pong;
                pong.type = lg::MessageType::PONG;
                net::send_frame(sock, pong, err);
                break;
            }
            case lg::MessageType::SHUTDOWN:
                net::tcp_close(sock);
                return;
            default:
                // Unknown or master-not-consumed control messages are ignored.
                break;
        }
    }

    if (is_worker) {
        std::string werr;
        st.governor->unregister_worker(wid, bid, werr);
        st.governor->fail_requests_for_worker(wid, bid);
        st.governor->bump_epoch_and_generations();
        std::lock_guard g(st.mutex);
        auto it = st.worker_socks.find(wid);
        if (it != st.worker_socks.end() && it->second == sock) st.worker_socks.erase(it);
        std::cout << "worker " << wid.to_string() << " disconnected" << std::endl;
    }
    net::tcp_close(sock);
}

} // namespace

int main(int argc, char** argv) {
    std::string host = "127.0.0.1";
    std::uint16_t port = 0;
    std::string persist;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--listen" && i + 2 < argc) {
            host = argv[i + 1];
            const int p = std::stoi(argv[i + 2]);
            if (p < 0 || p > 65535) {
                std::cerr << "invalid port: " << p << std::endl;
                return 1;
            }
            port = static_cast<std::uint16_t>(p);
            i += 2;
        } else if (a == "--persist" && i + 1 < argc) {
            persist = argv[i + 1];
            ++i;
        }
    }

    if (port == 0) {
        std::cerr << "usage: latency-governor-coordinator --listen <host> <port> [--persist <file>]"
                  << std::endl;
        return 1;
    }

    std::string err;
    net::SocketInitGuard init(err);
    if (!init.ok()) { std::cerr << "socket init failed: " << err << std::endl; return 1; }

    net::SocketHandle listen_sock = 0;
    if (!net::tcp_listen(host, port, listen_sock, err)) {
        std::cerr << "listen failed: " << err << std::endl;
        return 1;
    }
    std::cout << "latency-governor-coordinator listening on " << host << ":" << port << std::endl;

    lg::SystemClock clock;
    lg::GovernorConfig cfg;
    lg::Governor governor(cfg, clock);

    bool loaded = false;
    if (!persist.empty() && file_exists(persist)) {
        std::string lerr;
        loaded = load_from_file(governor, persist, lerr);
        if (loaded) {
            std::cout << "loaded governor state from " << persist << std::endl;
        } else {
            std::cerr << "failed to load " << persist << ": " << lerr << std::endl;
        }
    }
    if (!loaded) setup_default_policy(governor);

    ServerState state{&governor, &clock};

    // Periodic persistence (best-effort) while the coordinator runs.
    std::atomic<bool> running{true};
    std::thread persist_thread;
    if (!persist.empty()) {
        persist_thread = std::thread([&]() {
            while (running.load()) {
                std::this_thread::sleep_for(std::chrono::seconds(5));
                std::string perr;
                if (!persist_to_file(governor, persist, perr)) {
                    std::cerr << "persist failed: " << perr << std::endl;
                }
            }
        });
    }

    for (;;) {
        net::SocketHandle conn = 0;
        if (!net::tcp_accept(listen_sock, conn, err)) {
            std::cerr << "accept failed: " << err << std::endl;
            break;
        }
        std::thread(handle_conn, conn, std::ref(state)).detach();
    }

    running.store(false);
    if (persist_thread.joinable()) persist_thread.join();
    if (!persist.empty()) {
        std::string perr;
        persist_to_file(governor, persist, perr);
    }
    net::tcp_close(listen_sock);
    return 0;
}
