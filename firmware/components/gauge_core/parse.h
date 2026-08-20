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

// Parse a mode-09 (vehicle info) reply, returning its payload bytes.
// Answers are multi-frame: the VIN arrives as several `49 02 <n>` lines.
// Frame counters and ISO-TP padding are dropped here - any byte below 0x20
// cannot be part of a printable field.
std::optional<Bytes> parse_mode09(const std::string& text, uint8_t pid);

// "13.8V" -> 13.8 (the ATRV reply), else nullopt.
std::optional<double> parse_voltage(const std::string& text);

}  // namespace gauge
