#pragma once
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace gauge_test {

inline std::vector<std::string>& failures() {
    static std::vector<std::string> f;
    return f;
}

inline std::string show(double v)             { char b[32]; snprintf(b, sizeof b, "%g", v); return b; }
inline std::string show(int v)                { return std::to_string(v); }
inline std::string show(bool v)               { return v ? "true" : "false"; }
inline std::string show(const std::string& v) { return "'" + v + "'"; }

inline std::string show(const std::vector<uint8_t>& v) {
    std::string s = "[";
    for (size_t i = 0; i < v.size(); ++i) {
        char b[16];
        snprintf(b, sizeof b, "%s0x%02X", i ? ", " : "", v[i]);
        s += b;
    }
    return s + "]";
}

template <typename T>
inline std::string show(const std::optional<T>& v) { return v ? show(*v) : "None"; }

template <typename T>
void check(const char* name, const T& got, const T& want) {
    bool ok = (got == want);
    if (!ok) failures().push_back(std::string(name) + ": got " + show(got) + " want " + show(want));
    printf("%-46s %s  (%s)\n", name, ok ? "ok  " : "FAIL", show(got).c_str());
}

inline int check_report() {
    printf("\n");
    if (failures().empty()) { printf("all tests passed\n"); return 0; }
    printf("%zu FAILURES:\n", failures().size());
    for (const auto& f : failures()) printf("  - %s\n", f.c_str());
    return static_cast<int>(failures().size());
}

}  // namespace gauge_test
