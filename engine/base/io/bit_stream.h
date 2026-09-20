// Bit-level LSB-first stream reader.
//
// The shape is lifted from engine/formats/cpt's CDEP bit codec (the proven
// consumer), and cpt consumes io::BitReader (its remaining_bits bound moved
// here). The matching writer stays cpt's own (cpt_io.cpp): it carries a
// normalizing set_position (a bit_offset > 8 folds into byte+bit) and a
// write_to_file.
//
// Layout contract: values pack LSB-first within a little-endian dword stream;
// align_dword() pads to the next 4-byte boundary (a partial byte first).

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace opennova {
namespace io {

class BitReader {
public:
    BitReader(const uint8_t *data, size_t size) : data_(data), size_(size) {}

    uint32_t read_bits(int num_bits)
    {
        if (num_bits <= 0) {
            return 0;
        }
        if (byte_pos_ + 4 <= size_) {
            uint32_t value = 0;
            std::memcpy(&value, data_ + byte_pos_, sizeof(uint32_t));
            const uint32_t mask = (num_bits < 32) ? ((1u << num_bits) - 1u) : 0xFFFFFFFFu;
            const uint32_t result = (value >> bit_pos_) & mask;
            const uint32_t total = bit_pos_ + static_cast<uint32_t>(num_bits);
            byte_pos_ += total >> 3;
            bit_pos_ = static_cast<int>(total & 7u);
            return result;
        }

        // Tail path: assemble byte by byte so reads near the end stay exact.
        uint32_t result = 0;
        int shift = 0;
        int remaining = num_bits;
        while (remaining > 0 && byte_pos_ < size_) {
            const int available = 8 - bit_pos_;
            const int take = std::min(remaining, available);
            const uint32_t mask = (1u << take) - 1u;
            result |= ((data_[byte_pos_] >> bit_pos_) & mask) << shift;
            shift += take;
            remaining -= take;
            byte_pos_ += static_cast<size_t>((bit_pos_ + take) / 8);
            bit_pos_ = (bit_pos_ + take) % 8;
        }
        return result;
    }

    void advance_byte()
    {
        if (bit_pos_) {
            bit_pos_ = 0;
            ++byte_pos_;
        }
    }

    void align_dword()
    {
        if (bit_pos_) {
            bit_pos_ = 0;
            ++byte_pos_;
        }
        if (byte_pos_ & 3u) {
            byte_pos_ = (byte_pos_ + 3u) & ~static_cast<size_t>(3);
        }
    }

    bool at_end() const { return byte_pos_ >= size_; }

    // Bits still readable from the cursor. Lets a decoder reject a declared
    // count the section cannot possibly encode BEFORE it allocates for it.
    uint64_t remaining_bits() const
    {
        if (byte_pos_ >= size_) {
            return 0;
        }
        return (static_cast<uint64_t>(size_ - byte_pos_) * 8u) -
               static_cast<uint64_t>(bit_pos_);
    }

private:
    const uint8_t *data_;
    size_t size_;
    size_t byte_pos_ = 0;
    int bit_pos_ = 0;
};

} // namespace io
} // namespace opennova
