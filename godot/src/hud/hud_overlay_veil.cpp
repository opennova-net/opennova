// The view veil: the three fullscreen damage-feedback quads and the sun veil, laid over the main
// HUD and under everything the scene frame draws after it (engine runtime/hud/hud_frame.h
// HudDrawList::veil_begin carries the witness; D-NET-1384). The engine owns every word, decay,
// cap and gate (runtime/world/player_view.h); this sizes the quads and picks their colours.
#include "hud/hud_overlay.h"
#include "render/d3d9_raster_device.h"

#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/variant/rect2.hpp>

#include <algorithm>
#include <initializer_list>

namespace godot {

namespace {

// The veil's quad samples the Celestial's pushed glare alpha; a gated frame's fragments discard.
constexpr const char *kSunVeilShader = "res://shaders/sun_veil_overlay.gdshader";
// The revive tint's multiply: the program a BLEND_MODE_MUL CanvasItemMaterial makes, as a Shader
// whose device program the load's HUD leg makes (GameHudPresenter, D-NET-1367).
constexpr const char *kReviveTintShader = "res://shaders/canvas_blend_mul.gdshader";

float byte_alpha(int p_word) {
	return std::clamp(static_cast<float>(p_word) / 255.0f, 0.0f, 1.0f);
}

} // namespace

void HudOverlay::update_damage_feedback(int p_white_alpha, int p_red_alpha, int p_revive,
		int p_revive_channel) {
	if (p_white_alpha == flash_white_ && p_red_alpha == flash_red_ && p_revive == flash_revive_ &&
			p_revive_channel == flash_revive_channel_)
		return;
	flash_white_ = p_white_alpha;
	flash_red_ = p_red_alpha;
	flash_revive_ = p_revive;
	flash_revive_channel_ = p_revive_channel;
	queue_redraw();
}

void HudOverlay::set_damage_vignette(const Ref<Texture2D> &p_texture) {
	damage_vignette_ = p_texture;
	queue_redraw();
}

void HudOverlay::set_sun_veil_enabled(bool p_enabled) {
	if (sun_veil_enabled_ == p_enabled) return;
	sun_veil_enabled_ = p_enabled;
	queue_redraw();
}

PackedColorArray HudOverlay::get_damage_feedback_colors() const {
	PackedColorArray out;
	out.resize(3);
	// 1. The white hit flash: an untextured white quad whose alpha is the word, through the
	// untextured iterated-colour material the veil uses.
	out.set(0, flash_white_ > 0 ? Color(1.0f, 1.0f, 1.0f, byte_alpha(flash_white_))
								: Color(0, 0, 0, 0));
	// 2. The red damage vignette: vignette.tga stretched over the screen, its texture alpha
	// times the vertex alpha, tinted by the vertex colour (material flags 593, AFUNC_BLEND |
	// ASRC_TEXTURExITERATED | COLOR_ITERATED): an ordinary modulate over an alpha-blended rect.
	// With no art loaded the quad has nothing to draw.
	out.set(1, flash_red_ > 0 && damage_vignette_.is_valid()
					? Color(1.0f, 0.0f, 0.0f, byte_alpha(flash_red_))
					: Color(0, 0, 0, 0));
	// 3. The medic revive tint: an opaque quad whose red and green fall to 255 - (word >> 1),
	// blue 255, the word flooring at 196 and holding there until the round clears. An opaque
	// source-over quad would wall the view off, so its blend is a multiply: the frame tinted
	// blue, deepening with the word (the one part of the three not witnessed byte for byte).
	if (flash_revive_ > 0) {
		const float channel = byte_alpha(flash_revive_channel_);
		out.set(2, Color(channel, channel, 1.0f, 1.0f));
	} else {
		out.set(2, Color(0, 0, 0, 0));
	}
	return out;
}

Ref<ShaderMaterial> HudOverlay::get_sun_veil_material() {
	ensure_view_veil_materials_();
	return sun_veil_material_;
}

Ref<ShaderMaterial> HudOverlay::get_revive_tint_material() {
	ensure_view_veil_materials_();
	return revive_tint_material_;
}

void HudOverlay::ensure_view_veil_materials_() {
	if (sun_veil_material_.is_valid()) return;
	ResourceLoader *loader = ResourceLoader::get_singleton();
	sun_veil_material_.instantiate();
	sun_veil_material_->set_shader(loader->load(kSunVeilShader));
	revive_tint_material_.instantiate();
	revive_tint_material_->set_shader(loader->load(kReviveTintShader));
}

void HudOverlay::ensure_view_veil_() {
	if (sun_veil_item_.is_valid()) return;
	ensure_view_veil_materials_();
	RenderingServer *rs = RenderingServer::get_singleton();
	const RID parent = get_canvas_item();
	// The quads in the scene frame's order: the white flash and the red vignette (one item, the
	// default blend), the revive tint (the multiply), then the veil, each above the main HUD's
	// items and under the flat entries the frame draws after them.
	damage_quads_item_ = rs->canvas_item_create();
	rs->canvas_item_set_parent(damage_quads_item_, parent);
	rs->canvas_item_set_draw_index(damage_quads_item_, kDrawDamageQuads);
	revive_tint_item_ = rs->canvas_item_create();
	rs->canvas_item_set_parent(revive_tint_item_, parent);
	rs->canvas_item_set_draw_index(revive_tint_item_, kDrawReviveTint);
	rs->canvas_item_set_material(revive_tint_item_, revive_tint_material_->get_rid());
	sun_veil_item_ = rs->canvas_item_create();
	rs->canvas_item_set_parent(sun_veil_item_, parent);
	rs->canvas_item_set_draw_index(sun_veil_item_, kDrawSunVeil);
	rs->canvas_item_set_material(sun_veil_item_, sun_veil_material_->get_rid());
}

void HudOverlay::ensure_over_veil_item_() {
	if (over_veil_item_.is_valid()) return;
	RenderingServer *rs = RenderingServer::get_singleton();
	over_veil_item_ = rs->canvas_item_create();
	rs->canvas_item_set_parent(over_veil_item_, get_canvas_item());
	rs->canvas_item_set_transform(over_veil_item_, d3d9_screen_to_canvas());
	rs->canvas_item_set_draw_index(over_veil_item_, kDrawOverVeil);
	ensure_flat_material_();
	rs->canvas_item_set_material(over_veil_item_, flat_material_->get_rid());
}

void HudOverlay::clear_view_veil_() {
	RenderingServer *rs = RenderingServer::get_singleton();
	for (const RID &item :
			{ damage_quads_item_, revive_tint_item_, sun_veil_item_, over_veil_item_ })
		if (item.is_valid()) rs->canvas_item_clear(item);
}

void HudOverlay::free_view_veil_() {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (rs != nullptr) {
		for (const RID &item :
				{ damage_quads_item_, revive_tint_item_, sun_veil_item_, over_veil_item_ })
			if (item.is_valid()) rs->free_rid(item);
	}
	damage_quads_item_ = RID();
	revive_tint_item_ = RID();
	sun_veil_item_ = RID();
	over_veil_item_ = RID();
}

void HudOverlay::render_view_veil_() {
	ensure_view_veil_();
	RenderingServer *rs = RenderingServer::get_singleton();
	const Rect2 screen(Vector2(), draw_surface_());
	const PackedColorArray colors = get_damage_feedback_colors();
	if (colors[0].a > 0.0f) rs->canvas_item_add_rect(damage_quads_item_, screen, colors[0]);
	if (colors[1].a > 0.0f)
		rs->canvas_item_add_texture_rect(damage_quads_item_, screen, damage_vignette_->get_rid(),
				false, colors[1]);
	if (colors[2].a > 0.0f) rs->canvas_item_add_rect(revive_tint_item_, screen, colors[2]);
	// The veil's quad stands every frame; its shader takes the pushed alpha (0 discards).
	if (sun_veil_enabled_) rs->canvas_item_add_rect(sun_veil_item_, screen, Color(1, 1, 1, 1));
}

HudOverlay::FlatRange HudOverlay::flat_span_(const opennova::hud::HudDrawList &p_list,
		const opennova::hud::HudDrawList::TopBegin &p_from,
		const opennova::hud::HudDrawList::TopBegin &p_to) {
	const auto span = [](size_t from, size_t to, size_t size, size_t &r_begin, size_t &r_end) {
		r_begin = std::min(from, size);
		r_end = std::max(r_begin, std::min(to, size));
	};
	FlatRange out;
	span(p_from.quads, p_to.quads, p_list.quads.size(), out.quads_begin, out.quads_end);
	span(p_from.tris, p_to.tris, p_list.tris.size(), out.tris_begin, out.tris_end);
	span(p_from.lines, p_to.lines, p_list.lines.size(), out.lines_begin, out.lines_end);
	span(p_from.glyphs, p_to.glyphs, p_list.glyphs.size(), out.glyphs_begin, out.glyphs_end);
	span(p_from.underlines, p_to.underlines, p_list.underlines.size(), out.underlines_begin,
			out.underlines_end);
	return out;
}

bool HudOverlay::flat_empty_(const FlatRange &p_range) {
	return p_range.quads_begin == p_range.quads_end && p_range.tris_begin == p_range.tris_end &&
			p_range.lines_begin == p_range.lines_end &&
			p_range.glyphs_begin == p_range.glyphs_end &&
			p_range.underlines_begin == p_range.underlines_end;
}

} // namespace godot
