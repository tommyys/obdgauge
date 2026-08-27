// A stand-in for the real part, with the two behaviours that matter:
// a write can only clear bits, and only an erase sets them back to 1.
// Without those, a test can "pass" doing something the flash would refuse.
#pragma once
#include <cstring>
#include <vector>
#include "logbuf.h"

namespace gauge_test {

class FakeFlash : public gauge::IFlash {
public:
    explicit FakeFlash(size_t sectors)
        : sectors_(sectors), data_(sectors * gauge::kSectorSize, 0xFF) {}

    size_t sector_count() const override { return sectors_; }

    bool read(size_t off, void* dst, size_t len) override {
        if (off + len > data_.size()) return false;
        memcpy(dst, data_.data() + off, len);
        ++reads_;
        return true;
    }

    bool write(size_t off, const void* src, size_t len) override {
        if (off + len > data_.size()) return false;
        if (off % 4 || len % 4) return false;      // the real part demands this
        const uint8_t* s = static_cast<const uint8_t*>(src);
        for (size_t i = 0; i < len; ++i) {
            // A write ANDs. Setting a cleared bit needs an erase.
            if (s[i] & ~data_[off + i]) return false;
            data_[off + i] &= s[i];
        }
        ++writes_;
        return true;
    }

    bool erase_sector(size_t index) override {
        if (index >= sectors_) return false;
        memset(data_.data() + index * gauge::kSectorSize, 0xFF, gauge::kSectorSize);
        ++erases_;
        return true;
    }

    // Test-only reach-ins.
    uint8_t* raw(size_t sector) { return data_.data() + sector * gauge::kSectorSize; }
    size_t erases() const { return erases_; }
    size_t writes() const { return writes_; }
    // Simulates losing power part-way through an erase: some bytes went to
    // 0xFF, the rest did not.
    void half_erase(size_t index, size_t bytes) {
        memset(raw(index), 0xFF, bytes);
    }

private:
    size_t sectors_;
    std::vector<uint8_t> data_;
    size_t reads_ = 0, writes_ = 0, erases_ = 0;
};

}  // namespace gauge_test
