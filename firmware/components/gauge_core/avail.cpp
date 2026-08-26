#include "avail.h"

namespace gauge {
namespace {

// Trim ASCII spaces from both ends, so " rpm , speed " lists the same two
// channels as "rpm,speed". Untrimmed keys would never match and would make a
// working view look unavailable.
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
    return s.substr(a, b - a);
}

}  // namespace

bool view_available(const char* needs, const std::set<std::string>* supported) {
    if (!supported) return true;          // car not identified yet
    if (!needs || !*needs) return true;   // view does not depend on the car

    bool any_key = false;
    std::string s(needs);
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t comma = s.find(',', pos);
        if (comma == std::string::npos) comma = s.size();
        const std::string key = trim(s.substr(pos, comma - pos));
        if (!key.empty()) {
            any_key = true;
            if (supported->count(key)) return true;
        }
        pos = comma + 1;
    }
    // A list that held only separators names no channel, which is the same as
    // naming none at all -- not a view that can never be shown.
    return !any_key;
}

}  // namespace gauge
