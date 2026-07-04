// Growable little-endian byte sink, the write-side counterpart of
// io/byte_reader.h (semantics lifted from libs/mission's BMS Writer).

#ifndef OPENNOVA_IO_BYTE_WRITER_H
#define OPENNOVA_IO_BYTE_WRITER_H

#include <cstdint>
#include <cstring>
#include <vector>

#include <io/fixed.h>

namespace opennova {
namespace io {

class ByteWriter {
public:
    void write_u8(uint8_t v) { data_.push_back(v); }
    void write_i8(int8_t v) { write_u8((uint8_t)v); }

    void write_u16(uint16_t v)
    {
        data_.push_back((uint8_t)(v & 0xFF));
        data_.push_back((uint8_t)((v >> 8) & 0xFF));
    }

    void write_i16(int16_t v) { write_u16((uint16_t)v); }

    void write_u32(uint32_t v)
    {
        data_.push_back((uint8_t)(v & 0xFF));
        data_.push_back((uint8_t)((v >> 8) & 0xFF));
        data_.push_back((uint8_t)((v >> 16) & 0xFF));
        data_.push_back((uint8_t)((v >> 24) & 0xFF));
    }

    void write_i32(int32_t v) { write_u32((uint32_t)v); }

    void write_f32(float f)
    {
        uint32_t v;
        std::memcpy(&v, &f, sizeof(v));
        write_u32(v);
    }

    void write_fixed16(float f) { write_i32(float_to_fp16_16(f)); }

    void write_bytes(const uint8_t *src, size_t count)
    {
        data_.insert(data_.end(), src, src + count);
    }

    void write_bytes(const std::vector<uint8_t> &src)
    {
        data_.insert(data_.end(), src.begin(), src.end());
    }

    // Fixed-size string field: writes exactly max_len bytes, zero-padded.
    void write_fixed_string(const char *src, size_t max_len)
    {
        size_t i = 0;
        for (; i < max_len && src[i] != '\0'; ++i)
            data_.push_back((uint8_t)src[i]);
        for (; i < max_len; ++i)
            data_.push_back(0);
    }

    size_t size() const { return data_.size(); }
    const std::vector<uint8_t> &data() const { return data_; }
    std::vector<uint8_t> take() { return std::move(data_); }

private:
    std::vector<uint8_t> data_;
};

} // namespace io
} // namespace opennova

#endif // OPENNOVA_IO_BYTE_WRITER_H
