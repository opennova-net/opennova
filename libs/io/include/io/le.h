// Little-endian primitive reads/writes over raw byte pointers.
//
// The shared home for the per-lib read_u32_le/read_f32_le helpers that every
// format parser used to roll locally. Callers are responsible for bounds; for
// a bounds-checked cursor use io/byte_reader.h.

#ifndef OPENNOVA_IO_LE_H
#define OPENNOVA_IO_LE_H

#include <cstdint>
#include <cstring>

namespace opennova {
namespace io {

inline uint8_t read_u8(const uint8_t *p)
{
    return p[0];
}

inline uint16_t read_u16_le(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

inline int16_t read_s16_le(const uint8_t *p)
{
    return (int16_t)read_u16_le(p);
}

inline uint32_t read_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

inline int32_t read_s32_le(const uint8_t *p)
{
    return (int32_t)read_u32_le(p);
}

inline float read_f32_le(const uint8_t *p)
{
    uint32_t v = read_u32_le(p);
    float f;
    std::memcpy(&f, &v, sizeof(f));
    return f;
}

inline void write_u16_le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

inline void write_u32_le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

inline void write_f32_le(uint8_t *p, float f)
{
    uint32_t v;
    std::memcpy(&v, &f, sizeof(v));
    write_u32_le(p, v);
}

} // namespace io
} // namespace opennova

#endif // OPENNOVA_IO_LE_H
