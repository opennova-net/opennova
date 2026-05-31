#pragma once

#include <godot_cpp/classes/color_rect.hpp>
#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/variant/color.hpp>

namespace godot {

// Draw a bw-px outline border around a Control using four mouse-transparent
// ColorRects, added as children so the border renders on top of content. Shared
// by the window outline pass (nova_mnu_builder add_outline) and the richer widgets
// (combo popup, table cells/bounds) so the 4-rect recipe lives in one place.
inline void mnu_add_outline(Control *node, const Color &color, int bw = 1) {
	struct Edge {
		const char *name;
		float al, at, ar, ab;
		float ol, ot, orr, ob;
	};
	const Edge edges[] = {
		{ "BorderTop", 0, 0, 1, 0, 0, 0, 0, (float)bw },
		{ "BorderBottom", 0, 1, 1, 1, 0, (float)-bw, 0, 0 },
		{ "BorderLeft", 0, 0, 0, 1, 0, 0, (float)bw, 0 },
		{ "BorderRight", 1, 0, 1, 1, (float)-bw, 0, 0, 0 },
	};
	for (const Edge &e : edges) {
		ColorRect *r = memnew(ColorRect);
		r->set_name(e.name);
		r->set_color(color);
		r->set_anchor(SIDE_LEFT, e.al);
		r->set_anchor(SIDE_TOP, e.at);
		r->set_anchor(SIDE_RIGHT, e.ar);
		r->set_anchor(SIDE_BOTTOM, e.ab);
		r->set_offset(SIDE_LEFT, e.ol);
		r->set_offset(SIDE_TOP, e.ot);
		r->set_offset(SIDE_RIGHT, e.orr);
		r->set_offset(SIDE_BOTTOM, e.ob);
		r->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
		node->add_child(r);
	}
}

} // namespace godot
