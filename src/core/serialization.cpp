#include "latency_governor/serialization.hpp"

#include <cstring>

namespace latency_governor {

std::uint64_t fnv1a(std::string_view data, std::uint64_t seed) noexcept {
    std::uint64_t h = seed;
    for (unsigned char c : data) {
        h ^= static_cast<std::uint64_t>(c);
        h *= 1099511628211ULL;
    }
    return h;
}

namespace {
void put_be32(std::string& b, std::uint32_t v) {
    b.push_back(static_cast<char>((v >> 24) & 0xFF));
    b.push_back(static_cast<char>((v >> 16) & 0xFF));
    b.push_back(static_cast<char>((v >> 8) & 0xFF));
    b.push_back(static_cast<char>(v & 0xFF));
}
void put_be64(std::string& b, std::uint64_t v) {
    for (int s = 56; s >= 0; s -= 8) b.push_back(static_cast<char>((v >> s) & 0xFF));
}
} // namespace

void BinaryWriter::u8(std::uint8_t v) { buf_.push_back(static_cast<char>(v)); }
void BinaryWriter::u16(std::uint16_t v) { buf_.push_back(static_cast<char>((v >> 8) & 0xFF)); buf_.push_back(static_cast<char>(v & 0xFF)); }
void BinaryWriter::u32(std::uint32_t v) { put_be32(buf_, v); }
void BinaryWriter::u64(std::uint64_t v) { put_be64(buf_, v); }
void BinaryWriter::i64(std::int64_t v) { put_be64(buf_, static_cast<std::uint64_t>(v)); }
void BinaryWriter::bytes(const void* p, std::size_t n) {
    const char* cp = static_cast<const char*>(p);
    buf_.append(cp, n);
}
void BinaryWriter::string(std::string_view s) {
    if (s.size() > kMaxStringSize) s = s.substr(0, kMaxStringSize);
    u32(static_cast<std::uint32_t>(s.size()));
    bytes(s.data(), s.size());
}
void BinaryWriter::bool_(bool b) { u8(b ? 1 : 0); }
void BinaryWriter::duration(Duration d) { i64(d.count()); }
void BinaryWriter::time_ns(std::int64_t ns) { i64(ns); }
void BinaryWriter::enums_raw(std::uint32_t v) { u32(v); }

bool BinaryReader::u8(std::uint8_t& v) {
    if (remaining() < 1) return false;
    v = static_cast<std::uint8_t>(buf_[pos_++]);
    return true;
}
bool BinaryReader::u16(std::uint16_t& v) {
    if (remaining() < 2) return false;
    v = static_cast<std::uint16_t>((static_cast<std::uint8_t>(buf_[pos_]) << 8) | static_cast<std::uint8_t>(buf_[pos_ + 1]));
    pos_ += 2; return true;
}
bool BinaryReader::u32(std::uint32_t& v) {
    if (remaining() < 4) return false;
    v = (static_cast<std::uint32_t>(static_cast<std::uint8_t>(buf_[pos_])) << 24)
      | (static_cast<std::uint32_t>(static_cast<std::uint8_t>(buf_[pos_ + 1])) << 16)
      | (static_cast<std::uint32_t>(static_cast<std::uint8_t>(buf_[pos_ + 2])) << 8)
      | static_cast<std::uint32_t>(static_cast<std::uint8_t>(buf_[pos_ + 3]));
    pos_ += 4; return true;
}
bool BinaryReader::u64(std::uint64_t& v) {
    if (remaining() < 8) return false;
    v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | static_cast<std::uint8_t>(buf_[pos_ + i]);
    pos_ += 8; return true;
}
bool BinaryReader::i64(std::int64_t& v) {
    std::uint64_t u; if (!u64(u)) return false;
    v = static_cast<std::int64_t>(u); return true;
}
bool BinaryReader::bytes(void* p, std::size_t n) {
    if (remaining() < n) return false;
    std::memcpy(p, buf_.data() + pos_, n); pos_ += n; return true;
}
bool BinaryReader::string(std::string& s, std::size_t max_len) {
    std::uint32_t len; if (!u32(len)) return false;
    if (len > max_len) return false;
    if (remaining() < len) return false;
    s.assign(buf_.data() + pos_, len); pos_ += len; return true;
}
bool BinaryReader::bool_(bool& b) { std::uint8_t v; if (!u8(v)) return false; if (v > 1) return false; b = (v == 1); return true; }
bool BinaryReader::duration(Duration& d) { std::int64_t n; if (!i64(n)) return false; d = Duration(n); return true; }
bool BinaryReader::time_ns(std::int64_t& ns) { return i64(ns); }
bool BinaryReader::enums_raw(std::uint32_t& v) { return u32(v); }
bool BinaryReader::take(std::size_t n, std::string_view& out) {
    if (remaining() < n) return false;
    out = std::string_view(buf_.data() + pos_, n);
    pos_ += n;
    return true;
}

} // namespace latency_governor