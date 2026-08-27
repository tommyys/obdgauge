// The CRC-32 convention between the board and tools/pull_drives.py.
//
// `GET` ends with a checksum the puller verifies with Python's
// binascii.crc32. If the two conventions ever disagree, EVERY pull of EVERY
// drive fails identically -- and the failure looks exactly like a flaky USB
// cable, which is what the board runbook used to tell the reader to go and
// check. Nothing asserted the agreement: the Python test generated both sides
// of its own fixture, so it could not have caught a mismatch.
//
// This pins the C side to the CRC-32 standard check value, published outside
// this project: CRC-32 of the ASCII string "123456789" is 0xCBF43926.
// tests/test_pull_drives.py pins Python to the same literal. Neither test is
// derived from the other; they meet at the constant.
#include <cstring>
#include <string>

#include "check.h"
#include "crc32.h"

using gauge_test::check;

static void test_standard_check_value() {
    const char* v = "123456789";
    check("crc32 of '123456789' is the standard check value",
          (int)(int32_t)gauge::crc32(0, v, strlen(v)), (int)(int32_t)0xCBF43926u);
}

static void test_empty_input_is_the_seed() {
    check("crc32 of nothing is 0", (int)gauge::crc32(0, "", 0), 0);
}

static void test_chaining_matches_one_shot() {
    // emit() in serial_cmd.cpp feeds the CRC one sector at a time, so a split
    // buffer must give the same answer as the whole. binascii.crc32's second
    // argument is the same contract.
    const char* v = "123456789";
    const uint32_t split = gauge::crc32(gauge::crc32(0, v, 4), v + 4, 5);
    check("crc32 chains across a split buffer",
          (int)(int32_t)split, (int)(int32_t)gauge::crc32(0, v, 9));
}

static void test_a_second_known_vector() {
    // A second published vector, so a table typo that happens to fix up on
    // one input cannot pass: CRC-32("The quick brown fox jumps over the lazy
    // dog") == 0x414FA339.
    const char* v = "The quick brown fox jumps over the lazy dog";
    check("crc32 of the pangram", (int)(int32_t)gauge::crc32(0, v, strlen(v)),
          (int)(int32_t)0x414FA339u);
}

int main() {
    test_standard_check_value();
    test_empty_input_is_the_seed();
    test_chaining_matches_one_shot();
    test_a_second_known_vector();
    return gauge_test::check_report();
}
