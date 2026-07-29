// Bit-level LSB-first stream reader/writer.
//
// The shape is lifted from libs/cpt's CDEP bit codec (the proven consumer);
// shipped here for NEW code.
//
// cpt still has its own copy, and the two have since DIVERGED. Both track a
// high-water mark; what differs is the surface each grew for its own consumer:
// cpt's writer has a normalizing set_position (a bit_offset > 8 folds into
// byte+bit) and a write_to_file, and its reader has a remaining_bits used to
// bound declared counts, while this one has byte_position instead. So they are
// not interchangeable: adopting this header in cpt is a real migration that
// has to be byte-diffed against the CPT corpus (tests/terrain's
// parametric_parity_test does exactly that), not a swap. (Verified 2026-07-28,
// quality campaign W2-4/W2-7 — the earlier "token-identical copy" note was
// wrong, and so was W2-4's account of which class held what.)
//
// Layout contract: values pack LSB-first within a little-endian dword stream;
// align_dword() pads to the next 4-byte boundary (a partial byte first).

#ifndef OPENNOVA_IO_BIT_STREAM_H
#define OPENNOVA_IO_BIT_STREAM_H

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include <io/le.h>

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
    size_t byte_position() const { return byte_pos_; }

private:
    const uint8_t *data_;
    size_t size_;
    size_t byte_pos_ = 0;
    int bit_pos_ = 0;
};

class BitWriter {
public:
    explicit BitWriter(size_t initial_capacity = 0x4000) : buffer_(initial_capacity, 0) {}

    void set_bit_width(int num_bits)
    {
        bit_width_ = static_cast<uint32_t>(num_bits);
        bitmask_ = (num_bits >= 32) ? 0xFFFFFFFFu : ((1u << num_bits) - 1u);
    }

    void write_bits(uint32_t value)
    {
        ensure_capacity(byte_pos_ + 32u);
        // Byte-wise little-endian read-modify-write. This was a dword store through a
        // reinterpret_cast at an arbitrary byte offset: strict-aliasing and alignment
        // UB that merely happened to work on x86, and that UBSan flags. io/le.h is
        // also the layout contract — the stream packs LSB-first within a
        // little-endian dword — so being explicit makes the codec correct rather
        // than accidentally correct. Identical bytes on a little-endian host.
        uint8_t *dst = buffer_.data() + byte_pos_;
        const uint32_t cur = read_u32_le(dst);
        const uint32_t mask = bitmask_ << bit_pos_;
        write_u32_le(dst, ((value & bitmask_) << bit_pos_) | (cur & ~mask));

        const uint32_t total_bits = bit_pos_ + bit_width_;
        byte_pos_ += total_bits >> 3;
        bit_pos_ = total_bits & 7u;
        if (byte_pos_ + 1u > high_water_) {
            high_water_ = byte_pos_ + 1u;
        }
    }

    void write_field(int num_bits, uint32_t value)
    {
        set_bit_width(num_bits);
        write_bits(value);
    }

    void write_bytes(const void *data, size_t size)
    {
        ensure_capacity(byte_pos_ + static_cast<uint32_t>(size) + 32u);
        if (bit_pos_) {
            bit_pos_ = 0;
            ++byte_pos_;
        }
        std::memcpy(buffer_.data() + byte_pos_, data, size);
        byte_pos_ += static_cast<uint32_t>(size);
        if (byte_pos_ > high_water_) {
            high_water_ = byte_pos_;
        }
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
            byte_pos_ = (byte_pos_ + 3u) & ~3u;
        }
    }

    const uint8_t *data() const { return buffer_.data(); }
    // Bytes written, counting a trailing partial byte.
    uint32_t high_water() const { return high_water_; }

private:
    void ensure_capacity(uint32_t needed)
    {
        if (buffer_.empty()) {
            buffer_.resize(0x4000, 0);
        }
        while (needed >= buffer_.size()) {
            buffer_.resize(buffer_.size() + 0x4000, 0);
        }
    }

    std::vector<uint8_t> buffer_;
    uint32_t bit_width_ = 0;
    uint32_t byte_pos_ = 0;
    uint32_t bit_pos_ = 0;
    uint32_t bitmask_ = 0;
    uint32_t high_water_ = 0;
};

} // namespace io
} // namespace opennova

#endif // OPENNOVA_IO_BIT_STREAM_H
