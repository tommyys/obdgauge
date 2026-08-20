#include "check.h"
#include "version.h"
using gauge_test::check;

int main() {
    check("harness reports equality", 1 + 1, 2);
    check("optional compares", std::optional<double>{1726.0}, std::optional<double>{1726.0});
    check("nullopt compares", std::optional<double>{}, std::optional<double>{});
    check("core links", std::string(gauge::core_version()), std::string("0.1.0"));
    return gauge_test::check_report();
}
