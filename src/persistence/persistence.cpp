#include "latency_governor/persistence.hpp"

#include "latency_governor/governor.hpp"
#include "latency_governor/serialization.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

namespace latency_governor {

bool persist_to_file(const Governor& g, const std::string& path, std::string& error) {
    std::string err;
    const std::string blob = g.encode_state(err);
    if (!err.empty()) { error = err; return false; }
    const std::string tmp = path + ".tmp";
    {
        std::ofstream os(tmp, std::ios::binary | std::ios::trunc);
        if (!os) { error = "cannot open temp file: " + tmp; return false; }
        os.write(blob.data(), static_cast<std::streamsize>(blob.size()));
        if (!os) { error = "write failed"; return false; }
        os.flush();
        if (!os) { error = "flush failed"; return false; }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) { error = "atomic rename failed: " + ec.message(); return false; }
    error.clear();
    return true;
}

bool load_from_file(Governor& g, const std::string& path, std::string& error) {
    std::ifstream is(path, std::ios::binary);
    if (!is) { error = "cannot open: " + path; return false; }
    std::string blob((std::istreambuf_iterator<char>(is)), std::istreambuf_iterator<char>());
    if (blob.empty()) { error = "empty file"; return false; }
    return g.decode_state(blob, error);
}

} // namespace latency_governor
