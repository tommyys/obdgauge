// PID table, supported-PID bitmask, and the polling rotation.
// Ported from mx5gauge/pids.py:181-280, 345-386. The Python is authoritative;
// the table below is machine-extracted from it, not hand-transcribed.
#pragma once
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>
#include "pid.h"

namespace gauge {

using Decoder = std::optional<double> (*)(const Bytes&);

struct PidInfo {
    const char* key;
    const char* label;
    const char* unit;
    Decoder     decoder;
};

struct Reading {
    std::string key;
    double      value;
};

// nullptr when the PID is not one we can decode.
const PidInfo* pid_info(uint8_t pid);

// Decode a mode-01 payload into its channel key and value.
std::optional<Reading> decode(uint8_t pid, const Bytes& data);

// Channel keys the car can supply, from its supported-PID set. PIDs we
// cannot decode are ignored - the car offering one does us no good if we
// cannot read it.
std::set<std::string> keys_for(const std::set<uint8_t>& supported);

// Bit 7 of byte 0 means base+1; bit 0 of byte 3 means base+32.
std::set<uint8_t> parse_supported(const Bytes& data, uint8_t base);

// log_all=true  -> sweep every supported PID we can decode
// log_all=false -> poll only the display set
// Fast PIDs are interleaved between every slow one so rpm/speed stay live.
std::vector<uint8_t> build_poll_cycle(const std::set<uint8_t>& supported, bool log_all = true);

}  // namespace gauge
