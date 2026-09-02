#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>

#include <cstdint>

namespace godot {

// The compiled menu draw list's element census (MenuFrame::get_draw_list_stats):
// quads (textured ones counted again), lines, glyphs and the widgets that
// drew. A debug/test accessor; to_json_value() for the diagnostic prints.
#define MENU_DRAW_LIST_STATS_FIELDS(X)   \
	X(quads)                             \
	X(quads_textured)                    \
	X(lines)                             \
	X(glyphs)                            \
	X(widgets_drawn)

class MenuDrawListStats : public RefCounted {
	GDCLASS(MenuDrawListStats, RefCounted)

public:
#define MENU_DRAW_LIST_STATS_ACCESSORS(m_name)                 \
	int64_t get_##m_name() const { return m_name##_; }        \
	void set_##m_name(int64_t p_value) { m_name##_ = p_value; }
	MENU_DRAW_LIST_STATS_FIELDS(MENU_DRAW_LIST_STATS_ACCESSORS)
#undef MENU_DRAW_LIST_STATS_ACCESSORS

	Dictionary to_json_value() const;

protected:
	static void _bind_methods();

private:
#define MENU_DRAW_LIST_STATS_MEMBER(m_name) int64_t m_name##_ = 0;
	MENU_DRAW_LIST_STATS_FIELDS(MENU_DRAW_LIST_STATS_MEMBER)
#undef MENU_DRAW_LIST_STATS_MEMBER
};

} // namespace godot
