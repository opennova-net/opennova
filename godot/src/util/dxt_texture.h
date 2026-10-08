#pragma once

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

#include <runtime/renderer/texture_dxt.h>

#include <vector>

namespace godot {

// A DXT texture's retail levels as one Godot image, the blocks uploaded as they
// are so the GPU decodes them as the retail device did. Godot needs the pyramid
// down to 1x1 where retail stops earlier; the tail continues D3DXFilterTexture's
// box chain from the last retail level (renderer::box_filter_half over its
// decoded blocks), and a draw that stops at the retail chain never selects it.
// Null for an empty or inconsistent chain (every level half the one before, in
// one format).
Ref<Image> image_from_dxt_levels(const std::vector<opennova::renderer::DxtSurface> &p_levels);
Ref<Texture2D> texture_from_dxt_levels(const std::vector<opennova::renderer::DxtSurface> &p_levels);

// The blocks of every level end to end, level 0 first: what a RenderingDevice
// texture of the format takes as its one layer's data.
PackedByteArray packed_dxt_levels(const std::vector<opennova::renderer::DxtSurface> &p_levels);

} // namespace godot
