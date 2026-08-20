#include "elm327.h"
#include <algorithm>
#include <cstdio>
#include "poll.h"
#include "vehicle.h"

namespace gauge {
namespace {

std::string upper_copy(const std::string& s) {
    std::string o = s;
    std::transform(o.begin(), o.end(), o.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return o;
}

std::string pid_cmd(uint8_t mode, uint8_t pid) {
    char b[8];
    snprintf(b, sizeof b, "%02X%02X", mode, pid);
    return b;
}

}  // namespace

std::string Elm327::cmd(const std::string& text, int timeout_ms) {
    if (!t_.write(text)) return "";
    std::string reply = t_.read(timeout_ms);
    // Drop the prompt, then split on CR/LF and discard the echoed command
    // if echo is still on.
    std::string out;
    std::string line;
    const std::string want = upper_copy(text);
    auto flush = [&]() {
        // trim
        size_t a = line.find_first_not_of(" \t");
        size_t b = line.find_last_not_of(" \t");
        if (a != std::string::npos) {
            std::string tok = line.substr(a, b - a + 1);
            if (!tok.empty() && upper_copy(tok) != want) {
                if (!out.empty()) out += " ";
                out += tok;
            }
        }
        line.clear();
    };
    for (char c : reply) {
        if (c == '>') continue;
        if (c == '\r' || c == '\n') { flush(); continue; }
        line += c;
    }
    flush();
    return out;
}

bool Elm327::init() {
    const std::pair<const char*, int> seq[] = {
        {"ATZ", 2000}, {"ATE0", 1000}, {"ATL0", 1000},
        {"ATS0", 1000}, {"ATH0", 1000}, {"ATSP0", 1000}};
    bool any = false;
    for (const auto& s : seq) {
        std::string r = cmd(s.first, s.second + 3000);
        if (!r.empty()) any = true;
        t_.delay_ms(150);
    }
    return any;
}

std::set<uint8_t> Elm327::discover() {
    std::set<uint8_t> supported;
    for (uint8_t base : {0x00, 0x20, 0x40, 0x60}) {
        std::string reply = cmd(pid_cmd(0x01, base));
        auto data = parse_mode01(reply, base);
        if (!data) break;
        auto found = parse_supported(*data, base);
        supported.insert(found.begin(), found.end());
        // bit 32 of each block indicates "next block supported"
        if (!found.count(static_cast<uint8_t>(base + 0x20))) break;
    }
    return supported;
}

std::optional<Bytes> Elm327::request(uint8_t pid) {
    return parse_mode01(cmd(pid_cmd(0x01, pid)), pid);
}

std::optional<double> Elm327::read_voltage() {
    return parse_voltage(cmd("ATRV", 1500));
}

std::string Elm327::read_vin() {
    auto payload = parse_mode09(cmd(pid_cmd(0x09, 0x02)), 0x02);
    if (!payload) return "";
    return clean_vin(std::string(payload->begin(), payload->end()));
}

}  // namespace gauge
