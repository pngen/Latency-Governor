#include "latency_governor/protocol.hpp"
#include "latency_governor/clock.hpp"
#include "latency_governor/duration.hpp"
#include "latency_governor/enums.hpp"
#include "latency_governor/request.hpp"
#include "latency_governor/version.hpp"

#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace lg = latency_governor;
namespace net = lg::net;

namespace {

std::vector<std::string> split_ids(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ',' || c == ' ') {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

bool connect_and_handshake(const std::string& host, std::uint16_t port, net::SocketHandle& sock,
                           std::string& error) {
    if (!net::tcp_connect(host, port, sock, error)) return false;
    const lg::RuntimeVersion rv = lg::current_runtime_version();
    lg::BinaryWriter w;
    net::encode_hello(w, rv.protocol_version, rv.abi_version);
    net::Frame hello;
    hello.type = lg::MessageType::HELLO;
    hello.payload = w.data();
    if (!net::send_frame(sock, hello, error)) {
        net::tcp_close(sock);
        return false;
    }
    net::Frame ack;
    if (!net::recv_frame(sock, ack, error) || ack.type != lg::MessageType::HELLO_ACK) {
        net::tcp_close(sock);
        return false;
    }
    return true;
}

int cmd_ping(const std::string& host, std::uint16_t port) {
    std::string err;
    net::SocketInitGuard init(err);
    if (!init.ok()) { std::cerr << "socket init failed: " << err << std::endl; return 1; }
    net::SocketHandle sock = 0;
    if (!connect_and_handshake(host, port, sock, err)) {
        std::cerr << "ping failed: " << err << std::endl;
        return 1;
    }
    net::tcp_close(sock);
    std::cout << "pong from " << host << ":" << port << std::endl;
    return 0;
}

int cmd_protocol_status(const std::string& host, std::uint16_t port) {
    std::string err;
    net::SocketInitGuard init(err);
    if (!init.ok()) { std::cerr << "socket init failed: " << err << std::endl; return 1; }
    net::SocketHandle sock = 0;
    if (!connect_and_handshake(host, port, sock, err)) {
        std::cerr << "protocol-status failed: " << err << std::endl;
        return 1;
    }
    const lg::RuntimeVersion rv = lg::current_runtime_version();
    std::cout << "protocol_version=" << rv.protocol_version << std::endl;
    std::cout << "abi_version=" << rv.abi_version << std::endl;
    std::cout << "persistence_version=" << rv.persistence_version << std::endl;
    std::cout << "coordinator=" << host << ":" << port << " (handshake ok)" << std::endl;
    net::tcp_close(sock);
    return 0;
}

int cmd_snapshot(const std::string& host, std::uint16_t port) {
    std::string err;
    net::SocketInitGuard init(err);
    if (!init.ok()) { std::cerr << "socket init failed: " << err << std::endl; return 1; }
    net::SocketHandle sock = 0;
    if (!connect_and_handshake(host, port, sock, err)) {
        std::cerr << "snapshot failed: " << err << std::endl;
        return 1;
    }
    net::Frame req;
    req.type = lg::MessageType::SNAPSHOT;
    if (!net::send_frame(sock, req, err)) {
        std::cerr << "send snapshot failed: " << err << std::endl;
        net::tcp_close(sock);
        return 1;
    }
    net::Frame resp;
    if (!net::recv_frame(sock, resp, err) || resp.type != lg::MessageType::SNAPSHOT) {
        std::cerr << "snapshot recv failed: " << err << std::endl;
        net::tcp_close(sock);
        return 1;
    }
    lg::BinaryReader rd(resp.payload);
    lg::Snapshot s;
    if (!net::decode_snapshot(rd, s)) {
        std::cerr << "snapshot decode failed" << std::endl;
        net::tcp_close(sock);
        return 1;
    }
    std::cout << s.json << std::endl;
    net::tcp_close(sock);
    return 0;
}

int cmd_admit(const std::string& host, std::uint16_t port,
              const std::vector<std::string>& req_ids, const std::vector<std::string>& tenant_ids,
              const std::vector<std::string>& model_ids, lg::SloClass slo_class,
              std::int64_t e2e_ms) {
    std::string err;
    net::SocketInitGuard init(err);
    if (!init.ok()) { std::cerr << "socket init failed: " << err << std::endl; return 1; }
    net::SocketHandle sock = 0;
    if (!connect_and_handshake(host, port, sock, err)) {
        std::cerr << "admit failed: " << err << std::endl;
        return 1;
    }

    int rc = 0;
    for (std::size_t i = 0; i < req_ids.size(); ++i) {
        lg::RequestId req_id;
        if (!lg::RequestId::parse(req_ids[i], req_id) || !req_id.is_valid()) {
            std::cerr << "invalid request id: " << req_ids[i] << std::endl;
            rc = 1;
            continue;
        }
        const std::string tenant_str = tenant_ids.empty() ? "1" : tenant_ids[i % tenant_ids.size()];
        const std::string model_str = model_ids.empty() ? "1" : model_ids[i % model_ids.size()];
        lg::TenantId tenant;
        lg::ModelId model;
        if (!lg::TenantId::parse(tenant_str, tenant) || !lg::ModelId::parse(model_str, model)) {
            std::cerr << "invalid tenant/model id" << std::endl;
            rc = 1;
            continue;
        }

        lg::RequestDescriptor desc;
        desc.request_id = req_id;
        desc.tenant_id = tenant;
        desc.model_id = model;
        desc.model_revision = lg::ModelRevision(1);
        desc.adapter_id.reset();
        desc.slo_class = slo_class;
        desc.contract.e2e_target = e2e_ms > 0 ? lg::ms(e2e_ms) : lg::Duration::zero();
        desc.contract.ttf_target = lg::Duration::zero();
        desc.contract.deadline_risk_threshold = 0.10;
        desc.prompt_tokens = 128;
        desc.max_tokens = 256;
        desc.remaining_tokens = 256;
        desc.backend_hint.reset();
        desc.device_hint = lg::DeviceClass::UNKNOWN;
        desc.warm_cache_hint = false;
        desc.arrival = lg::mono_now();

        lg::BinaryWriter w;
        net::encode_admit(w, desc);
        net::Frame req;
        req.type = lg::MessageType::ADMIT;
        req.payload = w.data();
        if (!net::send_frame(sock, req, err)) {
            std::cerr << "send admit failed: " << err << std::endl;
            rc = 1;
            break;
        }
        net::Frame resp;
        if (!net::recv_frame(sock, resp, err) || resp.type != lg::MessageType::ADMIT_ACK) {
            std::cerr << "admit recv failed: " << err << std::endl;
            rc = 1;
            break;
        }
        lg::BinaryReader rd(resp.payload);
        bool accepted = false;
        lg::RejectionCode code = lg::RejectionCode::NONE;
        lg::RequestId ack_req;
        lg::AttemptId attempt;
        lg::Generation gen;
        lg::CoordinatorEpoch epoch;
        std::string msg;
        if (!net::decode_admit_ack(rd, accepted, code, ack_req, attempt, gen, epoch, msg)) {
            std::cerr << "admit ack decode failed" << std::endl;
            rc = 1;
            break;
        }
        std::cout << "accepted=" << (accepted ? "true" : "false")
                  << " request=" << ack_req.to_string()
                  << " attempt=" << attempt.to_string()
                  << " generation=" << gen.to_string()
                  << " epoch=" << epoch.to_string()
                  << " code=" << lg::to_string(code)
                  << (!msg.empty() ? (" msg=" + msg) : std::string())
                  << std::endl;
    }
    net::tcp_close(sock);
    return rc;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: latency-governorctl <ping|snapshot|admit|protocol-status> ..."
                  << std::endl;
        return 1;
    }
    const std::string cmd = argv[1];
    if (cmd == "ping" || cmd == "protocol-status" || cmd == "snapshot") {
        if (argc < 4) {
            std::cerr << "usage: latency-governorctl " << cmd << " <host> <port>" << std::endl;
            return 1;
        }
        const std::string host = argv[2];
        const int p = std::stoi(argv[3]);
        if (p < 0 || p > 65535) { std::cerr << "invalid port" << std::endl; return 1; }
        const std::uint16_t port = static_cast<std::uint16_t>(p);
        if (cmd == "ping") return cmd_ping(host, port);
        if (cmd == "protocol-status") return cmd_protocol_status(host, port);
        return cmd_snapshot(host, port);
    }
    if (cmd == "admit") {
        if (argc < 7) {
            std::cerr << "usage: latency-governorctl admit <host> <port> <request-ids> "
                         "<tenant-ids> <model-ids> [--class REALTIME] [--e2e-ms N]" << std::endl;
            return 1;
        }
        const std::string host = argv[2];
        const int p = std::stoi(argv[3]);
        if (p < 0 || p > 65535) { std::cerr << "invalid port" << std::endl; return 1; }
        const std::uint16_t port = static_cast<std::uint16_t>(p);
        const std::vector<std::string> req_ids = split_ids(argv[4]);
        const std::vector<std::string> tenant_ids = split_ids(argv[5]);
        const std::vector<std::string> model_ids = split_ids(argv[6]);
        lg::SloClass slo_class = lg::SloClass::STANDARD;
        std::int64_t e2e_ms = 0;
        for (int i = 7; i < argc; ++i) {
            const std::string a = argv[i];
            if (a == "--class" && i + 1 < argc) {
                auto c = lg::enum_from_string<lg::SloClass>(argv[i + 1]);
                if (!c) {
                    std::cerr << "invalid class: " << argv[i + 1] << std::endl;
                    return 1;
                }
                slo_class = *c;
                ++i;
            } else if (a == "--e2e-ms" && i + 1 < argc) {
                e2e_ms = std::stoll(argv[i + 1]);
                ++i;
            }
        }
        return cmd_admit(host, port, req_ids, tenant_ids, model_ids, slo_class, e2e_ms);
    }

    std::cerr << "unknown subcommand: " << cmd << std::endl;
    return 1;
}
