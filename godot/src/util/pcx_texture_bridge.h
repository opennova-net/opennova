#pragma once

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace opennova {

// The terrain's index-data PCX slots (the foliage and char maps): the indices and
// the palette, uploaded as an RGB texture for display. A PCX texture the game draws
// loads through util/texture_path_resolver.h's loaders instead.
bool decode_pcx_with_palette(const uint8_t *data,
                             size_t size,
                             std::vector<uint8_t> &out_indices,
                             uint8_t out_palette[256][3],
                             int &out_w,
                             int &out_h);
godot::Ref<godot::Texture2D> build_indexed_texture(const std::vector<uint8_t> &indices,
                                                   const uint8_t palette[256][3],
                                                   int width,
                                                   int height);

} // namespace opennova
