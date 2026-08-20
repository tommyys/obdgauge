// ELM327 reply parsing. Ported from mx5gauge/pids.py:281-315, 387-395.
#pragma once
#include <optional>
#include <string>
#include "pid.h"

namespace gauge {

// Collect hex byte values from an ELM327 reply, ignoring spaces/prompt.
Bytes hex_bytes(const std::string& text);

// "41 0C 1A F8" -> {0x1A, 0xF8} for pid 0x0C, else nullopt. Tolerates
// multi-line/multi-ECU replies by scanning for `41 <pid>` anywhere.
std::optional<Bytes> parse_mode01(const std::string& text, uint8_t pid);

// "13.8V" -> 13.8 (the ATRV reply), else nullopt.
std::optional<double> parse_voltage(const std::string& text);

}  // namespace gauge
