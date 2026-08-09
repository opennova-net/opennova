#pragma once

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace opennova {

godot::Ref<godot::Image> decode_pcx_image(const uint8_t *data, size_t size);
// PCX texture with the retail sky/effect-loader alpha: RGB from the palette,
// per-pixel A = the palette entry's (85*(r+g+b))>>8 luminance
// [orig: load_texture_from_archive @ 0x58b980, alpha loop @ 0x58bc35..0x58bcee].
godot::Ref<godot::Texture2D> build_pcx_luminance_alpha_texture(const godot::PackedByteArray &bytes);
bool decode_pcx_with_palette(const uint8_t *data,
                             size_t size,
                             std::vector<uint8_t> &out_indices,
                             uint8_t out_palette[256][3],
                             int &out_w,
                             int &out_h);
godot::PackedByteArray encode_pcx_indices(const uint8_t *indices,
                                          int width,
                                          int height,
                                          const uint8_t palette[256][3]);
godot::Ref<godot::Texture2D> build_indexed_texture(const std::vector<uint8_t> &indices,
                                                   const uint8_t palette[256][3],
                                                   int width,
                                                   int height);
std::vector<uint8_t> load_pcx_indices(const godot::String &path, int &out_w, int &out_h);

} // namespace opennova
