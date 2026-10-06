// HudOverlay's flat-HUD submission and the two stage shaders it and the map
// passes draw through: the compiled draw list's quads, triangles, lines and
// glyphs into canvas commands, each textured command combined through its
// texture's material word (renderer::material_color_stage; the words are
// runtime/hud/hud_texture_materials.h, renderer::hud_loader_material_word and,
// for the font pages, hud::kFontPageMaterialWord).

#include "hud/hud_overlay.h"
#include "hud/font_page_glyphs.h"
#include "util/color_convert.h"
#include "util/string_convert.h"

#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/rect2.hpp>

#include <runtime/renderer/texture_load_rules.h>

#include <algorithm>

using namespace godot;

namespace {

using opennova::hud::HudDrawList;
using opennova::renderer::MaterialColorStage;

// The map pass's top-layer shader. A sprite whose texture's material word is
// colour family 0x600 (every textured map sprite: the TSDicon strip, the
// compass ring, the radar marks, the WPIndctr strip; and every map label's
// font page, D-HUD-51) arrives with a +8 flag on
// UV.x (map UVs stay inside [0, 1]); its vertex() strips the flag and
// fragment() runs the MODULATE2X(TEXTURE, DIFFUSE) colour stage, saturated per
// channel, over COLOR (the texel times the vertex colour), the alpha stage
// MODULATE(TEXTURE, DIFFUSE) left as COLOR has it, under the
// SRCALPHA/INVSRCALPHA blend (renderer::hud_color_material_argb). Every other
// command draws texel x vertex colour, so the flagged sprites keep their place
// in the pass's order.
constexpr const char *kMapModulate2xShader = R"(
shader_type canvas_item;
render_mode unshaded, blend_mix;

varying flat float modulate2x_on;

void vertex() {
	modulate2x_on = 0.0;
	if (UV.x >= 4.0) {
		UV.x -= 8.0;
		modulate2x_on = 1.0;
	}
}

void fragment() {
	if (modulate2x_on > 0.5) {
		COLOR.rgb = min(COLOR.rgb * 2.0, vec3(1.0));
	}
}
)";

// The flat HUD items' shader. A command whose UV.y carries a +16 flag draws a
// texture whose material word is colour family 0x600 (a font page's glyph run
// among them, hud::kFontPageMaterialWord; D-HUD-51): its vertex() strips the
// flag and fragment() runs the material's stage-0 MODULATE2X(TEXTURE, DIFFUSE)
// over COLOR, saturated per channel, the alpha MODULATE as COLOR has it
// (renderer::hud_color_material_argb; D-HUD-49). A quad with a second texture
// stage arrives as a triangle pair whose UV.x carries a +8 flag (flat HUD UVs
// stay inside [0, ~1.01]); its vertex() strips the flag and derives the
// stage-1 UV from the surface position -- UV1 = (screen_px + 0.5) / stage dims
// in retail's D3D9 raster, whose pixel centres sit on the integers; here pixel
// centres sit at +0.5, so px / stage dims samples the same texel -- and
// fragment() then applies MODULATE2X(CURRENT, TEXTURE1) to the colour and
// MODULATE(CURRENT, TEXTURE1) to the alpha, the stage-1 texture
// wrap-addressed. Every other command keeps the default COLOR (vertex colour x
// TEXTURE). The stage-1 witness rides the engine's HudQuad::texture2
// (runtime/hud/hud_frame.h).
constexpr const char *kHudFlatShader = R"(
shader_type canvas_item;
render_mode unshaded, blend_mix;

uniform sampler2D stage1_texture : repeat_enable, filter_nearest;
uniform vec2 stage1_inv_size = vec2(0.0);

varying flat float stage1_on;
varying flat float modulate2x_on;
varying vec2 stage1_uv;

void vertex() {
	stage1_on = 0.0;
	modulate2x_on = 0.0;
	stage1_uv = vec2(0.0);
	if (UV.x >= 4.0) {
		UV.x -= 8.0;
		stage1_on = 1.0;
		stage1_uv = VERTEX * stage1_inv_size;
	}
	if (UV.y >= 8.0) {
		UV.y -= 16.0;
		modulate2x_on = 1.0;
	}
}

void fragment() {
	if (modulate2x_on > 0.5) {
		COLOR.rgb = min(COLOR.rgb * 2.0, vec3(1.0));
	}
	if (stage1_on > 0.5) {
		vec4 camo = texture(stage1_texture, stage1_uv);
		COLOR.rgb = min(COLOR.rgb * camo.rgb * 2.0, vec3(1.0));
		COLOR.a *= camo.a;
	}
}
)";

// The +8 UV.x flag a second-stage quad's vertices carry into kHudFlatShader.
constexpr float kStage1UvFlag = 8.0f;
// The +16 UV.y flag a family-0x600 texture's vertices carry into kHudFlatShader.
constexpr float kModulate2xUvFlag = 16.0f;

} // namespace

String HudOverlay::flat_shader_code() {
	return String(kHudFlatShader);
}

String HudOverlay::map_shader_code() {
	return String(kMapModulate2xShader);
}

int64_t HudOverlay::get_texture_material_word(const String &p_name) const {
	const auto found = named_material_words_.find(opennova::to_std(p_name.get_file().to_lower()));
	return found == named_material_words_.end() ? -1 : static_cast<int64_t>(found->second);
}

MaterialColorStage HudOverlay::texture_stage_(const Ref<Texture2D> &p_texture) const {
	if (p_texture.is_null()) {
		return MaterialColorStage::Other;
	}
	const auto found = texture_material_words_.find(p_texture->get_instance_id());
	return opennova::renderer::material_color_stage(
			found == texture_material_words_.end() ? 0u : found->second);
}

Color HudOverlay::texture_draw_color_(const Ref<Texture2D> &p_texture, uint32_t p_argb) const {
	if (texture_stage_(p_texture) == MaterialColorStage::AddDiffuse) {
		return opennova::color_from_argb(opennova::renderer::hud_alpha_material_argb(p_argb));
	}
	return opennova::color_from_argb(p_argb);
}

PackedColorArray HudOverlay::get_textured_quad_colors(bool p_drawn) {
	PackedColorArray out;
	if (!configured_) {
		return out;
	}
	const Vector2 surface = draw_surface_();
	ensure_label_fonts_(surface.x);
	const HudDrawList &list = compiler_.compile(state_, surface.x, surface.y);
	for (const opennova::hud::HudQuad &quad : list.quads) {
		if (quad.texture < 0 || quad.texture >= kTextureSlots) {
			continue;
		}
		uint32_t argb = quad.color;
		if (p_drawn) {
			switch (texture_stage_(textures_[static_cast<size_t>(quad.texture)])) {
			case MaterialColorStage::AddDiffuse:
				argb = opennova::renderer::hud_alpha_material_argb(argb);
				break;
			case MaterialColorStage::Modulate2x:
				argb = opennova::renderer::hud_color_material_argb(0xFFFFFFFFu, argb);
				break;
			case MaterialColorStage::Other:
				break;
			}
		}
		out.push_back(opennova::color_from_argb(argb));
	}
	return out;
}

Array HudOverlay::get_flat_submissions() {
	Array out;
	if (!configured_ || !is_inside_tree()) {
		return out;
	}
	const Vector2 surface = draw_surface_();
	ensure_label_fonts_(surface.x);
	const HudDrawList &list = compiler_.compile(state_, surface.x, surface.y);
	FlatRange all;
	all.quads_end = list.quads.size();
	all.tris_end = list.tris.size();
	all.lines_end = list.lines.size();
	all.glyphs_end = list.glyphs.size();
	all.underlines_end = list.underlines.size();
	flat_record_ = &out;
	render_flat_runs_(get_canvas_item(), list, all);
	flat_record_ = nullptr;
	queue_redraw();
	return out;
}

Array HudOverlay::get_map_submissions() {
	Array out;
	if (!configured_ || !is_inside_tree()) {
		return out;
	}
	const Vector2 surface = draw_surface_();
	ensure_label_fonts_(surface.x);
	const HudDrawList &list = compiler_.compile(state_, surface.x, surface.y);
	corner_map_.set_record(&out);
	render_map_(list.map, list.map_glyphs, false);
	corner_map_.set_record(nullptr);
	queue_redraw();
	return out;
}

void HudOverlay::ensure_flat_material_() {
	if (flat_material_.is_valid()) return;
	flat_shader_.instantiate();
	flat_shader_->set_code(kHudFlatShader);
	flat_material_.instantiate();
	flat_material_->set_shader(flat_shader_);
}

void HudOverlay::ensure_map_materials_() {
	if (additive_material_.is_null()) {
		additive_material_.instantiate();
		additive_material_->set_blend_mode(CanvasItemMaterial::BLEND_MODE_ADD);
	}
	ensure_minimap_water_material_();
	if (map_modulate2x_material_.is_null()) {
		map_modulate2x_shader_.instantiate();
		map_modulate2x_shader_->set_code(kMapModulate2xShader);
		map_modulate2x_material_.instantiate();
		map_modulate2x_material_->set_shader(map_modulate2x_shader_);
	}
}

void HudOverlay::render_flat_runs_(const RID &p_item, const HudDrawList &p_list,
		const FlatRange &p_range) {
	FlatRange run = p_range;
	for (const HudDrawList::TopBegin &mark : p_list.order_breaks) {
		run.quads_end = std::clamp(mark.quads, run.quads_begin, p_range.quads_end);
		run.tris_end = std::clamp(mark.tris, run.tris_begin, p_range.tris_end);
		run.lines_end = std::clamp(mark.lines, run.lines_begin, p_range.lines_end);
		run.glyphs_end = std::clamp(mark.glyphs, run.glyphs_begin, p_range.glyphs_end);
		run.underlines_end =
				std::clamp(mark.underlines, run.underlines_begin, p_range.underlines_end);
		render_flat_(p_item, p_list, run);
		run.quads_begin = run.quads_end;
		run.tris_begin = run.tris_end;
		run.lines_begin = run.lines_end;
		run.glyphs_begin = run.glyphs_end;
		run.underlines_begin = run.underlines_end;
	}
	run.quads_end = p_range.quads_end;
	run.tris_end = p_range.tris_end;
	run.lines_end = p_range.lines_end;
	run.glyphs_end = p_range.glyphs_end;
	run.underlines_end = p_range.underlines_end;
	render_flat_(p_item, p_list, run);
}

void HudOverlay::render_flat_(const RID &p_item, const HudDrawList &p_list, const FlatRange &p_range) {
	RenderingServer *rs = RenderingServer::get_singleton();
	// The test seam's copy of one textured command as submitted
	// (get_flat_submissions).
	const auto record = [this](int32_t p_slot, const char *p_kind, const PackedVector2Array &p_uvs,
								const PackedColorArray &p_colors) {
		if (flat_record_ == nullptr) return;
		const Ref<Texture2D> &tex = textures_[static_cast<size_t>(p_slot)];
		Dictionary row;
		row["texture"] = p_slot;
		row["size"] = tex.is_valid() ? Vector2i(tex->get_width(), tex->get_height()) : Vector2i();
		row["kind"] = p_kind;
		row["uvs"] = p_uvs;
		row["colors"] = p_colors;
		flat_record_->push_back(row);
	};
	const auto record_rect = [&](int32_t p_slot, const Color &p_color) {
		if (flat_record_ == nullptr) return;
		PackedColorArray colors;
		colors.push_back(p_color);
		record(p_slot, "rect", PackedVector2Array(), colors);
	};
	// One textured quad as a triangle pair whose UVs carry kHudFlatShader's
	// stage flags, in its place in the run.
	const auto submit_flagged_quad = [&](const opennova::hud::HudQuad &p_quad,
											 const Ref<Texture2D> &p_tex, const Color &p_color,
											 float p_u_flag, float p_v_flag) {
		const Vector2 corners[4] = { Vector2(p_quad.x0, p_quad.y0), Vector2(p_quad.x1, p_quad.y0),
			Vector2(p_quad.x1, p_quad.y1), Vector2(p_quad.x0, p_quad.y1) };
		const Vector2 uvs[4] = { Vector2(p_quad.u0 + p_u_flag, p_quad.v0 + p_v_flag),
			Vector2(p_quad.u1 + p_u_flag, p_quad.v0 + p_v_flag),
			Vector2(p_quad.u1 + p_u_flag, p_quad.v1 + p_v_flag),
			Vector2(p_quad.u0 + p_u_flag, p_quad.v1 + p_v_flag) };
		PackedVector2Array points, uv;
		PackedColorArray colors;
		PackedInt32Array indices;
		for (int corner = 0; corner < 4; ++corner) {
			points.push_back(corners[corner]);
			uv.push_back(uvs[corner]);
			colors.push_back(p_color);
		}
		for (int index : { 0, 1, 2, 0, 2, 3 }) indices.push_back(index);
		rs->canvas_item_add_triangle_array(p_item, indices, points, colors, uv,
				PackedInt32Array(), PackedFloat32Array(), p_tex->get_rid());
		record(p_quad.texture, "triangles", uv, colors);
	};
	// Kind-grouped submission preserves the compiler's per-kind insertion
	// order and keeps every glyph above the quads (retail draws its text
	// elements over the bars/frames the same walk emitted).
	for (size_t i = p_range.quads_begin; i < p_range.quads_end; ++i) {
		const opennova::hud::HudQuad &quad = p_list.quads[i];
		const Rect2 rect(quad.x0, quad.y0, quad.x1 - quad.x0, quad.y1 - quad.y0);
		if (!quad.filled) {
			PackedVector2Array outline;
			outline.push_back(Vector2(quad.x0, quad.y0));
			outline.push_back(Vector2(quad.x1, quad.y0));
			outline.push_back(Vector2(quad.x1, quad.y1));
			outline.push_back(Vector2(quad.x0, quad.y1));
			outline.push_back(Vector2(quad.x0, quad.y0));
			PackedColorArray outline_color;
			outline_color.push_back(opennova::color_from_argb(quad.color));
			rs->canvas_item_add_polyline(p_item, outline, outline_color, -1.0f);
			continue;
		}
		Ref<Texture2D> tex;
		if (quad.texture >= 0 && quad.texture < kTextureSlots) {
			tex = textures_[static_cast<size_t>(quad.texture)];
		}
		// A family-0xA00 texture draws with its material's colour.
		const Color color = texture_draw_color_(tex, quad.color);
		// A family-0x600 texture's MODULATE2X stage rides the UV.y flag.
		const float v_flag = tex.is_valid() && texture_stage_(tex) == MaterialColorStage::Modulate2x
				? kModulate2xUvFlag
				: 0.0f;
		// A second texture stage: the flagged triangle pair kHudFlatShader
		// combines, the stage-1 texture and its divisors bound on the item's
		// material (every such quad names the same stage, the boxtile camo).
		if (quad.texture2 >= 0 && quad.texture2 < kTextureSlots && tex.is_valid() &&
				quad.stage2_w > 0.0f && quad.stage2_h > 0.0f &&
				textures_[static_cast<size_t>(quad.texture2)].is_valid()) {
			ensure_flat_material_();
			flat_material_->set_shader_parameter("stage1_texture",
					textures_[static_cast<size_t>(quad.texture2)]);
			flat_material_->set_shader_parameter("stage1_inv_size",
					Vector2(1.0f / quad.stage2_w, 1.0f / quad.stage2_h));
			submit_flagged_quad(quad, tex, color, kStage1UvFlag, v_flag);
			continue;
		}
		if (v_flag != 0.0f) {
			submit_flagged_quad(quad, tex, color, 0.0f, v_flag);
			continue;
		}
		if (quad.additive) {
			// Per-command blend modes do not exist on a CanvasItem: additive
			// rows ride the child item carrying the add material.
			ensure_additive_item_();
			if (tex.is_valid()) {
				rs->canvas_item_add_texture_rect(additive_item_, rect, tex->get_rid(),
						false, color);
				record_rect(quad.texture, color);
			} else {
				rs->canvas_item_add_rect(additive_item_, rect, color);
			}
			continue;
		}
		if (tex.is_valid()) {
			if (quad.u0 != 0.0f || quad.v0 != 0.0f || quad.u1 != 1.0f || quad.v1 != 1.0f) {
				const Vector2 tex_size = tex->get_size();
				rs->canvas_item_add_texture_rect_region(p_item, rect, tex->get_rid(),
						Rect2(quad.u0 * tex_size.x, quad.v0 * tex_size.y,
								(quad.u1 - quad.u0) * tex_size.x,
								(quad.v1 - quad.v0) * tex_size.y),
						color);
			} else {
				rs->canvas_item_add_texture_rect(p_item, rect, tex->get_rid(), false, color);
			}
			record_rect(quad.texture, color);
		} else {
			rs->canvas_item_add_rect(p_item, rect, color);
		}
	}
	// Consecutive texture runs preserve primitive order and per-vertex color.
	// Device submission follows the font batch witness in docs/fonts/fnt-re.md;
	// the compiler still owns all crosshair and glyph geometry.
	for (size_t first = p_range.tris_begin; first < p_range.tris_end;) {
		const int texture = p_list.tris[first].texture;
		size_t end = first + 1;
		while (end < p_range.tris_end && p_list.tris[end].texture == texture) ++end;
		Ref<Texture2D> tex;
		if (texture >= 0 && texture < kTextureSlots) tex = textures_[static_cast<size_t>(texture)];
		const MaterialColorStage stage = texture_stage_(tex);
		const bool alpha_mode = stage == MaterialColorStage::AddDiffuse;
		// A family-0x600 texture's MODULATE2X stage rides kHudFlatShader's UV.y flag.
		const float v_flag = stage == MaterialColorStage::Modulate2x ? kModulate2xUvFlag : 0.0f;
		const int count = static_cast<int>((end - first) * 3);
		PackedVector2Array points, uvs;
		PackedColorArray colors;
		PackedInt32Array indices;
		points.resize(count);
		uvs.resize(count);
		colors.resize(count);
		indices.resize(count);
		Vector2 *point = points.ptrw(), *uv = uvs.ptrw();
		Color *color = colors.ptrw();
		int32_t *index = indices.ptrw();
		int vertex_index = 0;
		for (size_t i = first; i < end; ++i) {
			const auto &tri = p_list.tris[i];
			const Color modulation = opennova::color_from_argb(tri.color);
			for (const auto *vertex : { &tri.a, &tri.b, &tri.c }) {
				point[vertex_index] = Vector2(vertex->x, vertex->y);
				uv[vertex_index] = Vector2(vertex->u, vertex->v + v_flag);
				const Color diffuse = modulation * opennova::color_from_argb(vertex->color);
				color[vertex_index] = alpha_mode
						? opennova::color_from_argb(opennova::renderer::hud_alpha_material_argb(
								  static_cast<uint32_t>(diffuse.to_argb32())))
						: diffuse;
				index[vertex_index] = vertex_index;
				++vertex_index;
			}
		}
		rs->canvas_item_add_triangle_array(p_item, indices, points, colors, uvs,
				PackedInt32Array(), PackedFloat32Array(), tex.is_valid() ? tex->get_rid() : RID());
		if (tex.is_valid()) record(texture, "triangles", uvs, colors);
		first = end;
	}
	for (size_t i = p_range.lines_begin; i < p_range.lines_end; ++i) {
		const opennova::hud::HudLine &line = p_list.lines[i];
		rs->canvas_item_add_line(p_item, Vector2(line.x0, line.y0), Vector2(line.x1, line.y1),
				opennova::color_from_argb(line.color), line.width);
	}
	// Keep the existing kind order, page order, italic corners, half-pixel
	// offsets and underline layer. Never sort text by texture across runs. Each
	// page run draws through the page's material: its MODULATE2X stage rides the
	// UV.y flag, doubling the drawers' halved colours (hud::kFontPageMaterialWord).
	for (size_t first = p_range.glyphs_begin; first < p_range.glyphs_end;) {
		const uint32_t page = p_list.glyphs[first].page;
		size_t end = first + 1;
		while (end < p_range.glyphs_end && p_list.glyphs[end].page == page) ++end;
		if (page >= page_textures_.size() || page_textures_[page].is_null()) {
			first = end;
			continue;
		}
		const Ref<Texture2D> &page_texture = page_textures_[page];
		const float v_flag =
				texture_stage_(page_texture) == MaterialColorStage::Modulate2x ? kModulate2xUvFlag : 0.0f;
		GlyphRunArrays run;
		append_glyph_quads(p_list.glyphs, first, end, Vector2(0.0f, v_flag), run);
		rs->canvas_item_add_triangle_array(p_item, run.indices, run.points, run.colors, run.uvs,
				PackedInt32Array(), PackedFloat32Array(), page_texture->get_rid());
		if (flat_record_ != nullptr) {
			Dictionary row;
			row["texture"] = -1;
			row["page"] = static_cast<int64_t>(page);
			row["size"] = Vector2i(page_texture->get_width(), page_texture->get_height());
			row["kind"] = "glyphs";
			row["uvs"] = run.uvs;
			row["colors"] = run.colors;
			flat_record_->push_back(row);
		}
		first = end;
	}
	for (size_t i = p_range.underlines_begin; i < p_range.underlines_end; ++i) {
		const opennova::hud::GameFontUnderline &underline = p_list.underlines[i];
		rs->canvas_item_add_line(p_item, Vector2(underline.x0, underline.y),
				Vector2(underline.x1, underline.y), opennova::color_from_argb(underline.color),
				1.0f);
	}
}
