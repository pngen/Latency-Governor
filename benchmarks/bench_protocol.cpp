// bench_protocol.cpp
// Measures serialization/deserialization throughput of the canonical wire format
// with BinaryWriter/BinaryReader (a realistic framed-payload round trip).
//
// Links: latency_governor (core only).
#include "latency_governor/serialization.hpp"

#include <cstdio>
#include <string>
#include <string_view>

using namespace latency_governor;

int main() {
    const std::uint64_t iterations = 1000000;
    const std::string key = "PREFILL";

    const TimePoint t0 = mono_now();
    std::uint64_t good = 0;
    for (std::uint64_t i = 0; i < iterations; ++i) {
        BinaryWriter w;
        w.u32(kProtocolVersion);
        w.u32(1u);                       // abi_version
        w.u64(12345u);                   // request_id payload
        w.duration(ms(42));              // elapsed
        w.string(key);
        w.bool_(true);                   // optional present flag
        w.enums_raw(2u);                 // a small enum payload

        const std::string_view payload = w.data();
        BinaryReader r(payload);
        std::uint32_t pv = 0, abi = 0, en = 0;
        std::uint64_t rid = 0;
        Duration d{0};
        std::string s;
        bool present = false;
        if (r.u32(pv) && r.u32(abi) && r.u64(rid) && r.duration(d) && r.string(s) && r.bool_(present) &&
            r.enums_raw(en) && r.at_end()) {
            ++good;
        }
    }
    const TimePoint t1 = mono_now();
    const double secs = static_cast<double>(ns_count(saturating_elapsed(t0, t1))) / 1e9;
    const double ops_per_s = (secs > 0.0) ? static_cast<double>(iterations) / secs : 0.0;

    std::printf("protocol round-trip ops=%llu good=%llu elapsed_ms=%.1f ops/s=%.0f completed=yes\n",
                (unsigned long long)iterations, (unsigned long long)good, secs * 1e3, ops_per_s);
    std::printf("  serialize+deserialize a canonical framed payload (u32/u64/duration/string/bool/enum).\n");

    std::printf("\n[bench_protocol] summary: BinaryWriter/Reader round trips fully completed and timed.\n");
    return good == iterations ? 0 : 1;
}
