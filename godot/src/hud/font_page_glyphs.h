#pragma once

// The device leg every compiled GameFont glyph run shares (the flat HUD, the map
// passes, the menus): a font's pages uploaded as textures, and a page run of glyph
// quads written as one triangle array. A font page draws through its material,
// hud::kFontPageMaterialWord: colour family 0x600, MODULATE2X(TEXTURE, DIFFUSE)
// saturated per channel, alpha MODULATE(TEXTURE, DIFFUSE)
// (renderer::material_color_stage). The run's UVs carry the offset its target
// item's shader reads as that stage (each item's own flag); the glyph colours stay
// the raw diffuses the compilers lay out, the drawers' halved colours.

#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/rid.hpp>
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

// The canvas_item shader of an item whose glyph runs flag nothing else on it
// (the menus' items, the loading screen): a command whose UV.y carries
// kGlyphCanvasUvFlag is a glyph run on a font page, whose material is colour
// family 0x600 (hud::kFontPageMaterialWord): vertex() strips the flag and
// fragment() runs MODULATE2X(TEXTURE, DIFFUSE) over COLOR (the texel times the
// vertex colour), saturated per channel, the alpha MODULATE as COLOR has it,
// under the SRCALPHA/INVSRCALPHA blend. The flag is negative because a menu's
// tiled rects carry UVs far above 1 and none below 0; every other command keeps
// the default texel x vertex colour. A command whose UV.x carries
// kFramePieceCanvasUvFlag is a frame border piece in retail's two-stage border
// material, drawn through a CanvasTexture whose normal slot holds the brush:
// stage 0 the stencil (TEXTURE at UV) x diffuse, MODULATE2X; stage 1 that x the
// brush SCREEN-anchored at ((x + 0.5) / brush height, (y + 0.5) / brush width) of
// the command's own (window) coordinates, MODULATE2X; each saturated, the alpha
// the stencil's x the diffuse's x the brush's (the MenuQuad contract, engine
// menu/menu_frame.h, carries the witness).
const char *glyph_canvas_shader_code();
inline constexpr float kGlyphCanvasUvFlag = -16.0f;
inline constexpr float kFramePieceCanvasUvFlag = -16.0f;

// The one Shader of that code every glyph-shader item draws through (each menu's frame, the
// loading screen's splash, the editor's font viewport): one program, as retail draws every menu
// through its one fixed-function device state, so the program's device version and its pipelines
// are made once per process and not again at each screen's first draw.
Ref<Shader> glyph_canvas_shader();
// Module teardown: drops the shared shader before the engine's servers go.
void release_glyph_canvas_shader();

// The load's one forced frame's warm of that program (GameWorld's menu_pipelines leg): one canvas
// item under `p_canvas`, through the shared shader, carrying one of each command a menu frame's
// draw list submits (a rect, a glyph run's triangle array of points, colours and UVs, a one-pixel
// line), so the program's version and those commands' pipelines are made in that frame, behind
// the loading screen, and not in the first frame a menu shows. The caller frees it past the
// drained frame (free_glyph_canvas_warm).
struct GlyphCanvasWarm {
	RID item;
	Ref<ShaderMaterial> material;
};
GlyphCanvasWarm build_glyph_canvas_warm(const RID &p_canvas);
void free_glyph_canvas_warm(GlyphCanvasWarm &r_warm);

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
