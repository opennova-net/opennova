#include "hud/font_page_glyphs.h"
#include "util/color_convert.h"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

#include <runtime/renderer/texture_load_rules.h>

#include <algorithm>
#include <cstring>

using namespace opennova::fnt;

namespace godot {

Ref<Texture2D> font_page_texture(const fnt_font_t &p_font, uint32_t p_page) {
	if (p_page >= p_font.num_pages || p_page >= FNT_MAX_PAGES) {
		return Ref<Texture2D>();
	}
	const uint8_t *data = fnt_get_page_data_const(&p_font, p_page);
	if (data == nullptr) {
		return Ref<Texture2D>();
	}
	PackedByteArray bytes;
	bytes.resize(FNT_TEXTURE_SIZE);
	std::memcpy(bytes.ptrw(), data, FNT_TEXTURE_SIZE);
	const Ref<Image> image = Image::create_from_data(FNT_TEXTURE_WIDTH, FNT_TEXTURE_HEIGHT, false,
			Image::FORMAT_RGBA8, bytes);
	if (image.is_null()) {
		return Ref<Texture2D>();
	}
	return ImageTexture::create_from_image(image);
}

bool font_page_runs_modulate2x() {
	return opennova::renderer::material_color_stage(opennova::hud::kFontPageMaterialWord) ==
			opennova::renderer::MaterialColorStage::Modulate2x;
}

const char *glyph_canvas_shader_code() {
	return R"(
shader_type canvas_item;
render_mode unshaded, blend_mix;

varying flat float modulate2x_on;

void vertex() {
	modulate2x_on = 0.0;
	if (UV.y <= -8.0) {
		UV.y += 16.0;
		modulate2x_on = 1.0;
	}
}

void fragment() {
	if (modulate2x_on > 0.5) {
		COLOR.rgb = min(COLOR.rgb * 2.0, vec3(1.0));
	}
}
)";
}

void append_glyph_quads(const std::vector<opennova::hud::GameFontQuad> &p_glyphs, size_t p_begin,
		size_t p_end, const Vector2 &p_uv_flag, GlyphRunArrays &r_out) {
	p_end = std::min(p_end, p_glyphs.size());
	if (p_begin >= p_end) {
		return;
	}
	const int64_t count = static_cast<int64_t>(p_end - p_begin);
	const int64_t vertex_base = r_out.points.size();
	const int64_t index_base = r_out.indices.size();
	r_out.points.resize(vertex_base + count * 4);
	r_out.uvs.resize(vertex_base + count * 4);
	r_out.colors.resize(vertex_base + count * 4);
	r_out.indices.resize(index_base + count * 6);
	Vector2 *point = r_out.points.ptrw() + vertex_base;
	Vector2 *uv = r_out.uvs.ptrw() + vertex_base;
	Color *color = r_out.colors.ptrw() + vertex_base;
	int32_t *index = r_out.indices.ptrw() + index_base;
	static constexpr int kCorners[] = { 0, 1, 2, 0, 2, 3 };
	for (int64_t i = 0; i < count; ++i) {
		const opennova::hud::GameFontQuad &glyph = p_glyphs[p_begin + static_cast<size_t>(i)];
		const int64_t base = i * 4;
		point[base] = Vector2(glyph.x_top_left, glyph.y_top);
		point[base + 1] = Vector2(glyph.x_top_right, glyph.y_top);
		point[base + 2] = Vector2(glyph.x_bottom_right, glyph.y_bottom);
		point[base + 3] = Vector2(glyph.x_bottom_left, glyph.y_bottom);
		uv[base] = Vector2(glyph.u0, glyph.v0) + p_uv_flag;
		uv[base + 1] = Vector2(glyph.u1, glyph.v0) + p_uv_flag;
		uv[base + 2] = Vector2(glyph.u1, glyph.v1) + p_uv_flag;
		uv[base + 3] = Vector2(glyph.u0, glyph.v1) + p_uv_flag;
		const Color modulation = opennova::color_from_argb(glyph.color);
		for (int corner = 0; corner < 4; ++corner) color[base + corner] = modulation;
		for (int corner = 0; corner < 6; ++corner) {
			index[i * 6 + corner] = static_cast<int32_t>(vertex_base + base + kCorners[corner]);
		}
	}
}

} // namespace godot
