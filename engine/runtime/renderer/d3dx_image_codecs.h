#pragma once

// The four codecs D3DX's content sniff tries on a "DDS" file's bytes that the port's
// image decoders lack: PPM, PFM, Radiance HDR, and a headerless DIB
// (`D3DXTex::CImage::Load @ 0x6DF1DC`, the order in dds_reader_codec_order). Each is a
// structural port of the D3DX codec the game links (d3dx_image_codecs.cpp holds the
// witnesses). Witness record: docs/render/render-material-re.md ("The DDS reader").
//
// What the game's texture holds: the DDS reader asks D3DX for D3DFMT_UNKNOWN
// (`GTexture_InitFromMemory @ 0x688315`), so the texture takes the codec's own format
// when the device has it (`D3DXCreateTextureFromFileInMemoryEx_Internal @ 0x6913BB`,
// `D3DXTex::FindClosestDeviceFormat @ 0x69053D`): X8R8G8B8 for a PPM, A32B32G32R32F
// for a PFM or HDR. The port's images are RGBA8, so a float channel becomes a byte by
// the rule D3DX's own 8-bit encoder applies (d3dx_float_to_unorm8).

#include <formats/pcx/pcx.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova::renderer {

// The most pixels any of these decodes to: Godot's own image limit (Image::MAX_PIXELS,
// 16384 x 16384), the TGA reader's kMaxTgaPixels. A header past it, or one naming
// more pixels than its data describes, fails before any buffer is made.
inline constexpr size_t kMaxD3dxImagePixels = size_t(1) << 28;

// The byte D3DX's A8R8G8B8 encoder writes for a float channel without dithering:
// the channel times 255 rounded to a float, plus 0.5 rounded to a float, truncated,
// then clamped to 0..255. NaN and anything whose sum reaches 2^31 come out 0 (the
// truncation's out-of-range result), so +infinity is 0, not 255.
uint8_t d3dx_float_to_unorm8(float value);

// The PPM codec: "P3" (ASCII) or "P6" (binary) only. Into `out` as RGBA8, the top row
// first, every pixel opaque (the codec's image is X8R8G8B8). False, with `error`,
// where the codec fails, and for a header the data cannot back.
bool decode_d3dx_ppm(const uint8_t *data, size_t size, opennova::RgbaImage &out, std::string &error);

// The PFM codec: "PF\n" (RGB) or "Pf\n" (grey), the rows bottom-up, big-endian floats
// unless the scale is negative. Into `out` as RGBA8 through d3dx_float_to_unorm8,
// the top row first, every pixel opaque.
bool decode_d3dx_pfm(const uint8_t *data, size_t size, opennova::RgbaImage &out, std::string &error);

// The Radiance HDR codec: "#?RADIANCE", a FORMAT line, optional EXPOSURE lines, the
// resolution line, then RGBE scanlines (flat, old run-length or new run-length). Two
// steps, so the embedder makes the one buffer and makes it only for data that
// describes its image: d3dx_hdr_size reads the header and walks every scanline as the
// decode does, writing nothing (false, with `error`, where the codec fails; an
// old-style run can describe millions of pixels in four bytes, so no byte count
// bounds the image before that walk); decode_d3dx_hdr_into then writes the image into
// `rgba` (width x height x 4 bytes) as RGBA8 through d3dx_float_to_unorm8, the top row
// first, opaque.
bool d3dx_hdr_size(const uint8_t *data, size_t size, int &width, int &height, std::string &error);
bool decode_d3dx_hdr_into(const uint8_t *data, size_t size, uint8_t *rgba, std::string &error);

// The DIB codec: the bytes as a BITMAPINFOHEADER (or BITMAPCOREHEADER) with no file
// header, which D3DX reads with its BMP codec's core. `bmp` becomes the same bytes
// behind a 14-byte BITMAPFILEHEADER whose bfOffBits is where the core reads the
// pixels from (it never reads a bfOffBits), so the port's BMP decoder can decode it.
// False, with `error`, where the core's header checks fail, and for a header the
// data cannot back. The core's pixel decode is not ported here: see the .cpp.
bool d3dx_dib_to_bmp(const uint8_t *data, size_t size, std::vector<uint8_t> &bmp, std::string &error);

// The BMP codec's view of a "BM" file: its file header checked as the codec checks it
// (14 bytes, "BM", bfSize within the bytes), then the bytes after it handled as a DIB
// (d3dx_dib_to_bmp), so `bmp` carries the pixel offset the core uses whatever the
// file's own bfOffBits says.
bool d3dx_bmp_rehead(const uint8_t *data, size_t size, std::vector<uint8_t> &bmp, std::string &error);

} // namespace opennova::renderer
