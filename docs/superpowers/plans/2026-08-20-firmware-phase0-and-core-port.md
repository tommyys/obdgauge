# Firmware Phase 0 + `gauge_core` Port Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Port the simulator's pure logic to host-tested C++, and prove every subsystem of the ESP32-S3-Touch-AMOLED-1.75C works, so that Phase 1 has a verified foundation and real measurements to design against.

**Architecture:** A new `firmware/` tree. `gauge_core` is dependency-free C++ that compiles for both the Mac and the board — the Python tests in `tests/` become C++ tests that run in CI with no hardware. `gauge_platform` and `gauge_ui` wrap ESP-IDF and LVGL and are proven by bring-up, not unit tests.

**Tech Stack:** ESP-IDF v5.5.x, C++17, CMake, LVGL 9, Waveshare `esp32_s3_touch_amoled_1_75c` BSP, NimBLE, CO5300 (QSPI), CST9217 touch, QMI8658 IMU.

**Spec:** `docs/superpowers/specs/2026-08-20-firmware-port-amoled-1-75c-design.md`

## Global Constraints

- **`gauge_core` may not include any ESP-IDF or LVGL header.** Plain C++17 over plain structs. Enforced by Task 1's host build, which has neither on its include path.
- **C++17**, no exceptions in `gauge_core` (return `std::optional`, matching the Python `None`).
- **The host tests build with `make`, not CMake.** Decided during execution on 2026-08-20:
  `cmake` is not installed on this Mac, `gauge_core` is dependency-free C++17, and
  ESP-IDF ships its own cmake/ninja — so a system CMake is never actually required.
  `firmware/test/host/Makefile` auto-discovers `test_*.cpp`. The per-component
  `CMakeLists.txt` files are still written, because ESP-IDF needs them.
  Where a task below says `cmake -B build . && ctest`, run `make test` instead.
- **Zero third-party test framework.** The repo has exactly one dependency (`bleak`); host tests use a 30-line `check.h`, mirroring the existing `check()` in `tests/test_pids.py`.
- **ESP-IDF pinned to v5.5.x** — matches `v5.5.2` that the board's own firmware was built with.
- **Panel resolution is unconfirmed.** 466×466 is the working assumption. It is defined **once**, in `gauge_ui/display_config.h`, and nothing else hardcodes it. Task 12 confirms it.
- **Where a C++ test disagrees with its Python ancestor, the Python is right** until proven otherwise. The Python suite is the specification being ported.
- **Never run `espefuse burn_*`, and never enable flash encryption or secure boot.** These are the only irreversible actions available and nothing here needs them.
- **The board is not blank.** It runs Xiaozhi. Before the first flash, verify the backup: `shasum -a 256 -c backups/esp32s3-full-32MB.bin.sha256`. Restore steps are in `backups/RESTORE.md`.

---

## Part A — host-side core port (no hardware required)

Tasks 1–9 run entirely on the Mac. They need no board, no toolchain, and nothing flashed.

---

### Task 1: Host test harness and `gauge_core` skeleton

**Files:**
- Create: `firmware/components/gauge_core/CMakeLists.txt`
- Create: `firmware/test/host/CMakeLists.txt`
- Create: `firmware/test/host/check.h`
- Create: `firmware/test/host/test_smoke.cpp`

**Interfaces:**
- Consumes: nothing
- Produces: `check(name, got, want)` returning void and recording failures; `check_report()` returning `int` (0 = all passed, non-zero = failure count) for use as a `main` return value.

- [x] **Step 1: Write the failing test**

`firmware/test/host/check.h`:

```cpp
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

inline std::string show(double v)            { char b[32]; snprintf(b, sizeof b, "%g", v); return b; }
inline std::string show(int v)               { return std::to_string(v); }
inline std::string show(bool v)              { return v ? "true" : "false"; }
inline std::string show(const std::string& v){ return "'" + v + "'"; }
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
```

`firmware/test/host/test_smoke.cpp`:

```cpp
#include "check.h"
using gauge_test::check;

int main() {
    check("harness reports equality", 1 + 1, 2);
    check("optional compares", std::optional<double>{1726.0}, std::optional<double>{1726.0});
    check("nullopt compares", std::optional<double>{}, std::optional<double>{});
    return gauge_test::check_report();
}
```

`firmware/test/host/CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.16)
project(gauge_core_host_tests CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
enable_testing()

file(GLOB CORE_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../components/gauge_core/*.cpp)
add_library(gauge_core ${CORE_SRC})
target_include_directories(gauge_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../../components/gauge_core)

foreach(t smoke)
    add_executable(test_${t} test_${t}.cpp)
    target_link_libraries(test_${t} PRIVATE gauge_core)
    target_include_directories(test_${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME ${t} COMMAND test_${t})
endforeach()
```

Note `file(GLOB ...)` may match nothing on this first task; add a placeholder-free guard by creating the component with a real file in Step 3.

- [x] **Step 2: Run test to verify it fails**

Run: `cd firmware/test/host && cmake -B build . && cmake --build build`
Expected: FAIL — `add_library` errors with "no sources given to target gauge_core", because `gauge_core` has no `.cpp` yet.

- [x] **Step 3: Write minimal implementation**

`firmware/components/gauge_core/version.h`:

```cpp
#pragma once
namespace gauge { const char* core_version(); }
```

`firmware/components/gauge_core/version.cpp`:

```cpp
#include "version.h"
namespace gauge { const char* core_version() { return "0.1.0"; } }
```

`firmware/components/gauge_core/CMakeLists.txt` (used by ESP-IDF later, ignored by the host build):

```cmake
idf_component_register(SRCS "version.cpp"
                       INCLUDE_DIRS ".")
```

- [x] **Step 4: Run test to verify it passes**

Run: `cd firmware/test/host && cmake -B build . && cmake --build build && ctest --test-dir build --output-on-failure`
Expected: PASS — "all tests passed".

- [x] **Step 5: Commit**

```bash
git add firmware/
git commit -m "firmware: host test harness and gauge_core skeleton"
```

---

### Task 2: PID decode formulas

Ports `mx5gauge/pids.py:12-178`. Every assertion below is lifted from `tests/test_pids.py` — the values are already proven against the car.

**Files:**
- Create: `firmware/components/gauge_core/pid.h`
- Create: `firmware/components/gauge_core/pid.cpp`
- Create: `firmware/test/host/test_pid.cpp`
- Modify: `firmware/test/host/CMakeLists.txt` (add `pid` to the `foreach` list)
- Modify: `firmware/components/gauge_core/CMakeLists.txt` (add `pid.cpp` to `SRCS`)

**Interfaces:**
- Consumes: `check.h` from Task 1
- Produces:
  - `using Bytes = std::vector<uint8_t>;`
  - `std::optional<double> dec_rpm(const Bytes&)`, `dec_speed`, `dec_percent`, `dec_fuel_trim`, `dec_maf`, `dec_timing`, `dec_fuel_rate`, `dec_control_voltage`, `dec_ref_torque`, `dec_catalyst_temp`, `dec_equiv_ratio`, `dec_o2_voltage`, `dec_fuel_pressure`, `dec_u16` — all `std::optional<double>`
  - `std::optional<int> dec_temp(const Bytes&)` — returns int, matching Python

- [x] **Step 1: Write the failing test**

`firmware/test/host/test_pid.cpp`:

```cpp
#include "check.h"
#include "pid.h"
using gauge_test::check;
using gauge::Bytes;
using D = std::optional<double>;
using I = std::optional<int>;

int main() {
    check("rpm 1A F8 -> 1726",  gauge::dec_rpm(Bytes{0x1A, 0xF8}), D{1726.0});
    check("rpm 0C 60 -> 792",   gauge::dec_rpm(Bytes{0x0C, 0x60}), D{792.0});
    check("rpm 00 00 -> 0",     gauge::dec_rpm(Bytes{0x00, 0x00}), D{0.0});
    check("short payload -> None", gauge::dec_rpm(Bytes{0x1A}), D{});

    check("coolant 0x5A -> 50C",  gauge::dec_temp(Bytes{0x5A}), I{50});
    check("coolant 0x28 -> 0C",   gauge::dec_temp(Bytes{0x28}), I{0});
    check("coolant 0x00 -> -40C", gauge::dec_temp(Bytes{0x00}), I{-40});

    check("speed 0x64 -> 100",     gauge::dec_speed(Bytes{0x64}), D{100.0});
    check("throttle 0xFF -> 100%", gauge::dec_percent(Bytes{0xFF}), D{100.0});
    check("throttle 0x00 -> 0%",   gauge::dec_percent(Bytes{0x00}), D{0.0});
    check("fuel trim 0x80 -> 0%",  gauge::dec_fuel_trim(Bytes{0x80}), D{0.0});
    check("maf 01 F4 -> 5.0 g/s",  gauge::dec_maf(Bytes{0x01, 0xF4}), D{5.0});
    check("timing 0x80 -> 0 deg",  gauge::dec_timing(Bytes{0x80}), D{0.0});
    check("fuel rate 00 64 -> 5 L/h",   gauge::dec_fuel_rate(Bytes{0x00, 0x64}), D{5.0});
    check("ctrl volt 37 6C -> 14.188V", gauge::dec_control_voltage(Bytes{0x37, 0x6C}), D{14.188});
    check("ref torque 00 FA -> 250Nm",  gauge::dec_ref_torque(Bytes{0x00, 0xFA}), D{250.0});

    check("catalyst 0F A0 -> 360.0C", gauge::dec_catalyst_temp(Bytes{0x0F, 0xA0}), D{360.0});
    check("catalyst 00 00 -> -40.0C", gauge::dec_catalyst_temp(Bytes{0x00, 0x00}), D{-40.0});
    check("lambda 80 00 -> 1.0",      gauge::dec_equiv_ratio(Bytes{0x80, 0x00}), D{1.0});
    check("O2 voltage 0x64 -> 0.5V",  gauge::dec_o2_voltage(Bytes{0x64}), D{0.5});
    check("fuel pressure 0x64 -> 300kPa", gauge::dec_fuel_pressure(Bytes{0x64}), D{300.0});
    check("runtime 00 3C -> 60s",     gauge::dec_u16(Bytes{0x00, 0x3C}), D{60.0});

    return gauge_test::check_report();
}
```

- [x] **Step 2: Run test to verify it fails**

Run: `cd firmware/test/host && cmake -B build . && cmake --build build`
Expected: FAIL — `fatal error: 'pid.h' file not found`.

- [x] **Step 3: Write minimal implementation**

`firmware/components/gauge_core/pid.h`:

```cpp
#pragma once
#include <cstdint>
#include <optional>
#include <vector>

namespace gauge {

using Bytes = std::vector<uint8_t>;

std::optional<double> dec_rpm(const Bytes& d);
std::optional<int>    dec_temp(const Bytes& d);
std::optional<double> dec_speed(const Bytes& d);
std::optional<double> dec_percent(const Bytes& d);
std::optional<double> dec_fuel_trim(const Bytes& d);
std::optional<double> dec_maf(const Bytes& d);
std::optional<double> dec_timing(const Bytes& d);
std::optional<double> dec_fuel_rate(const Bytes& d);
std::optional<double> dec_control_voltage(const Bytes& d);
std::optional<double> dec_ref_torque(const Bytes& d);
std::optional<double> dec_catalyst_temp(const Bytes& d);
std::optional<double> dec_equiv_ratio(const Bytes& d);
std::optional<double> dec_o2_voltage(const Bytes& d);
std::optional<double> dec_fuel_pressure(const Bytes& d);
std::optional<double> dec_u16(const Bytes& d);
std::optional<double> dec_u8(const Bytes& d);

}  // namespace gauge
```

`firmware/components/gauge_core/pid.cpp` — each body mirrors the Python comment above it in `mx5gauge/pids.py`:

```cpp
#include "pid.h"

namespace gauge {
namespace {
inline bool have(const Bytes& d, size_t n) { return d.size() >= n; }
inline double u16(const Bytes& d) { return d[0] * 256.0 + d[1]; }
}  // namespace

// ((A*256)+B)/4 -> rpm
std::optional<double> dec_rpm(const Bytes& d) {
    if (!have(d, 2)) return std::nullopt;
    return u16(d) / 4.0;
}
// A - 40 -> deg C
std::optional<int> dec_temp(const Bytes& d) {
    if (!have(d, 1)) return std::nullopt;
    return static_cast<int>(d[0]) - 40;
}
// A -> km/h
std::optional<double> dec_speed(const Bytes& d) {
    if (!have(d, 1)) return std::nullopt;
    return static_cast<double>(d[0]);
}
// A * 100/255 -> %
std::optional<double> dec_percent(const Bytes& d) {
    if (!have(d, 1)) return std::nullopt;
    return d[0] * 100.0 / 255.0;
}
// (A - 128) * 100/128 -> %
std::optional<double> dec_fuel_trim(const Bytes& d) {
    if (!have(d, 1)) return std::nullopt;
    return (static_cast<double>(d[0]) - 128.0) * 100.0 / 128.0;
}
// ((A*256)+B)/100 -> g/s
std::optional<double> dec_maf(const Bytes& d) {
    if (!have(d, 2)) return std::nullopt;
    return u16(d) / 100.0;
}
// (A/2) - 64 -> deg before TDC
std::optional<double> dec_timing(const Bytes& d) {
    if (!have(d, 1)) return std::nullopt;
    return (d[0] / 2.0) - 64.0;
}
// ((A*256)+B)/20 -> L/h
std::optional<double> dec_fuel_rate(const Bytes& d) {
    if (!have(d, 2)) return std::nullopt;
    return u16(d) / 20.0;
}
// ((A*256)+B)/1000 -> V
std::optional<double> dec_control_voltage(const Bytes& d) {
    if (!have(d, 2)) return std::nullopt;
    return u16(d) / 1000.0;
}
// (A*256)+B -> Nm
std::optional<double> dec_ref_torque(const Bytes& d) {
    if (!have(d, 2)) return std::nullopt;
    return u16(d);
}
// ((A*256)+B)/10 - 40 -> deg C
std::optional<double> dec_catalyst_temp(const Bytes& d) {
    if (!have(d, 2)) return std::nullopt;
    return u16(d) / 10.0 - 40.0;
}
// ((A*256)+B) * 2/65536 -> ratio
std::optional<double> dec_equiv_ratio(const Bytes& d) {
    if (!have(d, 2)) return std::nullopt;
    return u16(d) * 2.0 / 65536.0;
}
// A/200 -> V
std::optional<double> dec_o2_voltage(const Bytes& d) {
    if (!have(d, 1)) return std::nullopt;
    return d[0] / 200.0;
}
// A*3 -> kPa
std::optional<double> dec_fuel_pressure(const Bytes& d) {
    if (!have(d, 1)) return std::nullopt;
    return d[0] * 3.0;
}
std::optional<double> dec_u16(const Bytes& d) {
    if (!have(d, 2)) return std::nullopt;
    return u16(d);
}
std::optional<double> dec_u8(const Bytes& d) {
    if (!have(d, 1)) return std::nullopt;
    return static_cast<double>(d[0]);
}

}  // namespace gauge
```

**Before writing these bodies, open `mx5gauge/pids.py:12-178` and confirm each divisor and offset against the Python.** The formulas above are transcribed from it, and the Python is authoritative. `dec_fuel_rate`, `dec_control_voltage`, `dec_equiv_ratio` and `dec_catalyst_temp` are the ones whose scaling is easiest to get wrong — the tests above pin all four.

- [x] **Step 4: Run test to verify it passes**

Run: `cd firmware/test/host && cmake -B build . && cmake --build build && ctest --test-dir build --output-on-failure -R pid`
Expected: PASS — 22 checks, "all tests passed".

- [x] **Step 5: Commit**

```bash
git add firmware/components/gauge_core/pid.h firmware/components/gauge_core/pid.cpp \
        firmware/test/host/test_pid.cpp firmware/test/host/CMakeLists.txt \
        firmware/components/gauge_core/CMakeLists.txt
git commit -m "firmware: port PID decode formulas with tests from test_pids.py"
```

---

### Task 3: ELM327 response parsing

Ports `mx5gauge/pids.py:281-315` and `:387-395`. The `_hex_bytes` scanner is the subtle part: it tolerates spaces, prompts, and multi-ECU header noise by scanning for the `41 <pid>` header anywhere in the byte stream.

**Files:**
- Create: `firmware/components/gauge_core/parse.h`
- Create: `firmware/components/gauge_core/parse.cpp`
- Create: `firmware/test/host/test_parse.cpp`
- Modify: `firmware/test/host/CMakeLists.txt` (add `parse`)
- Modify: `firmware/components/gauge_core/CMakeLists.txt` (add `parse.cpp`)

**Interfaces:**
- Consumes: `Bytes` from `pid.h`
- Produces:
  - `Bytes hex_bytes(const std::string& text)`
  - `std::optional<Bytes> parse_mode01(const std::string& text, uint8_t pid)`
  - `std::optional<double> parse_voltage(const std::string& text)`

- [x] **Step 1: Write the failing test**

`firmware/test/host/test_parse.cpp`:

```cpp
#include "check.h"
#include "parse.h"
using gauge_test::check;
using gauge::Bytes;
using B = std::optional<Bytes>;
using D = std::optional<double>;

int main() {
    check("parse 41 0C 1A F8", gauge::parse_mode01("41 0C 1A F8", 0x0C), B{Bytes{0x1A, 0xF8}});
    check("parse no spaces",   gauge::parse_mode01("410C1AF8", 0x0C),    B{Bytes{0x1A, 0xF8}});
    check("parse with prompt", gauge::parse_mode01("41 0C 1A F8 \r>", 0x0C), B{Bytes{0x1A, 0xF8}});
    check("parse with header noise",
          gauge::parse_mode01("7E8 03 41 0C 1A F8", 0x0C), B{Bytes{0x1A, 0xF8}});
    check("parse wrong pid -> None", gauge::parse_mode01("41 0D 20", 0x0C), B{});
    check("parse NO DATA -> None",   gauge::parse_mode01("NO DATA", 0x0C), B{});

    check("ATRV 13.8V",       gauge::parse_voltage("13.8V"),   D{13.8});
    check("ATRV 14.4V\\r>",   gauge::parse_voltage("14.4V\r>"), D{14.4});
    check("ATRV junk -> None", gauge::parse_voltage("ELM327"),  D{});
    return gauge_test::check_report();
}
```

`check.h` needs to print `Bytes`; add this overload to `check.h` in this task:

```cpp
inline std::string show(const std::vector<uint8_t>& v) {
    std::string s = "[";
    for (size_t i = 0; i < v.size(); ++i) {
        char b[8]; snprintf(b, sizeof b, "%s0x%02X", i ? ", " : "", v[i]); s += b;
    }
    return s + "]";
}
```

- [x] **Step 2: Run test to verify it fails**

Run: `cd firmware/test/host && cmake -B build . && cmake --build build`
Expected: FAIL — `fatal error: 'parse.h' file not found`.

- [x] **Step 3: Write minimal implementation**

`firmware/components/gauge_core/parse.h`:

```cpp
#pragma once
#include <optional>
#include <string>
#include "pid.h"

namespace gauge {
Bytes hex_bytes(const std::string& text);
std::optional<Bytes> parse_mode01(const std::string& text, uint8_t pid);
std::optional<double> parse_voltage(const std::string& text);
}  // namespace gauge
```

`firmware/components/gauge_core/parse.cpp`:

```cpp
#include "parse.h"
#include <cstdlib>

namespace gauge {

// Collect hex byte values from an ELM327 reply, ignoring spaces/prompt.
// A non-hex character resets any half-byte in progress.
Bytes hex_bytes(const std::string& text) {
    Bytes out;
    int hi = -1;
    for (char ch : text) {
        int v;
        if (ch >= '0' && ch <= '9')      v = ch - '0';
        else if (ch >= 'a' && ch <= 'f') v = ch - 'a' + 10;
        else if (ch >= 'A' && ch <= 'F') v = ch - 'A' + 10;
        else { hi = -1; continue; }
        if (hi < 0) hi = v;
        else { out.push_back(static_cast<uint8_t>((hi << 4) | v)); hi = -1; }
    }
    return out;
}

std::optional<Bytes> parse_mode01(const std::string& text, uint8_t pid) {
    Bytes b = hex_bytes(text);
    if (b.size() < 2) return std::nullopt;
    for (size_t i = 0; i + 1 < b.size(); ++i) {
        if (b[i] == 0x41 && b[i + 1] == pid) return Bytes(b.begin() + i + 2, b.end());
    }
    return std::nullopt;
}

std::optional<double> parse_voltage(const std::string& text) {
    size_t i = 0;
    while (i < text.size() && !((text[i] >= '0' && text[i] <= '9'))) ++i;
    if (i == text.size()) return std::nullopt;
    size_t j = i;
    while (j < text.size() && ((text[j] >= '0' && text[j] <= '9') || text[j] == '.')) ++j;
    if (j >= text.size() || (text[j] != 'V' && text[j] != 'v')) return std::nullopt;
    return std::strtod(text.substr(i, j - i).c_str(), nullptr);
}

}  // namespace gauge
```

Note `parse_voltage("ELM327")` correctly returns `nullopt`: the scan finds `327` but the next character is end-of-string, not `V`.

- [x] **Step 4: Run test to verify it passes**

Run: `cd firmware/test/host && cmake -B build . && cmake --build build && ctest --test-dir build --output-on-failure -R parse`
Expected: PASS — 9 checks.

- [x] **Step 5: Commit**

```bash
git add firmware/components/gauge_core/parse.* firmware/test/host/test_parse.cpp \
        firmware/test/host/check.h firmware/test/host/CMakeLists.txt \
        firmware/components/gauge_core/CMakeLists.txt
git commit -m "firmware: port ELM327 response parsing with tests"
```

---

### Task 4: Supported-PID bitmask and poll-cycle construction

Ports `mx5gauge/pids.py:246-280` and `:372-386`. `build_poll_cycle` is what makes the needle responsive — §4 records that polling only display channels made the log a narrow slice, so `log_all` sweeps everything with rpm/speed/throttle interleaved.

**Files:**
- Create: `firmware/components/gauge_core/poll.h`
- Create: `firmware/components/gauge_core/poll.cpp`
- Create: `firmware/test/host/test_poll.cpp`
- Modify: `firmware/test/host/CMakeLists.txt` (add `poll`)
- Modify: `firmware/components/gauge_core/CMakeLists.txt` (add `poll.cpp`)

**Interfaces:**
- Consumes: `Bytes` from `pid.h`
- Produces:
  - `std::set<uint8_t> parse_supported(const Bytes& data, uint8_t base)`
  - `std::vector<uint8_t> build_poll_cycle(const std::set<uint8_t>& supported, bool log_all = true)`

- [x] **Step 1: Write the failing test**

`firmware/test/host/test_poll.cpp`:

```cpp
#include "check.h"
#include "poll.h"
#include <algorithm>
using gauge_test::check;
using gauge::Bytes;

static bool has(const std::set<uint8_t>& s, uint8_t v) { return s.count(v) > 0; }
static bool has(const std::vector<uint8_t>& v, uint8_t x) {
    return std::find(v.begin(), v.end(), x) != v.end();
}

int main() {
    // 0xBE1FA813 is the classic example mask for PIDs 01-20
    auto sup = gauge::parse_supported(Bytes{0xBE, 0x1F, 0xA8, 0x13}, 0x00);
    check("bitmask contains 0x0C (rpm)",   has(sup, 0x0C), true);
    check("bitmask contains 0x0D (speed)", has(sup, 0x0D), true);
    check("bitmask excludes 0x02",         has(sup, 0x02), false);
    check("MSB of A means base+1",
          has(gauge::parse_supported(Bytes{0x80, 0, 0, 0}, 0x00), 0x01), true);
    check("LSB of D means base+32",
          has(gauge::parse_supported(Bytes{0, 0, 0, 0x01}, 0x00), 0x20), true);
    check("base 0x20 offsets",
          has(gauge::parse_supported(Bytes{0x80, 0, 0, 0}, 0x20), 0x21), true);

    auto cyc = gauge::build_poll_cycle({0x0C, 0x0D, 0x05});
    check("poll cycle includes rpm",        has(cyc, 0x0C), true);
    check("poll cycle drops unsupported oil", has(cyc, 0x5C), false);
    check("poll cycle empty when nothing supported",
          gauge::build_poll_cycle({}).empty(), true);

    auto wide = gauge::build_poll_cycle({0x0C, 0x0D, 0x05, 0x2F, 0x46}, true);
    check("log_all includes fuel level 0x2F", has(wide, 0x2F), true);
    check("log_all includes ambient 0x46",    has(wide, 0x46), true);
    check("rpm interleaved between slow pids",
          std::count(wide.begin(), wide.end(), uint8_t{0x0C}) > 1, true);

    auto narrow = gauge::build_poll_cycle({0x0C, 0x0D, 0x05, 0x2F, 0x46}, false);
    check("display-only skips fuel level", has(narrow, 0x2F), false);
    check("display-only keeps coolant",    has(narrow, 0x05), true);
    return gauge_test::check_report();
}
```

- [x] **Step 2: Run test to verify it fails**

Run: `cd firmware/test/host && cmake -B build . && cmake --build build`
Expected: FAIL — `fatal error: 'poll.h' file not found`.

- [x] **Step 3: Write minimal implementation**

Read `mx5gauge/pids.py:246-280` before writing this. `POLL_FAST` is `{0x0C, 0x0D, 0x11, 0x1F}` and `POLL_PRIORITY` is the ordered list at line 248; both must be transcribed exactly, and `PIDS` (line 181) supplies the set of PIDs that are legal to request at all.

`firmware/components/gauge_core/poll.h`:

```cpp
#pragma once
#include <cstdint>
#include <set>
#include <vector>
#include "pid.h"

namespace gauge {
std::set<uint8_t> parse_supported(const Bytes& data, uint8_t base);
std::vector<uint8_t> build_poll_cycle(const std::set<uint8_t>& supported, bool log_all = true);
}  // namespace gauge
```

`firmware/components/gauge_core/poll.cpp`:

```cpp
#include "poll.h"

namespace gauge {

// Bit 7 of byte 0 means base+1; bit 0 of byte 3 means base+32.
std::set<uint8_t> parse_supported(const Bytes& data, uint8_t base) {
    std::set<uint8_t> out;
    if (data.size() < 4) return out;
    for (int i = 0; i < 4; ++i) {
        for (int bit = 0; bit < 8; ++bit) {
            if (data[i] & (0x80 >> bit)) {
                out.insert(static_cast<uint8_t>(base + i * 8 + bit + 1));
            }
        }
    }
    return out;
}

}  // namespace gauge
```

`build_poll_cycle` follows the Python at `pids.py:251`: filter `POLL_PRIORITY` (or the full `PIDS` key set when `log_all`) to those in `supported`, then interleave one member of `POLL_FAST` between each slow PID so rpm/speed stay responsive. Transcribe the ordering from the Python rather than inventing one — the test only pins the observable properties, so a different-but-plausible ordering would pass while changing needle behaviour in the car.

- [x] **Step 4: Run test to verify it passes**

Run: `cd firmware/test/host && cmake -B build . && cmake --build build && ctest --test-dir build --output-on-failure -R poll`
Expected: PASS — 14 checks.

- [x] **Step 5: Commit**

```bash
git add firmware/components/gauge_core/poll.* firmware/test/host/test_poll.cpp \
        firmware/test/host/CMakeLists.txt firmware/components/gauge_core/CMakeLists.txt
git commit -m "firmware: port supported-PID bitmask and poll cycle"
```

---

### Task 5: `VehicleState` and range validation

Ports `mx5gauge/state.py` (374 lines). This is where §4's "views degrade honestly" is enforced: a channel outside its plausibility bound becomes absent, never a plausible-looking zero.

**Files:**
- Create: `firmware/components/gauge_core/state.h`
- Create: `firmware/components/gauge_core/state.cpp`
- Create: `firmware/test/host/test_state.cpp`
- Modify: `firmware/test/host/CMakeLists.txt` (add `state`)
- Modify: `firmware/components/gauge_core/CMakeLists.txt` (add `state.cpp`)

**Interfaces:**
- Consumes: `pid.h`
- Produces:
  - `struct VehicleState` with `std::optional<double>` members for each channel (`rpm`, `speed`, `throttle`, `coolant`, `intake`, `battery`, `catalyst`, `fuel_rate`, `oil`, ...) — names transcribed from `state.py`
  - `bool in_range(const std::string& channel, double value)`
  - `void VehicleState::set(const std::string& channel, std::optional<double> value)` — applies range validation, leaving the channel absent when out of range
  - `std::optional<double> VehicleState::get(const std::string& channel) const` — `nullopt` when the channel is absent
  - `bool VehicleState::has(const std::string& channel) const`

- [x] **Step 1: Write the failing test**

Read `state.py`'s `RANGES` table first and transcribe the real bounds; the test below uses coolant as the worked example because §4 cites the 72→95°C warm-up as the channel that matters most.

```cpp
#include "check.h"
#include "state.h"
using gauge_test::check;
using D = std::optional<double>;

int main() {
    gauge::VehicleState s;
    check("fresh state has no coolant", s.has("coolant"), false);

    s.set("coolant", 88.0);
    check("plausible coolant is kept", s.get("coolant"), D{88.0});

    s.set("coolant", 900.0);
    check("implausible coolant is rejected", s.get("coolant"), D{88.0});

    s.set("rpm", 1726.0);
    check("rpm kept", s.get("rpm"), D{1726.0});
    s.set("rpm", -5.0);
    check("negative rpm rejected", s.get("rpm"), D{1726.0});

    check("absent channel reads as nullopt", s.get("oil"), D{});
    check("in_range accepts mid-scale coolant", gauge::in_range("coolant", 88.0), true);
    check("in_range rejects 900C coolant",      gauge::in_range("coolant", 900.0), false);
    return gauge_test::check_report();
}
```

- [x] **Step 2: Run test to verify it fails**

Run: `cd firmware/test/host && cmake -B build . && cmake --build build`
Expected: FAIL — `fatal error: 'state.h' file not found`.

- [x] **Step 3: Write minimal implementation**

Implement `VehicleState` as a `std::map<std::string, double>` behind `get`/`set`/`has`, plus a static `RANGES` table transcribed from `state.py`. A string-keyed map is chosen deliberately over named struct fields: the poll loop, the recorder and the UI all address channels dynamically by name, exactly as the Python does, and a named-field struct would need a parallel lookup anyway.

```cpp
#pragma once
#include <map>
#include <optional>
#include <string>

namespace gauge {

bool in_range(const std::string& channel, double value);

class VehicleState {
  public:
    void set(const std::string& channel, std::optional<double> value);
    std::optional<double> get(const std::string& channel) const;
    bool has(const std::string& channel) const;
  private:
    std::map<std::string, double> values_;
};

}  // namespace gauge
```

`set` ignores `nullopt` and out-of-range values, leaving any previous good reading in place — matching the Python, where a failed poll does not blank the display.

- [x] **Step 4: Run test to verify it passes**

Run: `cd firmware/test/host && cmake -B build . && cmake --build build && ctest --test-dir build --output-on-failure -R state`
Expected: PASS — 8 checks.

- [x] **Step 5: Commit**

```bash
git add firmware/components/gauge_core/state.* firmware/test/host/test_state.cpp \
        firmware/test/host/CMakeLists.txt firmware/components/gauge_core/CMakeLists.txt
git commit -m "firmware: port VehicleState with range validation"
```

---

### Task 6: Metrics — economy, trip, driving score

Ports `mx5gauge/metrics.py` (271 lines); assertions come from `tests/test_metrics.py` (148 lines). **Read both in full before starting.**

**Files:**
- Create: `firmware/components/gauge_core/metrics.h`
- Create: `firmware/components/gauge_core/metrics.cpp`
- Create: `firmware/test/host/test_metrics.cpp`
- Modify: `firmware/test/host/CMakeLists.txt` (add `metrics`)
- Modify: `firmware/components/gauge_core/CMakeLists.txt` (add `metrics.cpp`)

**Interfaces:**
- Consumes: `state.h`
- Produces:
  - `struct Tunables { double fuel_price_rm = 2.05; double w_smooth = 0.40, w_econ = 0.30, w_calm = 0.30; double eco_rpm_lo = 1200, eco_rpm_hi = 2600; double harsh_accel, harsh_brake; };`
  - `class Metrics { void update(const VehicleState&, double dt_seconds); double km_per_litre() const; double trip_km() const; double trip_cost_rm() const; int score() const; std::string coach_word() const; };`

**The tunable defaults above are transcribed from `SPEC.md` §5 "Tunables" and must match `metrics.py` exactly.** They are the numbers B3 will eventually change, so they live in one struct rather than scattered as constants.

- [x] **Step 1: Write the failing test**

Port the existing assertions from `tests/test_metrics.py`. Two properties matter most and must be covered:

```cpp
#include "check.h"
#include "metrics.h"
using gauge_test::check;

int main() {
    // Economy is quoted in km/L (see commit "Quote fuel economy in km/L").
    gauge::Metrics m;
    gauge::VehicleState s;
    s.set("speed", 60.0);       // km/h
    s.set("fuel_rate", 6.0);    // L/h  -> 10 km/L
    m.update(s, 1.0);
    check("60 km/h at 6 L/h -> 10 km/L", m.km_per_litre(), 10.0);

    // Metrics must see real-world seconds: a 10x replay must not invent
    // harsh events (SPEC.md section 4 - 84 false events were seen this way).
    gauge::Metrics fast;
    gauge::VehicleState a, b;
    a.set("speed", 0.0);  fast.update(a, 0.0);
    b.set("speed", 50.0); fast.update(b, 10.0);   // 50 km/h over 10 real seconds
    check("gentle accel over 10s is not harsh", fast.harsh_events(), 0);
    return gauge_test::check_report();
}
```

Add `int harsh_events() const;` to the `Metrics` interface for this test.

- [x] **Step 2: Run test to verify it fails**

Run: `cd firmware/test/host && cmake -B build . && cmake --build build`
Expected: FAIL — `fatal error: 'metrics.h' file not found`.

- [x] **Step 3: Write minimal implementation**

Transcribe from `metrics.py`. Keep the accumulators (distance, time, fuel) as running sums updated by `dt_seconds`, and take `dt` from the caller rather than a clock — that is what makes replay-vs-live behave identically and is the direct fix for the phantom-braking bug in §4.

Leave the score formula exactly as the Python has it. **Do not re-tune `harsh_accel` / `harsh_brake` here** — those change in Task 11 against real IMU data, and B3 is still an open decision.

- [x] **Step 4: Run test to verify it passes**

Run: `cd firmware/test/host && cmake -B build . && cmake --build build && ctest --test-dir build --output-on-failure -R metrics`
Expected: PASS.

- [x] **Step 5: Commit**

```bash
git add firmware/components/gauge_core/metrics.* firmware/test/host/test_metrics.cpp \
        firmware/test/host/CMakeLists.txt firmware/components/gauge_core/CMakeLists.txt
git commit -m "firmware: port economy, trip and driving-score metrics"
```

---

### Task 7: Vehicle identity and dial profiles

Ports `mx5gauge/vehicle.py` (257 lines); assertions from `tests/test_vehicle.py` (109 lines). This is §10's universal layer — VIN decode, per-car dial scaling, and the honest view gating that hides a view whose channels the car does not report.

**Files:**
- Create: `firmware/components/gauge_core/vehicle.h`
- Create: `firmware/components/gauge_core/vehicle.cpp`
- Create: `firmware/test/host/test_vehicle.cpp`
- Modify: `firmware/test/host/CMakeLists.txt` (add `vehicle`)
- Modify: `firmware/components/gauge_core/CMakeLists.txt` (add `vehicle.cpp`)

**Interfaces:**
- Consumes: `state.h`, `parse.h`
- Produces:
  - `struct Identity { std::string make; std::string model; int model_year = 0; std::string vin; };`
  - `Identity decode_vin(const std::string& vin)`
  - `struct DialProfile { double rpm_max; double speed_max; double coolant_lo, coolant_hi; };`
  - `DialProfile profile_for(const Identity&)`
  - `bool view_is_fed(int view_number, const std::set<uint8_t>& supported)`

- [x] **Step 1: Write the failing test**

Port from `tests/test_vehicle.py`. The two behaviours that must hold:

```cpp
#include "check.h"
#include "vehicle.h"
using gauge_test::check;

int main() {
    // A VIN gives make and model year, never a model name (SPEC.md section on
    // the universal layer) - the model is user-supplied.
    auto id = gauge::decode_vin("JM1NDAD75M0123456");
    check("VIN yields a make", id.make.empty(), false);
    check("VIN yields no model name", id.model.empty(), true);
    check("VIN yields a model year", id.model_year > 1980, true);

    // A view with no supported channels is gated off, not drawn empty.
    check("thermals gated off with no temp PIDs", gauge::view_is_fed(7, {}), false);
    check("engine view fed by coolant",           gauge::view_is_fed(2, {0x05}), true);
    return gauge_test::check_report();
}
```

Confirm the exact VIN and expected make against `tests/test_vehicle.py` before writing — use the VIN that file already uses rather than the illustrative one above.

- [x] **Step 2: Run test to verify it fails**

Run: `cd firmware/test/host && cmake -B build . && cmake --build build`
Expected: FAIL — `fatal error: 'vehicle.h' file not found`.

- [x] **Step 3: Write minimal implementation**

Transcribe `PROFILES` and the WMI→make table from `vehicle.py`. The model-year character maps via the standard VIN position-10 table, which `vehicle.py` already implements.

- [x] **Step 4: Run test to verify it passes**

Run: `cd firmware/test/host && cmake -B build . && cmake --build build && ctest --test-dir build --output-on-failure -R vehicle`
Expected: PASS.

- [x] **Step 5: Commit**

```bash
git add firmware/components/gauge_core/vehicle.* firmware/test/host/test_vehicle.cpp \
        firmware/test/host/CMakeLists.txt firmware/components/gauge_core/CMakeLists.txt
git commit -m "firmware: port VIN decode, dial profiles and view gating"
```

---

### Task 8: Ignition detection

Ports `mx5gauge/ignition.py` (92 lines); assertions from `tests/test_ignition.py` (163 lines). On the board this drives deep-sleep rather than file rotation.

**Caveat carried from the backlog:** ignition detection shipped 2026-08-16 on an unmerged branch and has **never been run in the car** — it is validated only against the 2026-08-16 log. Port it as-is; do not "improve" it, because its real-world behaviour is still unproven and changing it now would confound the first in-car test.

**Files:**
- Create: `firmware/components/gauge_core/ignition.h`
- Create: `firmware/components/gauge_core/ignition.cpp`
- Create: `firmware/test/host/test_ignition.cpp`
- Modify: `firmware/test/host/CMakeLists.txt` (add `ignition`)
- Modify: `firmware/components/gauge_core/CMakeLists.txt` (add `ignition.cpp`)

**Interfaces:**
- Consumes: `state.h`
- Produces:
  - `enum class Ignition { Unknown, Running, Stopped };`
  - `class IgnitionDetector { Ignition update(const VehicleState&, double t_seconds); Ignition state() const; };`

- [x] **Step 1: Write the failing test**

```cpp
#include "check.h"
#include "ignition.h"
using gauge_test::check;

int main() {
    gauge::IgnitionDetector det;
    gauge::VehicleState s;
    s.set("rpm", 850.0);
    check("running engine detected", det.update(s, 0.0) == gauge::Ignition::Running, true);

    gauge::VehicleState off;
    off.set("rpm", 0.0);
    det.update(off, 1.0);
    det.update(off, 30.0);
    check("sustained zero rpm means stopped",
          det.state() == gauge::Ignition::Stopped, true);
    return gauge_test::check_report();
}
```

Confirm the debounce window against `ignition.py` — the exact dwell before declaring a stop is what stops a stall or a restart being misread, and `tests/test_ignition.py` pins it.

- [x] **Step 2: Run test to verify it fails**

Run: `cd firmware/test/host && cmake -B build . && cmake --build build`
Expected: FAIL — `fatal error: 'ignition.h' file not found`.

- [x] **Step 3: Write minimal implementation**

Transcribe the two-signal detection from `ignition.py` and its debounce.

- [x] **Step 4: Run test to verify it passes**

Run: `cd firmware/test/host && cmake -B build . && cmake --build build && ctest --test-dir build --output-on-failure -R ignition`
Expected: PASS.

- [x] **Step 5: Commit**

```bash
git add firmware/components/gauge_core/ignition.* firmware/test/host/test_ignition.cpp \
        firmware/test/host/CMakeLists.txt firmware/components/gauge_core/CMakeLists.txt
git commit -m "firmware: port ignition detection for deep-sleep"
```

---

### Task 9: `Elm327` over an abstract transport

Splits `mx5gauge/sources.py` (447 lines) along the seam the spec identifies: protocol logic is pure and host-tested; BLE is platform. Assertions come from `tests/test_sources.py` (199 lines).

**Files:**
- Create: `firmware/components/gauge_core/transport.h`
- Create: `firmware/components/gauge_core/elm327.h`
- Create: `firmware/components/gauge_core/elm327.cpp`
- Create: `firmware/test/host/fake_transport.h`
- Create: `firmware/test/host/test_elm327.cpp`
- Modify: `firmware/test/host/CMakeLists.txt` (add `elm327`)
- Modify: `firmware/components/gauge_core/CMakeLists.txt` (add `elm327.cpp`)

**Interfaces:**
- Consumes: `parse.h`, `poll.h`, `state.h`
- Produces:
  - `struct ITransport { virtual ~ITransport() = default; virtual bool write(const std::string&) = 0; virtual std::string read(int timeout_ms) = 0; };`
  - `class Elm327 { explicit Elm327(ITransport&); bool init(); std::set<uint8_t> discover_supported(); std::optional<Bytes> request(uint8_t pid); std::optional<double> read_voltage(); };`

- [x] **Step 1: Write the failing test**

`firmware/test/host/fake_transport.h`:

```cpp
#pragma once
#include <deque>
#include <string>
#include <vector>
#include "transport.h"

struct FakeTransport : gauge::ITransport {
    std::vector<std::string> written;
    std::deque<std::string> replies;
    bool write(const std::string& s) override { written.push_back(s); return true; }
    std::string read(int) override {
        if (replies.empty()) return "";
        std::string r = replies.front(); replies.pop_front(); return r;
    }
};
```

`firmware/test/host/test_elm327.cpp`:

```cpp
#include "check.h"
#include "elm327.h"
#include "fake_transport.h"
using gauge_test::check;
using B = std::optional<gauge::Bytes>;

int main() {
    FakeTransport t;
    t.replies = {"ELM327 v1.5\r>", "OK\r>", "OK\r>", "OK\r>"};
    gauge::Elm327 elm(t);
    check("init succeeds on a healthy adapter", elm.init(), true);
    check("init sent a reset", t.written.empty(), false);

    t.replies = {"41 0C 1A F8\r>"};
    check("request returns decoded payload", elm.request(0x0C), B{gauge::Bytes{0x1A, 0xF8}});

    t.replies = {"NO DATA\r>"};
    check("NO DATA yields nothing", elm.request(0x0C), B{});
    return gauge_test::check_report();
}
```

- [x] **Step 2: Run test to verify it fails**

Run: `cd firmware/test/host && cmake -B build . && cmake --build build`
Expected: FAIL — `fatal error: 'elm327.h' file not found`.

- [x] **Step 3: Write minimal implementation**

Transcribe the handshake sequence from `sources.py` exactly — `ATZ`, `ATE0`, and the rest, in the order the Python sends them, with the same waits. §3 warns that cheap clones are flaky; the existing sequence is what has been proven against the vLinker in the car, so the ordering is load-bearing.

- [x] **Step 4: Run test to verify it passes**

Run: `cd firmware/test/host && cmake -B build . && cmake --build build && ctest --test-dir build --output-on-failure -R elm327`
Expected: PASS.

- [x] **Step 5: Commit**

```bash
git add firmware/components/gauge_core/transport.h firmware/components/gauge_core/elm327.* \
        firmware/test/host/fake_transport.h firmware/test/host/test_elm327.cpp \
        firmware/test/host/CMakeLists.txt firmware/components/gauge_core/CMakeLists.txt
git commit -m "firmware: split ELM327 protocol from transport, host-tested"
```

---

### Task 10: Cross-validation harness — C++ must agree with Python

The strongest verification available: both implementations consume the same capture, and their outputs must match. This converts "does the C++ match the Python?" from a judgement call into a diff.

**Files:**
- Create: `firmware/test/host/replay_check.cpp`
- Create: `tools/dump_python_states.py`
- Create: `firmware/test/host/README.md`
- Modify: `firmware/test/host/CMakeLists.txt` (add the `replay_check` executable)

**Interfaces:**
- Consumes: everything from Tasks 2–8
- Produces: a `replay_check` binary taking a CSV path and a reference JSONL path, exiting non-zero on any divergence

- [x] **Step 1: Write the failing test**

`tools/dump_python_states.py` replays a capture through the **Python** and writes one JSON object per sample — the reference:

```python
"""Dump per-sample VehicleState from the Python implementation, for
cross-validating the C++ port. Usage:
    .venv/bin/python tools/dump_python_states.py logs/<drive>.csv > ref.jsonl
"""
import json, os, sys
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from mx5gauge import metrics, state  # noqa: E402

def main(path):
    gauge = state.VehicleState()
    m = metrics.Metrics()
    for row, dt in state.iter_capture(path):      # confirm helper name in state.py
        for channel, value in row.items():
            gauge.set(channel, value)
        m.update(gauge, dt)
        print(json.dumps({
            "coolant": gauge.get("coolant"),
            "rpm": gauge.get("rpm"),
            "speed": gauge.get("speed"),
            "km_per_litre": round(m.km_per_litre(), 6),
            "trip_km": round(m.trip_km(), 6),
            "score": m.score(),
        }, sort_keys=True))

if __name__ == "__main__":
    main(sys.argv[1])
```

`replay_check.cpp` feeds the same CSV through the C++ core and compares each sample against the reference line, tolerance `1e-6` on doubles and exact on integers, reporting the first divergence with its sample index and channel.

- [x] **Step 2: Run test to verify it fails**

Run:
```bash
.venv/bin/python tools/dump_python_states.py logs/$(ls logs | head -1) > /tmp/ref.jsonl
cd firmware/test/host && cmake --build build && ./build/replay_check ../../../logs/$(ls ../../../logs | head -1) /tmp/ref.jsonl
```
Expected: FAIL — divergence, or "no such file", before the C++ CSV reader exists.

- [x] **Step 3: Write minimal implementation**

Implement the CSV reader in `replay_check.cpp` only — **not** in `gauge_core`. The board never reads CSVs (that is Phase 3 SD work), so this stays test-side.

- [x] **Step 4: Run test to verify it passes**

Run the Step 2 commands again.
Expected: PASS — "N samples, 0 divergences".

Run it against **every** capture in `logs/`, not just one. Divergences tend to hide in the unusual samples — dropped channels, engine-off gaps, the fragments under a minute that the backlog notes.

- [x] **Step 5: Commit**

```bash
git add firmware/test/host/replay_check.cpp tools/dump_python_states.py \
        firmware/test/host/README.md firmware/test/host/CMakeLists.txt
git commit -m "firmware: cross-validate C++ core against the Python simulator"
```

---


## Execution notes (2026-08-20, branch `firmware-core-port`)

Tasks 1-10 are complete. Four things went differently from the plan:

1. **`make`, not CMake** (see Global Constraints) — `cmake` was not installed.
2. **Task 8's test sketch was wrong.** The plan guessed ignition was detected
   from rpm; it is actually detected from `volts` plus PID silence plus
   `run_time` going backwards. The real semantics were ported, and the
   real-drive fixture (`tests/fixtures/ignition-edges.csv`) is now a C++ test
   that reproduces exactly two ignition-offs and two restarts.
3. **`vehicle.from_capture_header` was not ported** — it reads Car Scanner
   capture headers, which the board never sees.
4. **Task 10's reference format is flat text, not JSONL**, so the C++ side
   needs no JSON parser.

Two tables are machine-extracted from the Python rather than transcribed (the
56-entry PID table, the 118-entry WMI table and the dial profiles), because
transcription is exactly where a port of this shape goes wrong.

**Result:** 10 host suites pass; `replay_check` reports **0 divergences across
42,412 samples** from all three drives in `logs/`. The Python suite still
passes unchanged.

## Board quirks (learned the hard way, 2026-08-21)

This board fights the standard ESP-IDF workflow in four ways. All four cost
real time before they were understood.

1. **There is no RESET button — only BOOT.** So "tap RESET" is not a thing you
   can do: the reset is *unplug, wait ~5s, replug, touching nothing*. Holding
   BOOT while replugging is what enters download mode.
2. **Every `esptool` invocation re-enters download mode** (`--before
   default-reset` is the default). Diagnosing "why won't it boot?" with esptool
   therefore recreates the state being diagnosed. Read the port **passively**
   with pyserial instead, and only reach for esptool when you intend to flash.
3. **`idf.py monitor` cannot be used here at all** — idf_monitor requires stdin
   to be a TTY and exits 1 otherwise. Use a passive pyserial read.
4. **The component manifest must live at `main/idf_component.yml`.** A manifest
   at the *project* root (`firmware/idf_component.yml`) is silently ignored:
   `idf.py reconfigure` succeeds, downloads nothing, and `managed_components/`
   never appears. Note also that `idf.py add-dependency` only edits the
   manifest — it does not contact the registry, so it "succeeding" proves
   nothing about whether a component exists.
5. **The console is USB-Serial/JTAG only.** There is no USB-UART bridge, so
   IDF's default UART0 console prints to pins nothing is connected to.
   `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` is mandatory, not optional.

Also: the port number changes with the Mac's USB socket (`usbmodem1101` vs
`usbmodem3101`). Detect it, never hardcode it:

```sh
PORT=$(ls /dev/cu.usbmodem* | head -1)
```

**The working loop is:** build -> flash -> replug -> passive read.

### Task 13 results (touch + IMU, 2026-08-21)

- **I2C devices on the shared bus**: `0x18 0x34 0x40 0x5A 0x6B` (codecs, power
  management, IMU). `BSP_CAPS_IMU` is 0, so the IMU is driven by
  `firmware/main/imu.c`, a ~100-line QMI8658 driver over `bsp_i2c_get_handle()`.
- **QMI8658 at 0x6B**, `WHO_AM_I = 0x05`.
- **Accel scaling validated against gravity**: +/-8g, 4096 LSB/g gives
  |a| = 1.044 g at rest. 4.4% high, which is a ~0.01 g error at the 2.5 m/s^2
  harsh threshold - negligible, but recorded rather than assumed.
- **Z is normal to the screen** (+1.04 g flat, screen up). X and Y are in-plane.
- **CST9217 reports its own resolution as 466x466**, a third independent
  confirmation after the BSP header and the unclipped full-rim arc.
- **Touch mapping verified visually**: a dot tracks the finger with no mirroring
  or axis swap.

**Which in-plane axis is longitudinal cannot be answered by the board.** X and Y
both lie in the plane of the screen; which one points down the car is a property
of the *mounting*, not the hardware, and rotating the gauge 90 degrees swaps
them. So the driving score must not hardcode an axis. Preferred fix: learn it -
gravity at rest gives "down", and the first hard straight-line accel/brake event
identifies longitudinal. Settle this with B3, since B3 already owns the question
of what "harsh" means.

### Confirmed on hardware

- **PSRAM is octal**: `esp_psram: SPI SRAM memory test OK`, 8388608 bytes.
  Spec open question 1 (PSRAM mode) is answered; `CONFIG_SPIRAM_MODE_OCT=y`.
- **32MB flash size config boots fine.**
- **`gauge_core` runs unchanged on the board**: 9/9 bring-up checks, including
  the plausibility gate that `replay_check` can never exercise on clean logs.
- **The restore path works end to end**: `write-flash 0` of the backup brought
  Xiaozhi back, booting, with its UI on the display, in ~2.5 minutes.
- **Panel is 466x466** (`BSP_LCD_H_RES`/`V_RES`), and a full-rim arc renders
  unclipped — so there is no rotation or offset error to chase.
- **A real UI runs on the panel**: the section 6 view 2 engine-vitals screen,
  animating, driven by `gauge_core` with the plausibility gate in the path.
  Tasks 12's display half is done; touch and IMU (Task 13) are not.

## Milestone 2026-08-22: eight views, swiping, replay on device

The simulator's live feature set now runs on the board:

- **Replay on device.** `tools/build_drive_asset.py` compiles `logs/*.csv` into
  a 0.49MB binary library (3 drives, 35 channels, 42412 records) in a `drives`
  partition; `gauge_core/replay` is pure and host-tested. Playback is 4x with
  the metrics fed the capture's own timeline.
- **Peaks ported**, so `verify_port.sh` compares 29 derived fields and no
  longer reports parity it was not checking.
- **Eight of section 6's nine views**, as a table rather than eight layouts.
  Drives is deliberately absent: it browses recorded drives and nothing on the
  board records yet.
- **Swiping**, with the wrap host-tested in `test_carousel.cpp`.
- **Page indicator** animates instead of the view sliding.

### The input bug, and what it cost

Seven attempts. Worth reading before touching LVGL input again:

| Attempt | Outcome |
|---|---|
| Poll `lv_indev_get_state()` from the app loop at 30Hz | Real bug (missed fast flicks), not the main one |
| `lv_indev_wait_release()` | No effect; measured |
| Z-order (ELECTRICAL drawn last, on top) | Disproved: visibility bitmask never had >1 bit |
| 400ms gesture debounce | Real bug (LVGL repeats GESTURE every input read), not the wedge |
| View roots non-clickable | Real bug (`switch_to` hid the active press target), and it **moved** the problem |
| Host-test the wrap | Disproved "no infinite scrolling": ring maths was always correct |
| **Screen `LV_OBJ_FLAG_SCROLLABLE`** | **The cause.** With presses landing on the screen, LVGL claimed each drag as a scroll and suppressed the gesture |

**The lesson: instrument the boundary before proposing a fix.** Counting
presses, releases and gestures separately is what finally distinguished "input
is dead" from "input is fine but gestures are suppressed" -- and that one
capture was worth more than four of the guesses above. Note also that making
the view roots non-clickable *caused* the final bug by moving presses onto the
one object whose scroll flag was still set.

### Display ceiling (measured, not assumed)

The CO5300 runs at 40MHz QSPI: ~22ms to move a 434KB frame, ~30ms of render on
top, so **~52ms and ~19fps for any full-screen change**. Double buffering would
overlap those but the adapter permits only tear-avoid NONE or TE_SYNC on a SPI
panel -- DOUBLE_PARTIAL boot-loops. So full-screen slide transitions are not
available on this hardware at any quality, which is why the page indicator
animates and the view content cuts.

## Known gaps when picking this up (2026-08-21) — BOTH CLOSED 2026-08-22/23

Kept for the diagnoses, which were right; do not pick this section up as work.
Verified closed on 2026-08-26 by reading the code and re-running the harness.

1. **The boot splash ran at ~6.5 fps.** The diagnosis was correct: the LVGL
   canvas cost the frame time. `play_boot_clip()` in `main/main.cpp` now goes
   straight to the panel via `gauge_ui::direct_draw_begin/frame/end`, the same
   banded path the carousel slide uses, and `sdkconfig.defaults` gained -O2 /
   240 MHz / QIO / 1 kHz tick with the panel QSPI clock raised 40 -> 80 MHz.
   **All 31 frames now play, at 12.4 fps** (78 ms/frame: 47 read + 12 swap +
   19 blit). Per-stage numbers in `memory/mx5-gauge-boot-splash-budget.md`.

   **What is still on the table, and it is the only measured win left:**
   playback reads each frame with `esp_partition_read` (47 ms); the benched
   `mmap`+memcpy path costs 29 ms, which would give ~60 ms => ~16 fps. It is
   not a drop-in: `assets` spans 0x410000-0x1410000 but only the part below the
   16 MB MMU line (0x1000000) can be mapped, which is ~12.5 MB = 30 of the
   clip's 31 frames, so the tail frame needs a read fallback. Weigh that
   against the fact that **24 fps full-screen is unreachable at any frame
   count** (the flash budget caps it near 28 frames), and that Tommy chose to
   author a new, cheaper animation rather than optimise this photographic one.

2. **The C++ core was one feature behind the simulator** on per-drive peaks.
   Closed by commit 607b307: `VehicleState` holds `peaks_` with the catalyst
   family folded into one field, and `replay_check.cpp` compares `peak_rpm`,
   `peak_kw`, `peak_speed`, `peak_coolant`, `peak_intake` and `peak_catalyst`
   on every drive. `verify_port.sh` reports 15 suites and 0 divergences across
   all three captures, so its "PORT VERIFIED" no longer overstates.

## Part B — hardware bring-up (board required)

Tasks 11–14 need the board. **Before the first flash, verify the backup:**
`shasum -a 256 -c backups/esp32s3-full-32MB.bin.sha256` must print `OK`.

These tasks are deliberately discovery-first. The vendor BSP's exact API is not knowable from here — it is read from the component's headers as the first step of each task. That is a real step with a real output, not a deferred decision.

---

### Task 11: ESP-IDF toolchain and project skeleton

**Files:**
- Create: `firmware/CMakeLists.txt`
- Create: `firmware/main/CMakeLists.txt`
- Create: `firmware/main/main.cpp`
- Create: `firmware/sdkconfig.defaults`
- Create: `firmware/partitions.csv`
- Create: `firmware/idf_component.yml`
- Modify: `.gitignore` (add `firmware/build/`, `firmware/sdkconfig`)

- [ ] **Step 1: Install the toolchain**

```bash
mkdir -p ~/esp && cd ~/esp
git clone -b v5.5.2 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf && ./install.sh esp32s3
```

Expected: "All done!". This is a ~2–3GB download. Pin to v5.5.2 to match the board's existing firmware.

- [ ] **Step 2: Verify it fails without a project**

```bash
source ~/esp/esp-idf/export.sh
cd firmware && idf.py build
```
Expected: FAIL — no `CMakeLists.txt`.

- [ ] **Step 3: Create the project**

`firmware/CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(mx5_gauge)
```

`firmware/main/CMakeLists.txt`:

```cmake
idf_component_register(SRCS "main.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES gauge_core)
```

`firmware/main/main.cpp`:

```cpp
#include <cstdio>
#include "version.h"

extern "C" void app_main(void) {
    printf("mx5-gauge core %s\n", gauge::core_version());
}
```

`firmware/partitions.csv` — our own layout. The app is far smaller than Xiaozhi's 9MB, so this is generous rather than tight:

```csv
# Name,   Type, SubType, Offset,   Size
nvs,      data, nvs,     0x9000,   0x6000
phy_init, data, phy,     0xf000,   0x1000
factory,  app,  factory, 0x10000,  0x400000
assets,   data, spiffs,  0x410000, 0x400000
```

`firmware/sdkconfig.defaults`:

```
CONFIG_IDF_TARGET="esp32s3"
CONFIG_ESPTOOLPY_FLASHSIZE_32MB=y
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_BT_ENABLED=y
CONFIG_BT_NIMBLE_ENABLED=y
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_COMPILER_CXX_EXCEPTIONS=n
```

`firmware/idf_component.yml` — starts with only the IDF floor; Task 12 adds the display and touch components once their registry names are confirmed:

```yaml
dependencies:
  idf:
    version: ">=5.5.0,<5.6.0"
```

Confirm `CONFIG_SPIRAM_MODE_OCT` against the board — `esptool flash-id` reported 8MB embedded PSRAM, but octal-vs-quad must be verified or the board will fail to boot. If it does, try `CONFIG_SPIRAM_MODE_QUAD`.

- [ ] **Step 4: Build and flash**

```bash
source ~/esp/esp-idf/export.sh
cd firmware && idf.py set-target esp32s3 && idf.py build
idf.py -p /dev/cu.usbmodem1101 flash monitor
```
Expected: `mx5-gauge core 0.1.0` on the serial monitor.

This is the first flash. Xiaozhi is now overwritten — recoverable per `backups/RESTORE.md`.

- [ ] **Step 5: Prove the restore path works, then reflash**

```bash
esptool --port /dev/cu.usbmodem1101 write-flash 0 backups/esp32s3-full-32MB.bin
```
Expected: Xiaozhi boots again. **Do this once, now** — a restore path you have never exercised is not a backup. Then reflash the gauge and continue.

- [ ] **Step 6: Commit**

```bash
git add firmware/CMakeLists.txt firmware/main/ firmware/partitions.csv \
        firmware/sdkconfig.defaults firmware/idf_component.yml .gitignore
git commit -m "firmware: ESP-IDF project skeleton for ESP32-S3"
```

---

### Task 12: Display bring-up and panel confirmation

Resolves open question 1 in the spec.

**Files:**
- Create: `firmware/components/gauge_ui/display_config.h`
- Create: `firmware/components/gauge_ui/display.cpp`
- Create: `firmware/components/gauge_ui/CMakeLists.txt`
- Modify: `firmware/main/main.cpp`
- Modify: `firmware/idf_component.yml` (add the BSP and CO5300 driver dependencies)

- [ ] **Step 1: Find the BSP**

```bash
source ~/esp/esp-idf/export.sh
compote component search esp32_s3_touch_amoled
compote component search co5300
```

The board's own firmware contained `components/esp32_s3_touch_amoled_1_75c/` and `managed_components/espressif__esp_lcd_co5300/`, so both exist. If the board component is not in the registry, take it from Waveshare's demo repo for the ESP32-S3-Touch-AMOLED-1.75C and vendor it into `firmware/components/`.

- [ ] **Step 2: Read the header and record the real resolution**

Open the BSP header and find its width/height and panel-init entry point. **Write the actual numbers into `display_config.h`** — this is the one place resolution is defined:

```cpp
#pragma once
// Confirmed against the panel on <date> during Phase 0 bring-up.
namespace gauge_ui {
inline constexpr int kPanelWidth  = 466;   // <- replace with the BSP's value
inline constexpr int kPanelHeight = 466;   // <- replace with the BSP's value
inline constexpr bool kPanelRound = true;
}
```

- [ ] **Step 3: Draw a test pattern**

Four quadrants in distinct colours plus a one-pixel border circle. The border is the point: on a round panel it proves the visible area and reveals any offset or clipping, which a full-screen fill does not.

- [ ] **Step 4: Flash and verify**

Run: `idf.py -p /dev/cu.usbmodem1101 flash monitor`
Expected: four correctly-oriented quadrants, and the border circle fully visible with no flat edge. A clipped border means the configured resolution is wrong — fix `display_config.h` before continuing.

- [ ] **Step 5: Commit**

```bash
git add firmware/components/gauge_ui/ firmware/main/main.cpp firmware/idf_component.yml
git commit -m "firmware: CO5300 display bring-up, panel geometry confirmed"
```

---

### Task 13: Touch, IMU, and the backdrop measurement

Resolves open question 4. **The measurement is the deliverable** — Phase 2's view work is designed against the number this task produces.

**Files:**
- Create: `firmware/components/gauge_platform/touch.{h,cpp}`
- Create: `firmware/components/gauge_platform/imu.{h,cpp}`
- Create: `firmware/components/gauge_platform/CMakeLists.txt`
- Create: `firmware/test/bench/backdrop_bench.cpp`
- Modify: `firmware/main/main.cpp`

**Interfaces:**
- Produces:
  - `struct TouchPoint { int x, y; bool pressed; };` and `std::optional<TouchPoint> touch_read();`
  - `struct ImuSample { float ax, ay, az, gx, gy, gz; };` and `std::optional<ImuSample> imu_read();`

- [ ] **Step 1: Log touch events**

Print every touch with coordinates. Verify a swipe produces a monotonic x-sweep and that the coordinate origin matches the display orientation from Task 12.

- [ ] **Step 2: Log IMU samples**

Print acceleration at 50Hz. **Verify the axes against gravity**: resting flat, one axis must read ≈1g and the others ≈0. Record which axis is which — Task 11's harsh-event work depends on knowing longitudinal from lateral, and getting it wrong silently corrupts the driving score.

- [ ] **Step 3: Measure the backdrop, all three mitigations**

Render the §11 `glow=rim` backdrop and report sustained fps over 10 seconds for each:

1. naive full-screen regeneration every frame
2. rpm quantised into 100rpm buckets, redrawing only on bucket change
3. annulus-only dirty region, centre untouched

Sweep rpm continuously 800→7000 during each run so the measurement reflects real driving, not a static frame.

- [ ] **Step 4: Record the result**

Write the three fps numbers into the spec's "Backdrop mitigation" open question and state which is chosen. If even (1) sustains 30fps, say so — then the mitigation is "none needed" and Phase 2 gets simpler.

- [ ] **Step 5: Commit**

```bash
git add firmware/components/gauge_platform/ firmware/test/bench/backdrop_bench.cpp \
        firmware/main/main.cpp docs/superpowers/specs/2026-08-20-firmware-port-amoled-1-75c-design.md
git commit -m "firmware: touch and IMU bring-up, backdrop performance measured"
```

---

### Task 14: BLE transport and first live link to the car

The end of Phase 0: the board talks to the vLinker with no Mac involved.

**Files:**
- Create: `firmware/components/gauge_platform/ble_transport.{h,cpp}`
- Modify: `firmware/main/main.cpp`

**Interfaces:**
- Consumes: `ITransport` from Task 9
- Produces: `class BleTransport : public gauge::ITransport` — same interface the fake implements, so `Elm327` is unchanged between host tests and the board

- [ ] **Step 1: Scan and connect**

Scan for the vLinker, connect, and discover its notify/write characteristics. Log the UUIDs. Cross-check against `mx5gauge/sources.py`, which already has the working UUIDs from the Mac side.

- [ ] **Step 2: Run the handshake**

Instantiate `gauge::Elm327` over `BleTransport` and call `init()`. Expected: the same handshake that passes in Task 9's host tests now completes against real hardware.

- [ ] **Step 3: Read one live PID**

Request `0x05` (coolant) with the engine running and print the decoded value.
Expected: a plausible coolant temperature that climbs during warm-up — §4 cites a clean 72→95°C, so a value moving through that band is the confirmation.

- [ ] **Step 4: Confirm the full cycle**

Run `discover_supported()`, build the poll cycle, and log a full sweep. Expected: the supported set matches what the simulator reports for the same car.

- [ ] **Step 5: Commit**

```bash
git add firmware/components/gauge_platform/ble_transport.* firmware/main/main.cpp
git commit -m "firmware: NimBLE transport, live ELM327 link to the car"
```

---

### Task 15: Rewrite `SPEC.md` §3 for the actual board

The spec requires this, and it lands **last on purpose**: after Phase 0 the hardware facts are confirmed rather than assumed, so §3 gets rewritten once with real numbers instead of twice with guesses.

**Files:**
- Modify: `SPEC.md` §3 (lines 48–90: "Chosen board", the BOM table, "Power")
- Modify: `SPEC.md` §5 (the module-map table, to point at the real firmware paths)
- Modify: `SPEC.md` §7 roadmap ("Buy the board" is done; mark Phase 0 complete)

- [ ] **Step 1: Replace "Chosen board"**

Waveshare **ESP32-S3-Touch-AMOLED-1.75C**: round AMOLED at the resolution confirmed in Task 12, CO5300 over QSPI, CST9217 capacitive touch, QMI8658 6-axis IMU, 32MB flash, 8MB PSRAM. Keep the existing note that the ESP32-S3 has no Bluetooth Classic and therefore talks BLE — that trade-off is unchanged and still worth recording.

- [ ] **Step 2: Update the BOM**

Change the board line. **Mark the buck converter and fuse tap as optional**, conditional on the car's USB socket test (spec open question 3): if the socket is ignition-switched, plugging into it is a legitimate install and neither part is needed.

- [ ] **Step 3: Note the audio hardware as unused**

One line, so a future session does not rediscover the speaker and mic array and assume they were overlooked.

- [ ] **Step 4: Update §5's module map**

Point each row at the real path (`firmware/components/gauge_core/pid.cpp` and so on) now that the files exist, replacing the aspirational `src/obd/pid.cpp` names.

- [ ] **Step 5: Commit**

```bash
git add SPEC.md
git commit -m "spec: retarget section 3 to the ESP32-S3-Touch-AMOLED-1.75C"
```

---

## Phase 0 exit criteria

- [x] Host tests pass for every `gauge_core` module (Tasks 2–9)
- [x] `replay_check` reports zero divergences across **every** capture in `logs/` (Task 10)
- [ ] Panel resolution confirmed and recorded in `display_config.h` (Task 12)
- [ ] Backup restore path exercised end to end (Task 11, Step 5)
- [ ] Touch, IMU axes, and display all verified on the real board (Tasks 12–13)
- [ ] A real fps number for the backdrop, and a chosen mitigation (Task 13)
- [ ] The board reads live coolant from the car with no Mac attached (Task 14)
- [ ] `SPEC.md` §3 rewritten for the real board (Task 15)
- [ ] **Tommy's test, at the car:** is the USB socket ignition-switched or constant? Plug a charger in, key off, wait a minute. Not a code task — but it settles spec open question 3 and decides whether the buck converter and fuse tap stay in the BOM.

## What this plan deliberately does not cover

**Phase 1's UI and all of Phase 2** are not planned here, and that is a decision rather than an omission. The nine views are a reimplementation, not a port, and their design depends on two facts this plan produces and does not yet have: the confirmed panel geometry, and the measured cost of the §11 backdrop. Writing those tasks now would mean inventing LVGL layouts against a resolution that is still an assumption.

Once Task 13 reports its numbers, the Phase 1–2 plan gets written against measurements instead of guesses.

Also untouched, per the spec: SD logging, Wi-Fi sync, GPS, shift LEDs, enclosure, the permanent 12V install, and the board's audio hardware.

## Milestone: the carousel slides (2026-08-22)

Views now slide instead of cutting. The slide never re-renders: each view is
snapshotted once into PSRAM (`lv_snapshot`), and every frame is a memcpy of two
rectangles plus a direct blit past LVGL via `esp_lv_adapter_dummy_draw_blit()`.
The pixel arithmetic lives in `gauge_core/frame_slide.h` and is host-tested
(`test_frame_slide.cpp`, the 14th suite) — both directions, both endpoints,
mid-slide alignment, every-pixel-written at all offsets, the static band, and
clamping. Two mutations (flipped direction, one pixel shaved) fail 2 and 4
checks respectively, so the test bites.

The banner and page dots are held stationary by taking rows 396–452 from the
destination snapshot: a page indicator that slides off screen tells you nothing.
The instant cut survives as the fallback when the buffers do not fit — a slide
must never be the reason a view is stuck. Boot's 434 KB clip framebuffer is
freed and reused, so the three slide buffers cost nothing at peak.

### What `dummy_draw_blit` does NOT do for you

Three separate bugs, all in the gap between the fast path and the adapter's
normal flush. The flush path quietly does **three** things that the blit path
does not, and adopting the fast path without auditing what it opts out of cost
four flash cycles:

| # | Symptom on the panel | Cause | Fix |
|---|---|---|---|
| 1 | Top half garbage, bottom frozen | A full-frame blit returns `ESP_ERR_NO_MEM`; the window is set, no data follows | Blit in 50-row bands (`kBlitRows`) |
| 2 | Whole screen pink | No RGB565 byte swap; the flush path does it guarded on this exact panel interface (`lvgl_bridge_v9.c`) | `lv_draw_sw_rgb565_swap` on each snapshot, once |
| 3 | (none observed) | No cache writeback before DMA; the flush path calls `display_cache_msync_range` | `esp_cache_msync(..., C2M \| UNALIGNED)` per frame |

Fix 3 is correct on its own merits but fixed nothing observable here. Recorded
as such rather than left to look like part of the cure.

**The blit's return value was being discarded.** Bug 1 was a total failure of
every frame that read as a rendering artifact, and a cache-coherency theory got
a whole flash cycle before anyone checked whether the new call succeeded. Check
the return value of the call you just introduced, first.

**The transfer ceiling is a heap artefact, not a constant.** `slide_selftest()`
probes it:

    466 rows (434312 B): ESP_ERR_NO_MEM
    233 rows (217156 B): ESP_ERR_NO_MEM
    156 rows (145392 B): ESP_OK, and every smaller size

The boundary is the size of a bounce buffer the SPI driver can allocate from
internal RAM, so the largest size that works today fails under a fragmented
heap. 50 rows is what the adapter's own flush path uses every frame.

`slide_selftest()` stays in the tree, uncalled. It answered two questions no
serial log could — the transfer limit, and the byte order, the latter by
painting RED/GREEN/BLUE/WHITE bands and having a human report BLUE/RED/GREEN/
WHITE. Panel transfer limits will come up again with the §11 backdrop.

### Measured, and short of the estimate

    slide: snap 133-174ms  5fr 289ms (17fps)  sync=ESP_OK blit=ESP_OK

The design was pitched at ~45 fps with a ~60 ms snapshot pause, from the panel's
raw bandwidth (434 KB over QSPI at 40 MHz × 4 lanes ≈ 22 ms). **The real figures
are 17 fps and a 105–174 ms pause** — ~58 ms per frame, and the 240 ms slide
overruns to 290 ms. The estimate assumed one full-frame transfer at bus speed;
the reality is ten banded transfers each waiting for its own completion, plus a
snapshot that renders into PSRAM far slower than allowed for.

Open lead, not yet measured: each 50-row band is 46,600 B, which is not a
multiple of 64, so both the length and every band start after the first are
misaligned — likely forcing the SPI driver to bounce each band through internal
RAM. **48-row bands** (44,736 B) would be 64-aligned in both length and start
for 9 of the 10 bands. Pipelining the bands (wait only on the last) is the other
obvious win but carries a real race: an earlier band's completion notification
can satisfy the last band's wait, releasing the buffer while a transfer is still
reading it.

Diagnostics are **latched** (`gauge_ui::slide_note()`) and reprinted on every
status line. Printing them only at the swipe meant every serial capture that
opened a moment late lost them, and the same question had to be asked again.

### Slide performance: what was tried, measured, and kept (2026-08-22)

| Change | Result | Kept |
|---|---|---|
| 48-row bands, cache-line aligned | Identical: 5 fr, 17 fps, 38 ms blit | No — theory refuted, reverted to 50 |
| Move only rows 16–396 | 5→6 fr, 17→21 fps, comp 20→17 ms, blit 38→30 ms | Yes |
| Snapshot outgoing view at press | Post-gesture delay 142–181 ms → 46–89 ms, no swipes missed | Yes |

The alignment result matters beyond itself: because aligning the bands changed
nothing, the SPI driver is **not** bouncing them through internal RAM, so the
earlier bounce-buffer explanation for the 217 KB `ESP_ERR_NO_MEM` ceiling is
wrong and the real limit is still unexplained. Do not pick a band size larger
than 50 on the strength of that reasoning.

`sync` measures 0 ms, which confirms `esp_cache_msync` is a no-op for this
memory — independent evidence that the cache fix (bug 3 above) fixed nothing.

**Remaining floor, and it is a design floor, not a bug.** A push-slide has to
have both views rendered before it can start, and each `lv_snapshot` is a full
screen render (~50–90 ms) during which nothing moves. Taking one at press time
splits that dead time either side of the gesture but does not remove it; the
user still reports a stutter before motion. Removing it means giving up the
push: reveal the incoming view under a growing clip instead, which needs no
snapshot at all and could use LVGL's normal flush path, at the cost of the
outgoing view no longer moving. Not done — the current behaviour was accepted.
