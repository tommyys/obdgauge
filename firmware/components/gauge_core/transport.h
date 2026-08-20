// The seam between the ELM327 conversation (pure, host-tested) and the
// bytes on the wire (NimBLE on the board, bleak on the Mac).
#pragma once
#include <string>

namespace gauge {

struct ITransport {
    virtual ~ITransport() = default;
    // Send a command. False if the link is down.
    virtual bool write(const std::string& text) = 0;
    // Read one reply, up to the ELM327 '>' prompt. "" on timeout.
    virtual std::string read(int timeout_ms) = 0;
    // Pause between commands. Kept on the transport so gauge_core needs no
    // clock and no ESP-IDF header.
    virtual void delay_ms(int) {}
};

}  // namespace gauge
