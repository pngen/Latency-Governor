#pragma once

#include "latency_governor/clock.hpp"
#include "latency_governor/enums.hpp"
#include "latency_governor/governor.hpp"
#include "latency_governor/intervention.hpp"
#include "latency_governor/observation.hpp"
#include "latency_governor/request.hpp"
#include "latency_governor/serialization.hpp"
#include "latency_governor/snapshot.hpp"
#include "latency_governor/worker.hpp"

#include <cstdint>
#include <string>

namespace latency_governor {
namespace net {

// Opaque socket handle. The public header stays Winsock-free to avoid macro
// pollution; protocol.cpp owns the OS socket type.
using SocketHandle = std::int64_t;

// A single framed message on the wire.
struct Frame {
    std::uint32_t magic = kFrameMagic;
    std::uint32_t version = kProtocolVersion;
    MessageType type = MessageType::HELLO;
    std::string payload;        // canonical serialized payload (bounded)
};

constexpr std::size_t kFrameHeaderSize = 4 + 4 + 4 + 4;   // magic + version + type + length
constexpr std::size_t kMaxFramePayload = kMaxFrameSize - kFrameHeaderSize;

// Winsock / socket init. Call once at process start; RAII wrapper provided.
bool socket_init(std::string& error);
void socket_cleanup();

// Frame-level primitives. These perform a complete read/write loop (a single
// recv/send is never assumed to equal one message).
bool send_frame(SocketHandle sock, const Frame& f, std::string& error);
bool recv_frame(SocketHandle sock, Frame& f, std::string& error);

bool tcp_listen(const std::string& host, std::uint16_t port, SocketHandle& out_handle, std::string& error);
bool tcp_accept(SocketHandle listen, SocketHandle& out_conn, std::string& error);
bool tcp_connect(const std::string& host, std::uint16_t port, SocketHandle& out_sock, std::string& error);
void tcp_close(SocketHandle sock);
void tcp_set_nonblocking(SocketHandle sock, bool enabled);

// RAII init guard.
class SocketInitGuard {
public:
    explicit SocketInitGuard(std::string& error) : ok_(socket_init(error)) {}
    ~SocketInitGuard() { socket_cleanup(); }
    [[nodiscard]] bool ok() const noexcept { return ok_; }
private:
    bool ok_;
};

// Semantic message payload codecs. Each writes/reads the canonical payload.
// --- registration / handshake ---
void encode_hello(BinaryWriter& w, std::uint32_t protocol_version, std::uint32_t abi_version);
bool decode_hello(BinaryReader& r, std::uint32_t& protocol_version, std::uint32_t& abi_version);
void encode_register(BinaryWriter& w, const WorkerDescriptor& wd);
bool decode_register(BinaryReader& r, WorkerDescriptor& wd);
void encode_register_ack(BinaryWriter& w, bool accepted, CoordinatorEpoch epoch, std::string msg);
bool decode_register_ack(BinaryReader& r, bool& accepted, CoordinatorEpoch& epoch, std::string& msg);

// --- admission ---
void encode_admit(BinaryWriter& w, const RequestDescriptor& d);
bool decode_admit(BinaryReader& r, RequestDescriptor& d);
void encode_admit_ack(BinaryWriter& w, bool accepted, RejectionCode code, RequestId id, AttemptId attempt, Generation gen, CoordinatorEpoch epoch, std::string msg);
bool decode_admit_ack(BinaryReader& r, bool& accepted, RejectionCode& code, RequestId& id, AttemptId& attempt, Generation& gen, CoordinatorEpoch& epoch, std::string& msg);

// --- observation / completion ---
void encode_observation(BinaryWriter& w, const Observation& o);
bool decode_observation(BinaryReader& r, Observation& o);
void encode_observation_ack(BinaryWriter& w, bool accepted, RejectionCode code);
bool decode_observation_ack(BinaryReader& r, bool& accepted, RejectionCode& code);
void encode_completion(BinaryWriter& w, const Completion& c);
bool decode_completion(BinaryReader& r, Completion& c);

// --- intervention ---
void encode_intervention(BinaryWriter& w, const Intervention& iv);
bool decode_intervention(BinaryReader& r, Intervention& iv);
void encode_intervention_plan(BinaryWriter& w, const InterventionPlan& p);
bool decode_intervention_plan(BinaryReader& r, InterventionPlan& p);

// --- snapshot / query ---
void encode_snapshot(BinaryWriter& w, const Snapshot& s);
bool decode_snapshot(BinaryReader& r, Snapshot& s);

// --- error ---
void encode_error(BinaryWriter& w, RejectionCode code, std::string msg);
bool decode_error(BinaryReader& r, RejectionCode& code, std::string& msg);

} // namespace net
} // namespace latency_governor