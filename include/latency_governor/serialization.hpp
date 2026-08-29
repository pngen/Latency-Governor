#pragma once

#include "latency_governor/clock.hpp"
#include "latency_governor/duration.hpp"
#include "latency_governor/identifiers.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace latency_governor {

// Wire/persistence format constants. The binary format is canonical byte order
// (big-endian), fixed-width, and versioned. Raw pod structs are never written
// directly; every field is encoded individually so the format is stable across
// compiler/platform ABI differences.
//
// Protocol framing version (matches RuntimeVersion::protocol_version).
constexpr std::uint32_t kProtocolVersion = 1;
// Persistence format version (matches RuntimeVersion::persistence_version).
constexpr std::uint32_t kPersistenceVersion = 1;
// Framing magic for the TCP protocol.
constexpr std::uint32_t kFrameMagic = 0x4C474252;  // "LGBR"
// Persistence magic.
constexpr std::uint64_t kPersistenceMagic = 0x4C4154454E434A41ULL;  // "LATENCJA"

// A maximally-bounded serialized frame (protocol or persistence record).
constexpr std::size_t kMaxFrameSize = 16 * 1024 * 1024;   // 16 MiB
constexpr std::size_t kMaxStringSize = 1 * 1024 * 1024;   // 1 MiB

// FNV-1a 64-bit checksum (tag = domain). Used as a tamper/truncation detector;
// it is not cryptographic, but it detects corruption.
[[nodiscard]] std::uint64_t fnv1a(std::string_view data, std::uint64_t seed = 1469598103934665603ULL) noexcept;

// ---------------------------------------------------------------------------
// BinaryWriter: canonical big-endian fixed-width writer.
// ---------------------------------------------------------------------------
class BinaryWriter {
public:
    void u8(std::uint8_t v);
    void u16(std::uint16_t v);
    void u32(std::uint32_t v);
    void u64(std::uint64_t v);
    void i64(std::int64_t v);
    void bytes(const void* p, std::size_t n);
    void string(std::string_view s);           // u32 length + bytes
    void bool_(bool b);
    void duration(Duration d);                 // i64 ns
    void time_ns(std::int64_t ns);             // i64 ns
    void enums_raw(std::uint32_t v);           // enum as u32 (range-checked by caller/decoder)

    template <typename Tag, typename Rep>
    void id(const StrongId<Tag, Rep>& id) { u64(static_cast<std::uint64_t>(id.value())); }

    [[nodiscard]] const std::string& data() const noexcept { return buf_; }
    [[nodiscard]] std::size_t size() const noexcept { return buf_.size(); }

private:
    std::string buf_;
};

// ---------------------------------------------------------------------------
// BinaryReader: bounds-checked big-endian reader. All reads verify that the
// remaining buffer is large enough; a malformed/truncated input never reads out
// of bounds and always fails cleanly (returns false).
// ---------------------------------------------------------------------------
class BinaryReader {
public:
    explicit BinaryReader(std::string_view buf) : buf_(buf), pos_(0) {}

    bool u8(std::uint8_t& v);
    bool u16(std::uint16_t& v);
    bool u32(std::uint32_t& v);
    bool u64(std::uint64_t& v);
    bool i64(std::int64_t& v);
    bool bytes(void* p, std::size_t n);
    bool string(std::string& s, std::size_t max_len = kMaxStringSize);
    bool bool_(bool& b);
    bool duration(Duration& d);
    bool time_ns(std::int64_t& ns);
    bool enums_raw(std::uint32_t& v);
    // Take a bounded view of the next n bytes (recording the consumed length).
    bool take(std::size_t n, std::string_view& out);

    template <typename Tag, typename Rep>
    bool id(StrongId<Tag, Rep>& out) { std::uint64_t v; if (!u64(v)) return false; out = StrongId<Tag, Rep>(static_cast<Rep>(v)); return true; }

    [[nodiscard]] std::size_t remaining() const noexcept { return buf_.size() - pos_; }
    [[nodiscard]] bool at_end() const noexcept { return pos_ >= buf_.size(); }
    [[nodiscard]] std::size_t position() const noexcept { return pos_; }

private:
    std::string_view buf_;
    std::size_t pos_;
};

// Read/write an optional string-field-free duration as a flag + duration.
template <typename Reader>
bool read_optional_duration(Reader& r, std::optional<Duration>& out) {
    bool present; if (!r.bool_(present)) return false;
    out.reset();
    if (present) { Duration d; if (!r.duration(d)) return false; out = d; }
    return true;
}

template <typename Writer>
void write_optional_duration(Writer& w, const std::optional<Duration>& v) {
    w.bool_(v.has_value());
    if (v) w.duration(*v);
}

} // namespace latency_governor