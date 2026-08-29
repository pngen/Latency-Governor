#pragma once

#include <string>
#include <string_view>

namespace latency_governor {

// Persistence of governor state uses an explicit versioned binary format with a
// checksum and atomic (temp -> flush -> close -> rename) replacement. Recovery
// rejects corruption, truncation, impossible lengths, unsupported versions,
// malformed enums, duplicate identities, inconsistent accounting, and
// impossible state transitions.

class Governor;

// Atomically persist governor state to a file: temp file -> flush -> close ->
// atomic rename. Never treats an incomplete write as durable.
bool persist_to_file(const Governor& g, const std::string& path, std::string& error);

// Load governor state from a file, validating size, magic, version, and
// checksum.
bool load_from_file(Governor& g, const std::string& path, std::string& error);

} // namespace latency_governor
