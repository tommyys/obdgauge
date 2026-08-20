#include "parse.h"
#include <cstdlib>

namespace gauge {

Bytes hex_bytes(const std::string& text) {
    Bytes out;
    int hi = -1;
    for (char ch : text) {
        int v;
        if (ch >= '0' && ch <= '9')      v = ch - '0';
        else if (ch >= 'a' && ch <= 'f') v = ch - 'a' + 10;
        else if (ch >= 'A' && ch <= 'F') v = ch - 'A' + 10;
        else { hi = -1; continue; }   // non-hex resets any half byte
        if (hi < 0) hi = v;
        else { out.push_back(static_cast<uint8_t>((hi << 4) | v)); hi = -1; }
    }
    return out;
}

std::optional<Bytes> parse_mode01(const std::string& text, uint8_t pid) {
    Bytes b = hex_bytes(text);
    if (b.size() < 2) return std::nullopt;
    for (size_t i = 0; i + 1 < b.size(); ++i) {
        if (b[i] == 0x41 && b[i + 1] == pid) {
            return Bytes(b.begin() + static_cast<long>(i) + 2, b.end());
        }
    }
    return std::nullopt;
}

std::optional<Bytes> parse_mode09(const std::string& text, uint8_t pid) {
    Bytes b = hex_bytes(text);
    Bytes out;
    size_t i = 0, n = b.size();
    bool seen_header = false;
    while (i + 1 < n) {
        if (b[i] == 0x49 && b[i + 1] == pid) { seen_header = true; i += 2; continue; }
        if (seen_header && b[i] >= 0x20) out.push_back(b[i]);
        ++i;
    }
    if (seen_header && i < n && b[i] >= 0x20) out.push_back(b[i]);
    if (!seen_header) return std::nullopt;
    return out;
}

std::optional<double> parse_voltage(const std::string& text) {
    size_t i = 0;
    while (i < text.size() && !(text[i] >= '0' && text[i] <= '9')) ++i;
    if (i == text.size()) return std::nullopt;
    size_t j = i;
    while (j < text.size() && ((text[j] >= '0' && text[j] <= '9') || text[j] == '.')) ++j;
    if (j >= text.size() || (text[j] != 'V' && text[j] != 'v')) return std::nullopt;
    return std::strtod(text.substr(i, j - i).c_str(), nullptr);
}

}  // namespace gauge
