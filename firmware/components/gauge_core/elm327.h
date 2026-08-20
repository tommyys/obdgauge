// The ELM327 conversation. Pure logic over an abstract transport, so the
// handshake that works against the vLinker in the car is the same code the
// host tests exercise against a fake.
// Split out of mx5gauge/sources.py.
#pragma once
#include <optional>
#include <set>
#include <string>
#include "parse.h"
#include "transport.h"

namespace gauge {

class Elm327 {
  public:
    explicit Elm327(ITransport& t) : t_(t) {}

    // Standard bring-up sequence. The order and the waits are what has been
    // proven against the vLinker - SPEC.md section 3 warns that cheap clones
    // are flaky, so this is load-bearing.
    bool init();

    // Query the supported-PID bitmasks across all four blocks.
    std::set<uint8_t> discover();

    // One mode-01 request; the decoded payload, or absent.
    std::optional<Bytes> request(uint8_t pid);

    // ATRV - an adapter command rather than a PID.
    std::optional<double> read_voltage();

    // Mode 09 PID 02. Empty when the car will not say.
    std::string read_vin();

    // Send a raw command and return the cleaned reply.
    std::string cmd(const std::string& text, int timeout_ms = 4000);

  private:
    ITransport& t_;
};

}  // namespace gauge
