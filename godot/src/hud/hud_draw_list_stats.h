#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>

#include <cstdint>

namespace godot {

// The compiled HUD draw list's element census (HudOverlay::get_draw_list_stats):
// the primitive counts, the elements-drawn tally, both minimap panes' visible
// flags and their geometry counts, and the canvas sampling the map textures
// were configured with. A debug/test accessor; read-write so a stub HUD
// authors one, to_json_value() for the diagnostic prints.
#define HUD_DRAW_LIST_STATS_FIELDS(X)              \
	X(int64_t, quads, 0)                           \
	X(int64_t, quads_filled, 0)                    \
	X(int64_t, quads_wire, 0)                      \
	X(int64_t, quads_textured, 0)                  \
	X(int64_t, quads_additive, 0)                  \
	X(int64_t, tris, 0)                            \
	X(int64_t, lines, 0)                           \
	X(int64_t, glyphs, 0)                          \
	X(int64_t, underlines, 0)                      \
	X(int64_t, elements_drawn, 0)                  \
	X(bool, map_visible, false)                    \
	X(int64_t, map_backing_tris, 0)                \
	X(int64_t, map_terrain_tris, 0)                \
	X(int64_t, map_footprint_tris, 0)              \
	X(int64_t, map_sprites, 0)                     \
	X(int64_t, map_lines_under, 0)                 \
	X(int64_t, map_lines, 0)                       \
	X(int64_t, map_labels, 0)                      \
	X(bool, big_map_visible, false)                \
	X(int64_t, big_map_backing_tris, 0)            \
	X(int64_t, big_map_terrain_tris, 0)            \
	X(int64_t, big_map_footprint_tris, 0)          \
	X(int64_t, big_map_sprites, 0)                 \
	X(int64_t, big_map_lines_under, 0)             \
	X(int64_t, big_map_lines, 0)                   \
	X(int64_t, big_map_labels, 0)                  \
	X(int64_t, big_map_glyphs, 0)                  \
	X(bool, map_icon_mipmaps, false)               \
	X(int64_t, map_icon_width, 0)                  \
	X(int64_t, map_icon_height, 0)                 \
	X(int64_t, map_texture_filter, 0)              \
	X(int64_t, map_texture_repeat, 0)              \
	X(int64_t, map_water_texture_filter, 0)        \
	X(int64_t, map_water_texture_repeat, 0)        \
	X(int64_t, big_map_texture_filter, 0)          \
	X(int64_t, big_map_texture_repeat, 0)          \
	X(int64_t, big_map_water_texture_filter, 0)    \
	X(int64_t, big_map_water_texture_repeat, 0)

class HudDrawListStats : public RefCounted {
	GDCLASS(HudDrawListStats, RefCounted)

public:
#define HUD_DRAW_LIST_STATS_ACCESSORS(m_type, m_name, m_default)  \
	m_type get_##m_name() const { return m_name##_; }            \
	void set_##m_name(m_type p_value) { m_name##_ = p_value; }
	HUD_DRAW_LIST_STATS_FIELDS(HUD_DRAW_LIST_STATS_ACCESSORS)
#undef HUD_DRAW_LIST_STATS_ACCESSORS

	Dictionary to_json_value() const;

protected:
	static void _bind_methods();

private:
#define HUD_DRAW_LIST_STATS_MEMBER(m_type, m_name, m_default) m_type m_name##_ = m_default;
	HUD_DRAW_LIST_STATS_FIELDS(HUD_DRAW_LIST_STATS_MEMBER)
#undef HUD_DRAW_LIST_STATS_MEMBER
};

} // namespace godot
