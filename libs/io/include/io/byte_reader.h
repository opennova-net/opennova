// Bounds-checked little-endian byte cursor.
//
// Semantics mirror the parser cursors this replaces (libs/mission's BMS
// Reader): out-of-range reads return 0 and do NOT advance; bulk reads
// zero-fill on overflow; skip clamps to the end. Format parsers rely on
// those exact recovery semantics, so they are the contract here.
//
// ok() reports whether any read has run past the end. It is OBSERVATIONAL:
// it never changes what a read returns or whether the cursor advances, so
// adding it cannot alter an existing parser's recovery path. It exists
// because the lenient contract above cannot distinguish a truncated field
// from a legitimate zero -- protocol decoders need that distinction, and
// each was carrying its own cursor to get it. A decoder that must STOP at
// the first truncation (rather than keep reading) wants a latching cursor
// instead; see libs/npwire/src/wire_cursor.h.

#ifndef OPENNOVA_IO_BYTE_READER_H
#define OPENNOVA_IO_BYTE_READER_H

#include <cstdint>
#include <cstring>
#include <vector>

#include <io/fixed.h>
#include <io/le.h>

namespace opennova {
namespace io {

class ByteReader {
public:
    ByteReader(const uint8_t *data, size_t size) : data_(data), size_(size), pos_(0) {}

    bool has_bytes(size_t count) const { return count <= size_ - pos_; }
    size_t position() const { return pos_; }
    size_t remaining() const { return size_ - pos_; }

    // False once any read or skip has been clipped by the end of the buffer.
    bool ok() const { return ok_; }
    // For callers that detect a SEMANTIC error (a bad magic, an impossible
    // count) and want it to travel with the cursor's own truncation state.
    void mark_failed() { ok_ = false; }

    uint8_t read_u8()
    {
        if (!has_bytes(1)) { ok_ = false; return 0; }
        return data_[pos_++];
    }

    int8_t read_i8() { return (int8_t)read_u8(); }

    uint16_t read_u16()
    {
        if (!has_bytes(2)) { ok_ = false; return 0; }
        uint16_t v = read_u16_le(data_ + pos_);
        pos_ += 2;
        return v;
    }

    int16_t read_i16() { return (int16_t)read_u16(); }

    uint32_t read_u32()
    {
        if (!has_bytes(4)) { ok_ = false; return 0; }
        uint32_t v = read_u32_le(data_ + pos_);
        pos_ += 4;
        return v;
    }

    int32_t read_i32() { return (int32_t)read_u32(); }

    float read_f32()
    {
        uint32_t v = read_u32();
        float f;
        std::memcpy(&f, &v, sizeof(f));
        return f;
    }

    float read_fixed16() { return fp16_16_to_float(read_i32()); }

    void read_bytes(uint8_t *out, size_t count)
    {
        if (!has_bytes(count)) {
            ok_ = false;
            std::memset(out, 0, count);
            return;
        }
        std::memcpy(out, data_ + pos_, count);
        pos_ += count;
    }

    void read_bytes(std::vector<uint8_t> &out, size_t count)
    {
        out.resize(count);
        read_bytes(out.data(), count);
    }

    // Fixed-size string field (preserves all bytes for roundtrip).
    void read_fixed_string(char *out, size_t max_len)
    {
        read_bytes(reinterpret_cast<uint8_t *>(out), max_len);
    }

    void skip(size_t count)
    {
        if (count > size_ - pos_) {
            ok_ = false;
            pos_ = size_;
        } else {
            pos_ += count;
        }
    }

private:
    const uint8_t *data_;
    size_t size_;
    size_t pos_;
    bool ok_ = true;
};

} // namespace io
} // namespace opennova

#endif // OPENNOVA_IO_BYTE_READER_H
