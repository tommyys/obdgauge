#pragma once
#include <deque>
#include <string>
#include <vector>
#include "transport.h"

struct FakeTransport : gauge::ITransport {
    std::vector<std::string> written;
    std::deque<std::string> replies;
    int delays = 0;

    bool write(const std::string& s) override { written.push_back(s); return true; }
    std::string read(int) override {
        if (replies.empty()) return "";
        std::string r = replies.front();
        replies.pop_front();
        return r;
    }
    void delay_ms(int) override { ++delays; }
};
