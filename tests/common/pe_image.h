// A Win32 PE image's initialized data by virtual address, read only: the
// compatibility legs that check a port against the program's own static tables
// (the installed Jointops.exe) read it this way. No loader and no relocation:
// a section's raw bytes sit at its image virtual address.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "common/file_io.h"

namespace pe {

class Image {
public:
    bool open(const std::string &path) {
        bytes_.clear();
        sections_.clear();
        if (!test_io::read_file(path, bytes_) || bytes_.size() < 0x40) return false;
        const uint32_t nt = le32(0x3C);
        if (nt + 24 > bytes_.size() || bytes_[nt] != 'P' || bytes_[nt + 1] != 'E' || bytes_[nt + 2] != 0 ||
            bytes_[nt + 3] != 0)
            return false;
        const uint16_t count = le16(nt + 6);
        const uint16_t optional_size = le16(nt + 20);
        const uint32_t optional = nt + 24;
        if (optional + 32 > bytes_.size() || le16(optional) != 0x10B) return false;  // PE32
        base_ = le32(optional + 28);
        const uint32_t table = optional + optional_size;
        for (uint16_t i = 0; i < count; ++i) {
            const uint32_t s = table + 40u * i;
            if (s + 40 > bytes_.size()) return false;
            Section section;
            section.va = base_ + le32(s + 12);
            section.raw = le32(s + 20);
            section.raw_size = le32(s + 16);
            sections_.push_back(section);
        }
        return !sections_.empty();
    }

    // `n` bytes at `va`; false when any of them lies outside a section's raw data.
    bool read(uint32_t va, size_t n, std::vector<uint8_t> &out) const {
        for (const Section &s : sections_) {
            if (va < s.va || va - s.va >= s.raw_size) continue;
            const size_t offset = va - s.va;
            if (offset + n > s.raw_size || s.raw + offset + n > bytes_.size()) return false;
            out.assign(bytes_.begin() + static_cast<std::ptrdiff_t>(s.raw + offset),
                       bytes_.begin() + static_cast<std::ptrdiff_t>(s.raw + offset + n));
            return true;
        }
        return false;
    }

    bool u32(uint32_t va, uint32_t &out) const {
        std::vector<uint8_t> b;
        if (!read(va, 4, b)) return false;
        out = static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) |
              (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
        return true;
    }

    // The NUL-terminated string at `va` (at most `max` bytes); "" when unreadable.
    std::string c_string(uint32_t va, size_t max = 256) const {
        std::string out;
        std::vector<uint8_t> b;
        for (size_t i = 0; i < max && read(va + static_cast<uint32_t>(i), 1, b) && b[0] != 0; ++i)
            out.push_back(static_cast<char>(b[0]));
        return out;
    }

private:
    struct Section {
        uint32_t va = 0;
        uint32_t raw = 0;
        uint32_t raw_size = 0;
    };

    uint16_t le16(size_t at) const {
        return static_cast<uint16_t>(bytes_[at] | (bytes_[at + 1] << 8));
    }
    uint32_t le32(size_t at) const {
        return static_cast<uint32_t>(bytes_[at]) | (static_cast<uint32_t>(bytes_[at + 1]) << 8) |
               (static_cast<uint32_t>(bytes_[at + 2]) << 16) | (static_cast<uint32_t>(bytes_[at + 3]) << 24);
    }

    std::vector<uint8_t> bytes_;
    std::vector<Section> sections_;
    uint32_t base_ = 0;
};

}  // namespace pe
