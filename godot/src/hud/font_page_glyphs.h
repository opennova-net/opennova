#pragma once

// The device leg every compiled GameFont glyph run shares (the flat HUD, the map
// passes, the menus): a font's pages uploaded as textures, and a page run of glyph
// quads written as one triangle array. A font page draws through its material,
// hud::kFontPageMaterialWord: colour family 0x600, MODULATE2X(TEXTURE, DIFFUSE)
// saturated per channel, alpha MODULATE(TEXTURE, DIFFUSE)
// (renderer::material_color_stage). The run's UVs carry the offset its target
// item's shader reads as that stage (each item's own flag); the glyph colours stay
// the raw diffuses the compilers lay out, the drawers' halved colours.

#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/vector2.hpp>

#include <formats/fnt/fnt.h>
#include <runtime/hud/game_font.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace godot {

// One page of a parsed font as an RGBA8 texture; null when the page holds no data.
Ref<Texture2D> font_page_texture(const opennova::fnt::fnt_font_t &p_font, uint32_t p_page);

// Whether a font page's material runs the MODULATE2X stage on the device
// (renderer::material_color_stage of hud::kFontPageMaterialWord).
bool font_page_runs_modulate2x();

// One page run's triangle array.
struct GlyphRunArrays {
	PackedVector2Array points;
	PackedVector2Array uvs;
	PackedColorArray colors;
	PackedInt32Array indices;
};

// p_glyphs[p_begin, p_end) appended to r_out as quads of two triangles each: the
// four corners (the italic shear rides the top edge), the UVs offset by p_uv_flag,
// every vertex at the glyph's colour.
void append_glyph_quads(const std::vector<opennova::hud::GameFontQuad> &p_glyphs, size_t p_begin,
		size_t p_end, const Vector2 &p_uv_flag, GlyphRunArrays &r_out);

} // namespace godot
