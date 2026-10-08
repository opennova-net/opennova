#include "material_texture.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace opennova::renderer {
namespace {
bool dimensions(uint32_t w, uint32_t h, uint32_t d) {
    return w && h && d && w <= 32767 && h <= 32767 && d <= 32767 &&
        uint64_t(w)*h*d <= std::numeric_limits<size_t>::max()/4;
}
uint32_t word(const uint8_t *p) { return uint32_t(p[0]) | uint32_t(p[1])<<8 | uint32_t(p[2])<<16 | uint32_t(p[3])<<24; }
struct Span { const uint8_t *p = nullptr; size_t n = 0; };
// The payload [offset, offset + length) of the first chunk tagged `tag` in [begin, end) of the
// container `read` serves, by the chunk headers alone: each a tag and a size whose low 24 bits are
// the payload's length and whose high bit makes it a container, searched before the chunks after it.
bool find_chunk_at(const MaterialChunkReader &read, uint64_t begin, uint64_t end, const char *tag,
        int depth, uint64_t &offset, uint64_t &length) {
    if (depth > 64) return false;
    while (end - begin >= 8) {
        uint8_t header[8];
        if (!read(begin, header, sizeof(header))) return false;
        const uint32_t size = word(header+4), chunk = size & 0xFFFFFFu;
        if (chunk > end-begin-8) return false;
        if (!std::memcmp(header,tag,4)) { offset = begin+8; length = chunk; return true; }
        if ((size & 0x80000000u) && find_chunk_at(read,begin+8,begin+8+chunk,tag,depth+1,offset,length)) return true;
        begin += 8+chunk;
    }
    return false;
}
Span find_chunk(Span s, const char *tag) {
    const MaterialChunkReader memory = [&s](uint64_t at, uint8_t *out, size_t n) {
        if (at > s.n || n > s.n-at) return false;
        std::memcpy(out,s.p+at,n);
        return true;
    };
    uint64_t offset = 0, length = 0;
    if (!find_chunk_at(memory,0,s.n,tag,0,offset,length)) return {};
    return {s.p+offset,size_t(length)};
}
const char *chunk_tag(uint8_t type) { return type==16?"NQ8B":type==17?"HRZ8":"AOC8"; }
// What the payload's first 28 bytes say: its width, height and depth, which the payload's length
// must hold (4 bytes a pixel for an NQ8B, 1 for the others).
bool chunk_holds(const uint8_t *head, uint64_t length, uint8_t type, uint32_t &w, uint32_t &h, uint32_t &d) {
    if (length<28) return false;
    w=word(head+12); h=word(head+16); d=type==17?word(head+20):1;
    if (!dimensions(w,h,d)) return false;
    const uint64_t count=uint64_t(w)*h*d, bytes=count*(type==16?4:1);
    return bytes<=length-28;
}
}
// Sixteen azimuth slices; source alpha, quarter-size XY, 256 wrapped samples.
// Direction Y and distance step round to binary32; the X accumulator stays PC53.
// [orig: Texture_GenerateEnvironmentMap @0x58A220..0x58A42B]
MaterialTexturePixels horizon_volume_from_height(const uint8_t *source, uint32_t w, uint32_t h) {
    MaterialTexturePixels result;
    if (!source || !dimensions(w,h,1) || w<4 || h<4) return result;
    result.width=w>>2; result.height=h>>2; result.depth=16;
    result.rgba.resize(size_t(result.width)*result.height*16*4);
    size_t offset=0;
    for (int slice=0;slice<16;++slice) {
        const double angle=double(slice*2)*0.1963493824005127;
        const double cosine=std::cos(angle), sine=std::sin(angle);
        const double inv=1.0/std::max(std::abs(cosine),std::abs(sine));
        const double dx=cosine*inv, raw_dy=sine*inv;
        const float dy=float(raw_dy), step=float(std::sqrt(dx*dx+raw_dy*raw_dy));
        for (uint32_t y=0;y<result.height;++y) for (uint32_t x=0;x<result.width;++x) {
            double px=double(x*w)/result.width, py=double(y*h)/result.height, distance=0, slope=0;
            const double base=source[(size_t(int32_t(py))*w+int32_t(px))*4+3];
            for (int sample=0;sample<256;++sample) {
                px+=dx; py+=dy; distance+=step;
                int32_t sx=int32_t(px+0.5),sy=int32_t(py+0.5);
                sx=(sx%int32_t(w)+int32_t(w))%int32_t(w); sy=(sy%int32_t(h)+int32_t(h))%int32_t(h);
                slope=std::max(slope,(double(source[(size_t(sy)*w+sx)*4+3])-base)/distance);
            }
            const uint8_t value=uint8_t(std::max(0,int32_t(std::sin(std::atan(slope*0.10000000149011612))*255.0)));
            for (int lane=0;lane<4;++lane) result.rgba[offset++]=value;
        }
    }
    return result;
}
// Retail computes temporary sample directions but fills the final image white.
// [orig: Texture_GenerateNormalMapFromSphereSamples @0x58CB90..0x58CDF5]
MaterialTexturePixels ambient_occlusion_from_height(const uint8_t *source, uint32_t w, uint32_t h) {
    if (!source || !dimensions(w,h,1)) return {};
    return {w,h,1,std::vector<uint8_t>(size_t(w)*h*4,255)};
}
MaterialTexturePixels load_material_chunk(const uint8_t *data, size_t size, uint8_t type) {
    if (!data || size<8 || (type!=16 && type!=17 && type!=18)) return {};
    const Span payload=find_chunk({data+8,size-8},chunk_tag(type));
    uint32_t w=0,h=0,d=0;
    if (!payload.p || !chunk_holds(payload.p,payload.n,type,w,h,d)) return {};
    const uint64_t count=uint64_t(w)*h*d;
    MaterialTexturePixels result{w,h,d,std::vector<uint8_t>(size_t(count)*4,255)};
    for (size_t i=0;i<count;++i) {
        if (type==16) { // format 1 -> D3DFMT_A8R8G8B8, little-endian BGRA
            result.rgba[4*i]=payload.p[28+4*i+2]; result.rgba[4*i+1]=payload.p[28+4*i+1];
            result.rgba[4*i+2]=payload.p[28+4*i]; result.rgba[4*i+3]=payload.p[28+4*i+3];
        } else result.rgba[4*i+3]=payload.p[28+i]; // format 2 -> D3DFMT_A8
    }
    return result;
}
bool material_chunk_loads(uint64_t size, const MaterialChunkReader &read, uint8_t type) {
    if (!read || size<8 || (type!=16 && type!=17 && type!=18)) return false;
    uint64_t offset=0, length=0;
    if (!find_chunk_at(read,8,size,chunk_tag(type),0,offset,length) || length<28) return false;
    uint8_t head[28];
    uint32_t w=0,h=0,d=0;
    return read(offset,head,sizeof(head)) && chunk_holds(head,length,type,w,h,d);
}
} // namespace opennova::renderer
