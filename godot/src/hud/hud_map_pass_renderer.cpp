#include "hud/hud_map_pass_renderer.h"
#include "util/color_convert.h"

#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/vector2.hpp>

#include <runtime/hud/hud_frame.h> // kHudTexMapIcons

#include <algorithm>
#include <cmath>

using namespace godot;

HudMapPassRenderer::~HudMapPassRenderer() {
	release();
}

bool HudMapPassRenderer::is_ready() const {
	return base_item_.is_valid() && add_item_.is_valid() && water_item_.is_valid() &&
			top_item_.is_valid();
}

void HudMapPassRenderer::ensure(const RID &parent, int first_draw_index, bool behind_parent,
		const RID &additive_material, const RID &water_material,
		const RID &top_material) {
	if (is_ready()) return;
	RenderingServer *rs = RenderingServer::get_singleton();
	if (rs == nullptr) return;
	// The retail map decal pipeline quadruples texture x diffuse (the
	// captures measure exactly 0x60 x 4 = 1.5058 x texture). A 1x canvas
	// cannot express >1 modulate, so the terrain draws twice — base + an
	// additive child-item pass — which saturates identically. Everything
	// the map draws ABOVE its terrain rides the top child so the sandwich
	// keeps retail's order (witness at hud_minimap.cpp kTerrainTint;
	// hud-re.md carries the pass addresses). Per-command blend modes do not
	// exist on a CanvasItem.
	if (!base_item_.is_valid()) {
		base_item_ = rs->canvas_item_create();
		rs->canvas_item_set_parent(base_item_, parent);
		rs->canvas_item_set_draw_behind_parent(base_item_, behind_parent);
		rs->canvas_item_set_draw_index(base_item_, first_draw_index);
	}
	if (!add_item_.is_valid()) {
		add_item_ = rs->canvas_item_create();
		rs->canvas_item_set_parent(add_item_, parent);
		rs->canvas_item_set_material(add_item_, additive_material);
		rs->canvas_item_set_draw_behind_parent(add_item_, behind_parent);
		rs->canvas_item_set_draw_index(add_item_, first_draw_index + 1);
	}
	if (!water_item_.is_valid()) {
		water_item_ = rs->canvas_item_create();
		rs->canvas_item_set_parent(water_item_, parent);
		rs->canvas_item_set_material(water_item_, water_material);
		rs->canvas_item_set_draw_behind_parent(water_item_, behind_parent);
		rs->canvas_item_set_draw_index(water_item_, first_draw_index + 2);
		rs->canvas_item_set_default_texture_filter(water_item_,
				RenderingServer::CANVAS_ITEM_TEXTURE_FILTER_LINEAR);
		rs->canvas_item_set_default_texture_repeat(water_item_,
				RenderingServer::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
		water_sampling_configured_ = true;
	}
	if (!top_item_.is_valid()) {
		top_item_ = rs->canvas_item_create();
		rs->canvas_item_set_parent(top_item_, parent);
		rs->canvas_item_set_draw_behind_parent(top_item_, behind_parent);
		rs->canvas_item_set_draw_index(top_item_, first_draw_index + 3);
		rs->canvas_item_set_default_texture_filter(top_item_,
				RenderingServer::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS);
		rs->canvas_item_set_default_texture_repeat(top_item_,
				RenderingServer::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
		// A MODULATE2X sprite selects its colour stage through the material's
		// UV flag, in its own place in the order (see render).
		rs->canvas_item_set_material(top_item_, top_material);
		top_sampling_configured_ = true;
	}
}

void HudMapPassRenderer::clear() {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (rs == nullptr) return;
	for (const RID &item : {base_item_, add_item_, water_item_, top_item_}) {
		if (item.is_valid()) rs->canvas_item_clear(item);
	}
}

void HudMapPassRenderer::release() {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (rs != nullptr) {
		for (const RID &item : {base_item_, add_item_, water_item_, top_item_}) {
			if (item.is_valid()) rs->free_rid(item);
		}
	}
	base_item_ = RID();
	add_item_ = RID();
	water_item_ = RID();
	top_item_ = RID();
	water_sampling_configured_ = false;
	top_sampling_configured_ = false;
}

void HudMapPassRenderer::set_transform(const Transform2D &transform) {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (rs == nullptr) return;
	for (const RID &item : {base_item_, add_item_, water_item_, top_item_}) {
		if (item.is_valid()) rs->canvas_item_set_transform(item, transform);
	}
}

void HudMapPassRenderer::render(const opennova::hud::HudMapPass &p_map,
		const std::vector<opennova::hud::GameFontQuad> &p_map_glyphs,
		const HudMapPassTextures &p_textures,
		const std::vector<opennova::hud::HudMapLine> *p_over_lines,
		const HudMapSegmentsView *p_segments) {
	if (!p_map.visible || !is_ready()) return;
	RenderingServer *rs = RenderingServer::get_singleton();
	const RID base_item = base_item_;
	const RID add_item = add_item_;
	const RID water_item = water_item_;
	// The top layer, in the retail order (sprites, marks, ring, compass, lines,
	// glyphs); a MODULATE2X sprite flags its own vertices.
	const RID top_item = top_item_;
	// The UV.x flag the top material reads as the MODULATE2X colour stage
	// (HudOverlay's kMapModulate2xShader); map UVs stay inside [0, 1].
	constexpr float kModulate2xUvFlag = 8.0f;

	// One triangle-array submission per (item, texture) group: the per-frame
	// map redraw must not request one RenderingServer polygon per triangle
	// (each polygon is its own GPU buffer request — the per-tri form measured
	// in whole milliseconds on missions with dense footprint sets).
	PackedVector2Array points, uvs;
	PackedColorArray colors;
	PackedInt32Array indices;
	const auto flush_tris = [&](const RID &item, const Ref<Texture2D> &tex) {
		if (indices.is_empty()) return;
		rs->canvas_item_add_triangle_array(item, indices, points, colors, uvs,
				PackedInt32Array(), PackedFloat32Array(),
				tex.is_valid() ? tex->get_rid() : RID());
		points.clear();
		uvs.clear();
		colors.clear();
		indices.clear();
	};
	const auto push_map_tri = [&](const opennova::hud::HudMapTri &tri) {
		const int base = static_cast<int>(points.size());
		points.push_back(Vector2(tri.a.x, tri.a.y));
		points.push_back(Vector2(tri.b.x, tri.b.y));
		points.push_back(Vector2(tri.c.x, tri.c.y));
		uvs.push_back(Vector2(tri.a.u, tri.a.v));
		uvs.push_back(Vector2(tri.b.u, tri.b.v));
		uvs.push_back(Vector2(tri.c.u, tri.c.v));
		const Color color = opennova::color_from_argb(tri.color);
		colors.push_back(color);
		colors.push_back(color);
		colors.push_back(color);
		indices.push_back(base);
		indices.push_back(base + 1);
		indices.push_back(base + 2);
	};
	const auto push_quad = [&](const Vector2 *corner, const Vector2 *uv,
			uint32_t argb) {
		const int base = static_cast<int>(points.size());
		const Color color = opennova::color_from_argb(argb);
		for (int i = 0; i < 4; ++i) {
			points.push_back(corner[i]);
			uvs.push_back(uv[i]);
			colors.push_back(color);
		}
		indices.push_back(base);
		indices.push_back(base + 1);
		indices.push_back(base + 2);
		indices.push_back(base);
		indices.push_back(base + 2);
		indices.push_back(base + 3);
	};

	// A pass's own opaque rect clear (the CMAP window) draws first; the
	// spinmap's disc mask is not geometry (hud_minimap.h HudMapPass::clear).
	const Ref<Texture2D> empty_texture;
	for (const opennova::hud::HudMapTri &tri : p_map.clear)
		push_map_tri(tri);
	flush_tris(base_item, empty_texture);

	const Ref<Texture2D> &terrain_texture = p_textures.terrain;
	for (const opennova::hud::HudMapTri &tri : p_map.terrain)
		push_map_tri(tri);
	if (!indices.is_empty()) {
		// Second (additive) half of the x4 output stage — see ensure. The
		// additive item repeats the same arrays.
		rs->canvas_item_add_triangle_array(add_item, indices, points, colors,
				uvs, PackedInt32Array(), PackedFloat32Array(),
				terrain_texture.is_valid() ? terrain_texture->get_rid()
						: RID());
	}
	flush_tris(base_item, terrain_texture);

	// Retail's alpha-tested depthspin draw is opaque and follows the completed
	// terrain output. Canvas splits that output across base/additive items, so
	// the sampled height cutoff rides its own shader item after both; putting
	// it in either terrain item lets the later leg add terrain back over water.
	for (const opennova::hud::HudMapTri &tri : p_map.terrain_water)
		push_map_tri(tri);
	flush_tris(water_item, p_textures.water);

	// The under-layer lines (the 300-wu grid rules) draw FIRST on the top
	// item: retail's grid branch runs before the marker walk, so the
	// buildings-first footprint fills paint OVER the rules (witness at
	// hud_minimap.h HudMapPass::lines_under).
	const auto submit_lines = [&](const std::vector<opennova::hud::HudMapLine>
			&lines) {
		if (lines.empty()) return;
		PackedVector2Array line_points;
		PackedColorArray line_colors;
		line_points.resize(static_cast<int64_t>(lines.size()) * 2);
		line_colors.resize(static_cast<int64_t>(lines.size()));
		int64_t li = 0;
		for (const opennova::hud::HudMapLine &line : lines) {
			if (line.color_end != 0 && line.color_end != line.color) {
				// A two-colour (gouraud) line rides its own two-point polyline.
				PackedVector2Array ends;
				ends.push_back(Vector2(line.x0, line.y0));
				ends.push_back(Vector2(line.x1, line.y1));
				PackedColorArray end_colors;
				end_colors.push_back(opennova::color_from_argb(line.color));
				end_colors.push_back(opennova::color_from_argb(line.color_end));
				rs->canvas_item_add_polyline(top_item, ends, end_colors, 1.0f);
				continue;
			}
			line_points.set(li * 2, Vector2(line.x0, line.y0));
			line_points.set(li * 2 + 1, Vector2(line.x1, line.y1));
			line_colors.set(li, opennova::color_from_argb(line.color));
			++li;
		}
		if (li == 0) return;
		line_points.resize(li * 2);
		line_colors.resize(li);
		rs->canvas_item_add_multiline(top_item, line_points, line_colors, 1.0f);
	};
	submit_lines(p_map.lines_under);

	// Footprint fills draw in the MARKER-WALK slot: retail's building fills
	// blend their ctx alpha over the terrain AFTER the decal's x4 output
	// stage completes, so here they must ride the TOP item — anything on
	// the base item gets the additive child's terrain resubmission summed
	// on top of it (that ordering mistake read near-white; the reference
	// capture measures retail's fill band at ~147..158 per channel over
	// ground, i.e. the 0xD0-alpha gray over the finished doubled terrain).
	// They paint over the grid rules and before the icon sprites, like
	// retail's grid-branch-then-buildings-first walk (witness at
	// world::minimap_footprint_fill_argb).
	for (const opennova::hud::HudMapTri &tri : p_map.overlays)
		push_map_tri(tri);
	flush_tris(top_item, empty_texture);

	// The threat ring's untextured vertex-coloured strips, submitted at the
	// compiler's split: after the radar marks, before the compass sprite
	// (witness at hud_minimap.h HudMapPass::ring_tris).
	const auto submit_ring = [&]() {
		for (const opennova::hud::HudMapColorTri &tri : p_map.ring_tris) {
			const int base = static_cast<int>(points.size());
			for (const opennova::hud::HudMapColorVertex *v : {&tri.a, &tri.b, &tri.c}) {
				points.push_back(Vector2(v->x, v->y));
				uvs.push_back(Vector2());
				colors.push_back(opennova::color_from_argb(v->color));
			}
			indices.push_back(base);
			indices.push_back(base + 1);
			indices.push_back(base + 2);
		}
		flush_tris(top_item, empty_texture);
	};

	// Sprites batch by consecutive texture slot (insertion order is the
	// compiler layer order, so only same-texture runs may merge). `with_ring`
	// submits the pass's ring strips at their split. A MODULATE2X sprite's
	// vertices carry the top material's UV flag.
	const auto submit_sprites = [&](const opennova::hud::HudMapPass &pass, size_t begin,
			size_t end, bool with_ring) {
		int run_texture_slot = -1;
		Ref<Texture2D> run_texture;
		for (size_t sprite_index = begin; sprite_index < end; ++sprite_index) {
			const opennova::hud::HudMapSprite &sprite = pass.sprites[sprite_index];
			if (with_ring && sprite_index == pass.ring_tris_before_sprite &&
					!pass.ring_tris.empty()) {
				flush_tris(top_item, run_texture);
				run_texture_slot = -2;
				submit_ring();
			}
			// kHudMapTextureNone draws untextured (the ring bands).
			const int texture_slot = sprite.texture == opennova::hud::kHudMapTextureNone
					? -1
					: opennova::hud::kHudTexMapIcons + sprite.texture;
			if (texture_slot != run_texture_slot) {
				flush_tris(top_item, run_texture);
				run_texture_slot = texture_slot;
				run_texture = texture_slot >= 0 && texture_slot < p_textures.slot_count &&
								p_textures.slots != nullptr
						? p_textures.slots[texture_slot]
						: Ref<Texture2D>();
			}
			const float u_flag = sprite.modulate2x ? kModulate2xUvFlag : 0.0f;
			if (sprite.geom_count != 0) {
				// The compiler's cropped/banded triangle list replaces the quad.
				const size_t geom_end = std::min<size_t>(pass.geom.size(),
						static_cast<size_t>(sprite.geom_first) + sprite.geom_count);
				for (size_t gi = sprite.geom_first; gi + 3 <= geom_end; gi += 3) {
					const int base = static_cast<int>(points.size());
					for (size_t k = 0; k < 3; ++k) {
						const opennova::hud::HudMapGeomVertex &v = pass.geom[gi + k];
						points.push_back(Vector2(v.x, v.y));
						uvs.push_back(Vector2(v.u + u_flag, v.v));
						colors.push_back(opennova::color_from_argb(v.color));
						indices.push_back(base + static_cast<int>(k));
					}
				}
				continue;
			}
			const float c = std::cos(sprite.rotation_rad);
			const float s = std::sin(sprite.rotation_rad);
			auto corner = [&](float x, float y) {
				return Vector2(sprite.center_x + x * c - y * s,
						sprite.center_y + x * s + y * c);
			};
			const Vector2 corners[4] = {
				corner(-sprite.half_w, -sprite.half_h),
				corner(sprite.half_w, -sprite.half_h),
				corner(sprite.half_w, sprite.half_h),
				corner(-sprite.half_w, sprite.half_h),
			};
			const Vector2 quad_uvs[4] = {
				Vector2(sprite.u0 + u_flag, sprite.v0),
				Vector2(sprite.u1 + u_flag, sprite.v0),
				Vector2(sprite.u1 + u_flag, sprite.v1),
				Vector2(sprite.u0 + u_flag, sprite.v1),
			};
			push_quad(corners, quad_uvs, sprite.color);
		}
		flush_tris(top_item, run_texture);
		if (with_ring && pass.ring_tris_before_sprite >= end && !pass.ring_tris.empty())
			submit_ring();
	};
	submit_sprites(p_map, 0, p_map.sprites.size(), true);

	submit_lines(p_map.lines);

	// Map text: the map passes compile per-pass GameFont glyph quads; they
	// render LAST on this pass top item, above its grid rules and markers.
	// Batched by consecutive font page.
	const auto submit_glyphs = [&](const std::vector<opennova::hud::GameFontQuad> &glyphs,
			size_t begin, size_t end) {
		uint32_t run_page = 0xFFFFFFFFu;
		Ref<Texture2D> run_page_texture;
		for (size_t gi = begin; gi < end && gi < glyphs.size(); ++gi) {
			const opennova::hud::GameFontQuad &glyph = glyphs[gi];
			if (p_textures.pages == nullptr || glyph.page >= p_textures.page_count) continue;
			if (glyph.page != run_page) {
				flush_tris(top_item, run_page_texture);
				run_page = glyph.page;
				run_page_texture = p_textures.pages[glyph.page];
			}
			if (run_page_texture.is_null()) continue;
			const Vector2 corners[4] = {
				Vector2(glyph.x_top_left, glyph.y_top),
				Vector2(glyph.x_top_right, glyph.y_top),
				Vector2(glyph.x_bottom_right, glyph.y_bottom),
				Vector2(glyph.x_bottom_left, glyph.y_bottom),
			};
			const Vector2 quad_uvs[4] = {
				Vector2(glyph.u0, glyph.v0),
				Vector2(glyph.u1, glyph.v0),
				Vector2(glyph.u1, glyph.v1),
				Vector2(glyph.u0, glyph.v1),
			};
			push_quad(corners, quad_uvs, glyph.color);
		}
		flush_tris(top_item, run_page_texture);
	};
	submit_glyphs(p_map_glyphs, 0, p_map_glyphs.size());

	// The DEATH window's zone walk: each zone's blip (its overlays, sprites
	// and lines), then that zone's letters, before the next zone's blip
	// (hud_map_view.h HudMapWindowSegment).
	if (p_segments != nullptr && p_segments->pass != nullptr && p_segments->segments != nullptr &&
			p_segments->glyphs != nullptr && p_segments->glyph_ends != nullptr) {
		const opennova::hud::HudMapPass &zones = *p_segments->pass;
		size_t overlay_begin = 0, sprite_begin = 0, line_begin = 0, glyph_begin = 0;
		const size_t count =
				std::min(p_segments->segments->size(), p_segments->glyph_ends->size());
		std::vector<opennova::hud::HudMapLine> segment_lines;
		for (size_t i = 0; i < count; ++i) {
			const opennova::hud::HudMapWindowSegment &segment = (*p_segments->segments)[i];
			for (size_t oi = overlay_begin;
					oi < segment.overlay_end && oi < zones.overlays.size(); ++oi)
				push_map_tri(zones.overlays[oi]);
			flush_tris(top_item, empty_texture);
			submit_sprites(zones, sprite_begin,
					std::min(segment.sprite_end, zones.sprites.size()), false);
			if (segment.line_end > line_begin && segment.line_end <= zones.lines.size()) {
				segment_lines.assign(zones.lines.begin() + static_cast<std::ptrdiff_t>(line_begin),
						zones.lines.begin() + static_cast<std::ptrdiff_t>(segment.line_end));
				submit_lines(segment_lines);
			}
			const size_t glyph_end = (*p_segments->glyph_ends)[i];
			submit_glyphs(*p_segments->glyphs, glyph_begin, glyph_end);
			overlay_begin = segment.overlay_end;
			sprite_begin = segment.sprite_end;
			line_begin = segment.line_end;
			glyph_begin = glyph_end;
		}
	}

	// The DEATH window's player crosshair draws after its zone letters.
	if (p_over_lines != nullptr) submit_lines(*p_over_lines);
}
