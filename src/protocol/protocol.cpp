#include "latency_governor/protocol.hpp"

// Windows: winsock2.h must precede windows.h; we avoid windows.h entirely.
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#endif

#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>

namespace latency_governor {
namespace net {

#ifdef _WIN32
using os_socket = SOCKET;
constexpr os_socket kInvalidOsSocket = INVALID_SOCKET;
#define LG_GETSOCKERR WSAGetLastError()
#define LG_CLOSESOCK closesocket
#else
using os_socket = int;
constexpr os_socket kInvalidOsSocket = -1;
#define LG_GETSOCKERR errno
#define LG_CLOSESOCK close
#endif

namespace {
os_socket from_handle(SocketHandle h) { return static_cast<os_socket>(h); }
SocketHandle to_handle(os_socket s) { return static_cast<SocketHandle>(s); }
void write_double_local(BinaryWriter& w, double v) { w.u64(std::bit_cast<std::uint64_t>(v)); }
bool read_double_local(BinaryReader& r, double& v) { std::uint64_t u; if (!r.u64(u)) return false; v = std::bit_cast<double>(u); return std::isfinite(v); }
} // namespace

bool socket_init(std::string& error) {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    if (wsa.wVersion != MAKEWORD(2, 2)) { error = "Winsock 2.2 unavailable"; return false; }
#endif
    error.clear();
    return true;
}

void socket_cleanup() {
#ifdef _WIN32
    WSACleanup();
#endif
}

static bool send_all(os_socket s, const char* data, std::size_t n, std::string& error) {
    std::size_t off = 0;
    while (off < n) {
        const int chunk = static_cast<int>(std::min<std::size_t>(n - off, 1u << 30));
#ifdef _WIN32
        const int sent = ::send(s, data + off, chunk, 0);
#else
        const ssize_t sent = ::send(s, data + off, chunk, MSG_NOSIGNAL);
#endif
        if (sent <= 0) {
            if (sent < 0 && (LG_GETSOCKERR == EINTR)) continue;
            error = "socket send failed";
            return false;
        }
        off += static_cast<std::size_t>(sent);
    }
    return true;
}

static bool recv_exact(os_socket s, char* data, std::size_t n, std::string& error) {
    std::size_t off = 0;
    while (off < n) {
        const int chunk = static_cast<int>(std::min<std::size_t>(n - off, 1u << 30));
        const int got = ::recv(s, data + off, chunk, 0);
        if (got == 0) { error = "peer closed"; return false; }
        if (got < 0) {
            if (LG_GETSOCKERR == EINTR) continue;
            error = "socket recv failed";
            return false;
        }
        off += static_cast<std::size_t>(got);
    }
    return true;
}

bool tcp_listen(const std::string& host, std::uint16_t port, SocketHandle& out_handle, std::string& error) {
    os_socket s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == kInvalidOsSocket) { error = "socket() failed"; return false; }
    int one = 1; ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (host.empty() || host == "localhost" || host == "0.0.0.0") addr.sin_addr.s_addr = htonl(INADDR_ANY);
    else if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) { LG_CLOSESOCK(s); error = "bad host"; return false; }
    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) { LG_CLOSESOCK(s); error = "bind failed"; return false; }
    if (::listen(s, SOMAXCONN) != 0) { LG_CLOSESOCK(s); error = "listen failed"; return false; }
    out_handle = to_handle(s);
    error.clear();
    return true;
}

bool tcp_accept(SocketHandle listen, SocketHandle& out_conn, std::string& error) {
    os_socket l = from_handle(listen);
    sockaddr_in peer{}; int len = sizeof(peer);
    os_socket c = ::accept(l, reinterpret_cast<sockaddr*>(&peer), &len);
    if (c == kInvalidOsSocket) { error = "accept failed"; return false; }
    out_conn = to_handle(c);
    error.clear();
    return true;
}

bool tcp_connect(const std::string& host, std::uint16_t port, SocketHandle& out_sock, std::string& error) {
    os_socket s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == kInvalidOsSocket) { error = "socket() failed"; return false; }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (host.empty() || host == "localhost") addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    else if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) { LG_CLOSESOCK(s); error = "bad host"; return false; }
    if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) { LG_CLOSESOCK(s); error = "connect failed"; return false; }
    out_sock = to_handle(s);
    error.clear();
    return true;
}

void tcp_close(SocketHandle sock) { if (sock != 0) LG_CLOSESOCK(from_handle(sock)); }

void tcp_set_nonblocking(SocketHandle sock, bool enabled) {
#ifdef _WIN32
    u_long mode = enabled ? 1 : 0; ioctlsocket(from_handle(sock), FIONBIO, &mode);
#else
    int flags = fcntl(from_handle(sock), F_GETFL, 0);
    fcntl(from_handle(sock), F_SETFL, enabled ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
#endif
}

static void u32put(std::string& b, std::uint32_t v) {
    for (int s = 24; s >= 0; s -= 8) b.push_back(static_cast<char>((v >> s) & 0xFF));
}

bool send_frame(SocketHandle sock, const Frame& f, std::string& error) {
    os_socket s = from_handle(sock);
    if (f.payload.size() > kMaxFramePayload) { error = "frame too large"; return false; }
    std::string hdr;
    hdr.reserve(kFrameHeaderSize);
    u32put(hdr, f.magic);
    u32put(hdr, f.version);
    u32put(hdr, static_cast<std::uint32_t>(f.type));
    u32put(hdr, static_cast<std::uint32_t>(f.payload.size()));
    if (!send_all(s, hdr.data(), hdr.size(), error)) return false;
    if (!f.payload.empty() && !send_all(s, f.payload.data(), f.payload.size(), error)) return false;
    return true;
}

bool recv_frame(SocketHandle sock, Frame& f, std::string& error) {
    os_socket s = from_handle(sock);
    std::string hdr(kFrameHeaderSize, '\0');
    if (!recv_exact(s, hdr.data(), hdr.size(), error)) return false;
    BinaryReader r(hdr);
    std::uint32_t magic, version, type, len;
    if (!r.u32(magic) || !r.u32(version) || !r.u32(type) || !r.u32(len)) { error = "bad frame header"; return false; }
    if (magic != kFrameMagic) { error = "bad frame magic"; return false; }
    if (version != kProtocolVersion) { error = "unsupported protocol version"; return false; }
    if (len > kMaxFramePayload) { error = "frame payload too large"; return false; }
    BinaryReader r2(hdr); (void)r2;
    f.magic = magic; f.version = version; f.type = static_cast<MessageType>(type);
    if (static_cast<std::size_t>(type) >= MessageType_count) { error = "unknown message type"; return false; }
    f.payload.resize(len);
    if (len > 0) { if (!recv_exact(s, f.payload.data(), len, error)) return false; }
    return true;
}

// ---------------------------------------------------------------------------
// Message codecs.
// ---------------------------------------------------------------------------
void encode_hello(BinaryWriter& w, std::uint32_t protocol_version, std::uint32_t abi_version) {
    w.u32(protocol_version); w.u32(abi_version);
}
bool decode_hello(BinaryReader& r, std::uint32_t& protocol_version, std::uint32_t& abi_version) {
    return r.u32(protocol_version) && r.u32(abi_version);
}

void encode_register(BinaryWriter& w, const WorkerDescriptor& wd) {
    w.id(wd.id); w.id(wd.boot_id); w.string(wd.host); w.u16(wd.port);
    w.enums_raw(static_cast<std::uint32_t>(wd.capabilities.device)); w.bool_(wd.capabilities.accelerator);
    w.string(wd.capabilities.backend_name); w.string(wd.capabilities.backend_version);
    w.u64(wd.capabilities.total_memory); w.u64(wd.capabilities.free_memory);
    w.u32(static_cast<std::uint32_t>(wd.capabilities.supported_ops.size()));
    for (const auto& op : wd.capabilities.supported_ops) w.string(op);
    w.u32(wd.protocol_version); w.u32(wd.generation);
}
bool decode_register(BinaryReader& r, WorkerDescriptor& wd) {
    if (!r.id(wd.id) || !r.id(wd.boot_id) || !r.string(wd.host)) return false;
    if (!r.u16(wd.port)) return false;
    std::uint32_t dev; if (!r.enums_raw(dev) || dev >= DeviceClass_count) return false;
    wd.capabilities.device = static_cast<DeviceClass>(dev);
    if (!r.bool_(wd.capabilities.accelerator)) return false;
    if (!r.string(wd.capabilities.backend_name) || !r.string(wd.capabilities.backend_version)) return false;
    if (!r.u64(wd.capabilities.total_memory) || !r.u64(wd.capabilities.free_memory)) return false;
    std::uint32_t n; if (!r.u32(n) || n > 4096) return false;
    wd.capabilities.supported_ops.clear();
    for (std::uint32_t i = 0; i < n; ++i) { std::string op; if (!r.string(op)) return false; wd.capabilities.supported_ops.push_back(op); }
    if (!r.u32(wd.protocol_version) || !r.u32(wd.generation)) return false;
    return true;
}

void encode_register_ack(BinaryWriter& w, bool accepted, CoordinatorEpoch epoch, std::string msg) {
    w.bool_(accepted); w.id(epoch); w.string(msg);
}
bool decode_register_ack(BinaryReader& r, bool& accepted, CoordinatorEpoch& epoch, std::string& msg) {
    return r.bool_(accepted) && r.id(epoch) && r.string(msg);
}

void encode_admit(BinaryWriter& w, const RequestDescriptor& d) {
    w.id(d.request_id); w.id(d.tenant_id); w.id(d.model_id); w.id(d.model_revision);
    w.bool_(d.adapter_id.has_value()); if (d.adapter_id) w.id(*d.adapter_id);
    w.enums_raw(static_cast<std::uint32_t>(d.slo_class));
    w.u32(d.prompt_tokens); w.u32(d.max_tokens); w.u32(d.remaining_tokens);
    w.bool_(d.backend_hint.has_value()); if (d.backend_hint) w.id(*d.backend_hint);
    w.enums_raw(static_cast<std::uint32_t>(d.device_hint)); w.bool_(d.warm_cache_hint);
}
bool decode_admit(BinaryReader& r, RequestDescriptor& d) {
    if (!r.id(d.request_id) || !r.id(d.tenant_id) || !r.id(d.model_id) || !r.id(d.model_revision)) return false;
    bool has; if (!r.bool_(has)) return false; d.adapter_id.reset(); if (has) { AdapterId a; if (!r.id(a)) return false; d.adapter_id = a; }
    std::uint32_t sc; if (!r.enums_raw(sc) || sc >= SloClass_count) return false; d.slo_class = static_cast<SloClass>(sc);
    if (!r.u32(d.prompt_tokens) || !r.u32(d.max_tokens) || !r.u32(d.remaining_tokens)) return false;
    if (!r.bool_(has)) return false; d.backend_hint.reset(); if (has) { BackendId b; if (!r.id(b)) return false; d.backend_hint = b; }
    std::uint32_t dh; if (!r.enums_raw(dh) || dh >= DeviceClass_count) return false; d.device_hint = static_cast<DeviceClass>(dh);
    return r.bool_(d.warm_cache_hint);
}

void encode_admit_ack(BinaryWriter& w, bool accepted, RejectionCode code, RequestId id, AttemptId attempt, Generation gen, CoordinatorEpoch epoch, std::string msg) {
    w.bool_(accepted); w.enums_raw(static_cast<std::uint32_t>(code)); w.id(id); w.id(attempt); w.id(gen); w.id(epoch); w.string(msg);
}
bool decode_admit_ack(BinaryReader& r, bool& accepted, RejectionCode& code, RequestId& id, AttemptId& attempt, Generation& gen, CoordinatorEpoch& epoch, std::string& msg) {
    std::uint32_t rc; if (!r.bool_(accepted) || !r.enums_raw(rc) || rc >= RejectionCode_count) return false; code = static_cast<RejectionCode>(rc);
    return r.id(id) && r.id(attempt) && r.id(gen) && r.id(epoch) && r.string(msg);
}

void encode_observation(BinaryWriter& w, const Observation& o) {
    w.id(o.id); w.enums_raw(static_cast<std::uint32_t>(o.type));
    w.id(o.request_id); w.id(o.attempt_id); w.id(o.generation); w.id(o.epoch);
    w.bool_(o.dispatch_id.has_value()); if (o.dispatch_id) w.id(*o.dispatch_id);
    w.bool_(o.worker_id.has_value()); if (o.worker_id) w.id(*o.worker_id);
    w.bool_(o.worker_boot_id.has_value()); if (o.worker_boot_id) w.id(*o.worker_boot_id);
    w.time_ns(o.at.time_since_epoch().count()); w.time_ns(o.phase_start.time_since_epoch().count());
    w.bool_(o.elapsed.has_value()); if (o.elapsed) w.duration(*o.elapsed);
    w.enums_raw(static_cast<std::uint32_t>(o.phase));
    w.bool_(o.value.has_value()); if (o.value) write_double_local(w, *o.value);
    w.bool_(o.bytes.has_value()); if (o.bytes) w.u64(*o.bytes);
    w.u32(o.tokens);
    w.string(o.predictor_key); w.string(o.detail);
}
bool decode_observation(BinaryReader& r, Observation& o) {
    if (!r.id(o.id)) return false;
    std::uint32_t t; if (!r.enums_raw(t) || t >= ObservationType_count) return false; o.type = static_cast<ObservationType>(t);
    if (!r.id(o.request_id) || !r.id(o.attempt_id) || !r.id(o.generation) || !r.id(o.epoch)) return false;
    bool has; if (!r.bool_(has)) return false; o.dispatch_id.reset(); if (has) { DispatchId d; if (!r.id(d)) return false; o.dispatch_id = d; }
    if (!r.bool_(has)) return false; o.worker_id.reset(); if (has) { WorkerId w; if (!r.id(w)) return false; o.worker_id = w; }
    if (!r.bool_(has)) return false; o.worker_boot_id.reset(); if (has) { WorkerBootId w; if (!r.id(w)) return false; o.worker_boot_id = w; }
    std::int64_t ns; if (!r.time_ns(ns)) return false; o.at = TimePoint(std::chrono::nanoseconds(ns));
    if (!r.time_ns(ns)) return false; o.phase_start = TimePoint(std::chrono::nanoseconds(ns));
    if (!r.bool_(has)) return false; o.elapsed.reset(); if (has) { Duration d; if (!r.duration(d)) return false; o.elapsed = d; }
    std::uint32_t ph; if (!r.enums_raw(ph) || ph >= Phase_count) return false; o.phase = static_cast<Phase>(ph);
    if (!r.bool_(has)) return false; o.value.reset(); if (has) { double v; if (!read_double_local(r, v)) return false; o.value = v; }
    if (!r.bool_(has)) return false; o.bytes.reset(); if (has) { std::uint64_t b; if (!r.u64(b)) return false; o.bytes = b; }
    if (!r.u32(o.tokens)) return false;
    return r.string(o.predictor_key) && r.string(o.detail);
}

void encode_observation_ack(BinaryWriter& w, bool accepted, RejectionCode code) { w.bool_(accepted); w.enums_raw(static_cast<std::uint32_t>(code)); }
bool decode_observation_ack(BinaryReader& r, bool& accepted, RejectionCode& code) {
    std::uint32_t rc; if (!r.bool_(accepted) || !r.enums_raw(rc) || rc >= RejectionCode_count) return false; code = static_cast<RejectionCode>(rc); return true;
}

void encode_completion(BinaryWriter& w, const Completion& c) {
    w.id(c.request_id); w.id(c.attempt_id); w.id(c.generation); w.id(c.epoch);
    w.bool_(c.dispatch_id.has_value()); if (c.dispatch_id) w.id(*c.dispatch_id);
    w.bool_(c.worker_id.has_value()); if (c.worker_id) w.id(*c.worker_id);
    w.bool_(c.worker_boot_id.has_value()); if (c.worker_boot_id) w.id(*c.worker_boot_id);
    w.enums_raw(static_cast<std::uint32_t>(c.outcome));
    w.bool_(c.slo_met); w.bool_(c.soft_violation); w.bool_(c.hard_violation);
    w.u32(c.tokens_generated); w.time_ns(c.at.time_since_epoch().count()); w.string(c.detail);
}
bool decode_completion(BinaryReader& r, Completion& c) {
    if (!r.id(c.request_id) || !r.id(c.attempt_id) || !r.id(c.generation) || !r.id(c.epoch)) return false;
    bool has; if (!r.bool_(has)) return false; c.dispatch_id.reset(); if (has) { DispatchId d; if (!r.id(d)) return false; c.dispatch_id = d; }
    if (!r.bool_(has)) return false; c.worker_id.reset(); if (has) { WorkerId w; if (!r.id(w)) return false; c.worker_id = w; }
    if (!r.bool_(has)) return false; c.worker_boot_id.reset(); if (has) { WorkerBootId w; if (!r.id(w)) return false; c.worker_boot_id = w; }
    std::uint32_t oc; if (!r.enums_raw(oc) || oc > 2) return false; c.outcome = static_cast<Completion::Outcome>(oc);
    if (!r.bool_(c.slo_met) || !r.bool_(c.soft_violation) || !r.bool_(c.hard_violation)) return false;
    if (!r.u32(c.tokens_generated)) return false;
    std::int64_t ns; if (!r.time_ns(ns)) return false; c.at = TimePoint(std::chrono::nanoseconds(ns));
    return r.string(c.detail);
}

void encode_intervention(BinaryWriter& w, const Intervention& iv) {
    w.enums_raw(static_cast<std::uint32_t>(iv.action)); w.enums_raw(static_cast<std::uint32_t>(iv.reason));
    w.id(iv.request_id); w.id(iv.attempt_id); w.enums_raw(static_cast<std::uint32_t>(iv.phase));
    w.duration(iv.remaining_budget); w.duration(iv.predicted_remaining); w.enums_raw(static_cast<std::uint32_t>(iv.risk));
    write_double_local(w, iv.threshold); write_double_local(w, iv.observed);
    w.u64(iv.policy_generation); w.u64(iv.decision_generation); w.time_ns(iv.at.time_since_epoch().count());
    w.bool_(iv.target_worker.has_value()); if (iv.target_worker) w.id(*iv.target_worker);
    w.u32(static_cast<std::uint32_t>(iv.supporting_reasons.size()));
    for (auto rc : iv.supporting_reasons) w.enums_raw(static_cast<std::uint32_t>(rc));
    w.string(iv.detail);
}
bool decode_intervention(BinaryReader& r, Intervention& iv) {
    std::uint32_t a, rs, ph, rk;
    if (!r.enums_raw(a) || a >= InterventionAction_count) return false; iv.action = static_cast<InterventionAction>(a);
    if (!r.enums_raw(rs) || rs >= ReasonCode_count) return false; iv.reason = static_cast<ReasonCode>(rs);
    if (!r.id(iv.request_id) || !r.id(iv.attempt_id)) return false;
    if (!r.enums_raw(ph) || ph >= Phase_count) return false; iv.phase = static_cast<Phase>(ph);
    if (!r.duration(iv.remaining_budget) || !r.duration(iv.predicted_remaining)) return false;
    if (!r.enums_raw(rk) || rk >= RiskState_count) return false; iv.risk = static_cast<RiskState>(rk);
    if (!read_double_local(r, iv.threshold) || !read_double_local(r, iv.observed)) return false;
    if (!r.u64(iv.policy_generation) || !r.u64(iv.decision_generation)) return false;
    std::int64_t ns; if (!r.time_ns(ns)) return false; iv.at = TimePoint(std::chrono::nanoseconds(ns));
    bool has; if (!r.bool_(has)) return false; iv.target_worker.reset(); if (has) { WorkerId w; if (!r.id(w)) return false; iv.target_worker = w; }
    std::uint32_t n; if (!r.u32(n) || n > ReasonCode_count) return false; iv.supporting_reasons.clear();
    for (std::uint32_t i = 0; i < n; ++i) { std::uint32_t rc; if (!r.enums_raw(rc) || rc >= ReasonCode_count) return false; iv.supporting_reasons.push_back(static_cast<ReasonCode>(rc)); }
    return r.string(iv.detail);
}

void encode_intervention_plan(BinaryWriter& w, const InterventionPlan& p) {
    w.u64(p.decision_generation);
    w.u32(static_cast<std::uint32_t>(p.items.size()));
    for (const auto& it : p.items) encode_intervention(w, it);
}
bool decode_intervention_plan(BinaryReader& r, InterventionPlan& p) {
    if (!r.u64(p.decision_generation)) return false;
    std::uint32_t n; if (!r.u32(n) || n > 64) return false; p.items.clear();
    for (std::uint32_t i = 0; i < n; ++i) { Intervention it; if (!decode_intervention(r, it)) return false; p.items.push_back(it); }
    return true;
}

void encode_snapshot(BinaryWriter& w, const Snapshot& s) {
    w.time_ns(s.at.time_since_epoch().count()); w.u64(s.coordinator_epoch); w.u64(s.decision_generation); w.u64(s.event_sequence);
    w.u32(static_cast<std::uint32_t>(s.requests.size()));
    for (const auto& q : s.requests) {
        w.id(q.request_id); w.id(q.attempt_id);
        w.enums_raw(static_cast<std::uint32_t>(q.lifecycle)); w.enums_raw(static_cast<std::uint32_t>(q.phase));
        w.enums_raw(static_cast<std::uint32_t>(q.slo_class)); w.id(q.tenant_id); w.enums_raw(static_cast<std::uint32_t>(q.risk));
        w.duration(q.remaining_budget); w.duration(q.elapsed_total); w.u64(q.generation); w.u64(q.epoch);
    }
    w.string(s.json);
}
bool decode_snapshot(BinaryReader& r, Snapshot& s) {
    std::int64_t ns; if (!r.time_ns(ns)) return false; s.at = TimePoint(std::chrono::nanoseconds(ns));
    if (!r.u64(s.coordinator_epoch) || !r.u64(s.decision_generation) || !r.u64(s.event_sequence)) return false;
    std::uint32_t n; if (!r.u32(n) || n > 1'000'000) return false;
    s.requests.clear();
    for (std::uint32_t i = 0; i < n; ++i) {
        RequestSnapshot q;
        if (!r.id(q.request_id) || !r.id(q.attempt_id)) return false;
        std::uint32_t lc, ph, sc, rk; if (!r.enums_raw(lc) || lc >= LifecycleState_count) return false; q.lifecycle = static_cast<LifecycleState>(lc);
        if (!r.enums_raw(ph) || ph >= Phase_count) return false; q.phase = static_cast<Phase>(ph);
        if (!r.enums_raw(sc) || sc >= SloClass_count) return false; q.slo_class = static_cast<SloClass>(sc);
        if (!r.id(q.tenant_id)) return false;
        if (!r.enums_raw(rk) || rk >= RiskState_count) return false; q.risk = static_cast<RiskState>(rk);
        if (!r.duration(q.remaining_budget) || !r.duration(q.elapsed_total)) return false;
        if (!r.u64(q.generation) || !r.u64(q.epoch)) return false;
        s.requests.push_back(q);
    }
    return r.string(s.json);
}

void encode_error(BinaryWriter& w, RejectionCode code, std::string msg) {
    w.enums_raw(static_cast<std::uint32_t>(code)); w.string(msg);
}
bool decode_error(BinaryReader& r, RejectionCode& code, std::string& msg) {
    std::uint32_t rc; if (!r.enums_raw(rc) || rc >= RejectionCode_count) return false; code = static_cast<RejectionCode>(rc);
    return r.string(msg);
}

} // namespace net
} // namespace latency_governor