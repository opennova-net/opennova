// Bit-level LSB-first stream reader and writer.
//
// The shape is lifted from engine/formats/cpt's CDEP/POLY bit codec (the proven
// consumer), and cpt reads through io::BitReader and writes through
// io::BitWriter; its byte-exact re-encode of the shipped form (cpt_shipped_form)
// pins the writer's bytes and its high-water rule.
//
// Layout contract: values pack LSB-first within a little-endian dword stream;
// align_dword() pads to the next 4-byte boundary (a partial byte first).

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include <base/io/le.h>

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

// The writer over a zero-filled buffer that grows in 0x4000-byte steps. A field
// is written at the cursor's current width (set_bit_width) as one little-endian
// dword read-modify-write, so the cursor's bit offset plus the width stays
// within 32 (every width up to 25 bits is exact at any offset). Unwritten bytes
// stay zero, so the align pads are zero.
class BitWriter {
public:
    void set_bit_width(int num_bits)
    {
        bit_width_ = static_cast<uint32_t>(num_bits);
        bitmask_ = (num_bits >= 32) ? 0xFFFFFFFFu : ((1u << num_bits) - 1u);
    }

    void write_bits(uint32_t value)
    {
        ensure_capacity(byte_pos_ + 32u);
        uint8_t *dst = buffer_.data() + byte_pos_;
        const uint32_t cur = read_u32_le(dst);
        const uint32_t mask = bitmask_ << bit_pos_;
        write_u32_le(dst, ((value & bitmask_) << bit_pos_) | (cur & ~mask));

        const uint32_t total = bit_pos_ + bit_width_;
        byte_pos_ += total >> 3;
        bit_pos_ = total & 7u;
        // A bit write counts the byte the cursor rests in, even when it rests
        // on a byte boundary with nothing written there yet.
        high_water_ = std::max(high_water_, byte_pos_ + 1u);
    }

    void write_field(int num_bits, uint32_t value)
    {
        set_bit_width(num_bits);
        write_bits(value);
    }

    // Raw bytes, starting on the next whole byte.
    void write_bytes(const void *data, size_t size)
    {
        ensure_capacity(byte_pos_ + size + 32u);
        advance_byte();
        std::memcpy(buffer_.data() + byte_pos_, data, size);
        byte_pos_ += size;
        high_water_ = std::max(high_water_, byte_pos_);
    }

    void advance_byte()
    {
        if (bit_pos_) {
            bit_pos_ = 0;
            ++byte_pos_;
        }
    }

    // Moves the cursor only: the pad becomes part of the output once a later
    // write lands past it.
    void align_dword()
    {
        advance_byte();
        if (byte_pos_ & 3u) {
            byte_pos_ = (byte_pos_ + 3u) & ~static_cast<size_t>(3);
        }
    }

    // The bytes through the high-water mark, then `zero_tail` zero bytes.
    std::vector<uint8_t> bytes(size_t zero_tail = 0) const
    {
        std::vector<uint8_t> out(buffer_.begin(),
                                 buffer_.begin() + static_cast<std::ptrdiff_t>(high_water_));
        out.resize(high_water_ + zero_tail, 0);
        return out;
    }

private:
    void ensure_capacity(size_t needed)
    {
        while (needed >= buffer_.size()) {
            buffer_.resize(buffer_.size() + 0x4000, 0);
        }
    }

    std::vector<uint8_t> buffer_ = std::vector<uint8_t>(0x4000, 0);
    size_t byte_pos_ = 0;
    size_t high_water_ = 0;
    uint32_t bit_pos_ = 0;
    uint32_t bit_width_ = 0;
    uint32_t bitmask_ = 0;
};

} // namespace io
} // namespace opennova
